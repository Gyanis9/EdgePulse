#include "Base/Config/ConfigValue.h"
#include "Base/Database/Database.h"
#include "Base/Runtime/TimeFormat.h"
#include "Base/Security/PasswordHash.h"
#include "Core/Alarm/AlarmRepository.h"
#include "Core/Alarm/AlarmService.h"
#include "Core/Auth/AuthService.h"
#include "Core/Command/AgentChannelRegistry.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/AgentRegistry.h"
#include "Core/Device/NodeGroupRepository.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Device/NodeService.h"
#include "Core/Firmware/FirmwareRepository.h"
#include "Core/Http/AgentWebSocket.h"
#include "Core/Http/Authorization.h"
#include "Core/Http/MiddlewareSetup.h"
#include "Core/Http/WebSocketEndpoints.h"
#include "Core/System/LogRepository.h"
#include "Core/System/TenantRepository.h"
#include "Core/Telemetry/TelemetryRepository.h"
#include "Plugins/AlarmPlugin/AlarmController.h"
#include "Plugins/AuthPlugin/AuthController.h"
#include "Plugins/CommandPlugin/CommandController.h"
#include "Plugins/DevicePlugin/AgentController.h"
#include "Plugins/DevicePlugin/DeviceController.h"
#include "Plugins/DevicePlugin/NodeGroupController.h"
#include "Plugins/FirmwarePlugin/FirmwareController.h"
#include "Plugins/TelemetryPlugin/TelemetryController.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"
#include "TestSupport/HttpClientHarness.h"

#include <Net/Http/Client/HttpClient.h>
#include <Net/WebSocket/WebSocketClient.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    std::vector<std::pair<std::string, std::string>> bearer(const std::string &token)
    {
        return {{"Authorization", "Bearer " + token}};
    }

    struct WebSocketProbeResult
    {
        bool        connected{false}; ///< 升级与 hello 都成功
        std::string commandPayload;   ///< 服务端推送的命令原文
        std::string error;            ///< 失败原因
    };

    AsynGyanis::Core::Task<> performWebSocketProbe(AsynGyanis::Core::EventLoop &loop, std::uint16_t port, std::string token, std::int64_t nodeId,
                                                   std::shared_ptr<std::promise<WebSocketProbeResult>> completion)
    {
        WebSocketProbeResult result;
        try
        {
            AsynGyanis::Net::WebSocketClient::Configuration configuration;
            configuration.hostName      = "127.0.0.1";
            configuration.port          = port;
            configuration.requestTarget = "/api/agent/ws";
            auto connection             = co_await AsynGyanis::Net::WebSocketClient::connect(loop, configuration);
            if (!connection)
            {
                result.error = "升级失败：" + connection.error().message;
                completion->set_value(std::move(result));
                co_return;
            }
            std::unique_ptr<AsynGyanis::Net::WebSocketClient> client = std::move(*connection);

            const AsynGyanis::Base::ConfigValue hello = AsynGyanis::Base::ConfigValue::object({{"type", "hello"}, {"nodeId", nodeId}, {"agentVersion", "protocol-test"}});
            if (!co_await client->sendText(hello.dump()))
            {
                result.error = "hello 发送失败";
                completion->set_value(std::move(result));
                co_return;
            }
            const auto reply = co_await client->receive();
            if (!reply || reply->opCode != AsynGyanis::Net::WebSocketOpCode::Text || reply->payload.find("\"ok\":true") == std::string::npos)
            {
                result.error = "未收到 hello 确认";
                completion->set_value(std::move(result));
                co_return;
            }
            result.connected = true;

            AsynGyanis::Net::HttpClientRequest request;
            request.method      = "POST";
            request.contentType = "application/json";
            request.headers     = bearer(token);
            request.body        = AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"type", "ws-echo"}, {"payload", "{}"}, {"timeoutSeconds", 30}}).dump();
            const auto created  = co_await AsynGyanis::Net::HttpClient::send(loop, "http://127.0.0.1:" + std::to_string(port) + "/api/commands", request, std::chrono::seconds{5});
            if (!created || created->statusCode != 200)
            {
                result.error = "命令创建失败";
                completion->set_value(std::move(result));
                co_return;
            }

            const auto frame = co_await client->receive();
            if (!frame || frame->opCode != AsynGyanis::Net::WebSocketOpCode::Text)
            {
                result.error = "未收到推送的命令";
                completion->set_value(std::move(result));
                co_return;
            }
            result.commandPayload = frame->payload;

            const auto value = AsynGyanis::Base::parseConfigValue(frame->payload);
            if (!value.has_value())
            {
                result.error = "命令推送不是 JSON";
                completion->set_value(std::move(result));
                co_return;
            }
            const auto command = value->find("command");
            if (command == value->end())
            {
                result.error = "命令推送缺少 command";
                completion->set_value(std::move(result));
                co_return;
            }
            const auto identifier = command->find("id");
            if (identifier == command->end() || !identifier->is_number_integer())
            {
                result.error = "命令推送缺少 command.id";
                completion->set_value(std::move(result));
                co_return;
            }
            const AsynGyanis::Base::ConfigValue acknowledgement = AsynGyanis::Base::ConfigValue::object(
                    {{"type", "command.result"}, {"commandId", identifier->get<std::int64_t>()}, {"status", "succeeded"}, {"result", R"({"ok":true})"}, {"message", "ws"}});
            static_cast<void>(co_await client->sendText(acknowledgement.dump()));
            static_cast<void>(co_await client->close(1000, "done"));
        } catch (const std::exception &failure)
        {
            result.error = failure.what();
        }
        completion->set_value(std::move(result));
        co_return;
    }

    struct AuthTimeoutProbeResult
    {
        bool          closed{false}; ///< 收到关闭帧
        std::uint16_t closeCode{0};  ///< 关闭帧状态码
        std::string   error;         ///< 失败原因
    };

    AsynGyanis::Core::Task<> probeUnauthenticatedBrowserSocket(AsynGyanis::Core::EventLoop &loop, std::uint16_t port,
                                                               std::shared_ptr<std::promise<AuthTimeoutProbeResult>> completion)
    {
        AuthTimeoutProbeResult result;
        try
        {
            AsynGyanis::Net::WebSocketClient::Configuration configuration;
            configuration.hostName      = "127.0.0.1";
            configuration.port          = port;
            configuration.requestTarget = "/api/ws";
            auto connection             = co_await AsynGyanis::Net::WebSocketClient::connect(loop, configuration);
            if (!connection)
            {
                result.error = "升级失败：" + connection.error().message;
                completion->set_value(std::move(result));
                co_return;
            }
            std::unique_ptr<AsynGyanis::Net::WebSocketClient> client = std::move(*connection);
            const auto                                        frame  = co_await client->receiveFor(std::chrono::seconds{7});
            if (!frame.has_value())
            {
                result.error = "未在时限内收到关闭帧";
            } else
            {
                result.closed = frame->opCode == AsynGyanis::Net::WebSocketOpCode::Close;
                if (result.closed && frame->payload.size() >= 2)
                {
                    result.closeCode = static_cast<std::uint16_t>((static_cast<std::uint8_t>(frame->payload[0]) << 8) | static_cast<std::uint8_t>(frame->payload[1]));
                }
            }
        } catch (const std::exception &failure)
        {
            result.error = failure.what();
        }
        completion->set_value(std::move(result));
        co_return;
    }
} // namespace

class FleetEndpointsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_context          = std::make_unique<EdgePulse::TestSupport::AuthTestContext>();
        m_storage          = std::filesystem::temp_directory_path() / ("edgepulse-firmware-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        m_nodes            = std::make_shared<EdgePulse::Device::NodeRepository>(m_context->database);
        m_nodeService      = std::make_shared<EdgePulse::Device::NodeService>(m_nodes);
        m_agentRegistry    = std::make_shared<EdgePulse::Device::AgentRegistry>(m_nodes, EdgePulse::Device::AgentRegistry::Options{});
        m_groups           = std::make_shared<EdgePulse::Device::NodeGroupRepository>(m_context->database);
        m_telemetry        = std::make_shared<EdgePulse::Telemetry::TelemetryRepository>(m_context->database);
        m_alarmRepo        = std::make_shared<EdgePulse::Alarm::AlarmRepository>(m_context->database);
        m_alarmService     = std::make_shared<EdgePulse::Alarm::AlarmService>(m_alarmRepo);
        m_commands         = std::make_shared<EdgePulse::Command::CommandRepository>(m_context->database);
        m_channels         = std::make_shared<EdgePulse::Command::AgentChannelRegistry>();
        m_firmware         = std::make_shared<EdgePulse::Firmware::FirmwareRepository>(m_context->database);
        m_browserWebSocket = std::make_shared<EdgePulse::Web::BrowserWebSocket>();

        m_server = std::make_unique<EdgePulse::TestSupport::ServerHarness>(
                [this](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                {
                    EdgePulse::Web::applySecurityHeaders(router);
                    EdgePulse::TestSupport::installTestAgentIdentity(router);
                    EdgePulse::Auth::registerRoutes(router, loop, *m_context->auth, *m_context->captcha, true);
                    const auto access = [this](AsynGyanis::Core::EventLoop  &accessLoop,
                                               AsynGyanis::Net::HttpRequest &request) -> AsynGyanis::Core::Task<std::optional<EdgePulse::Device::AccessContext>>
                    {
                        const auto token = EdgePulse::Web::bearerToken(request);
                        if (!token.has_value())
                        {
                            co_return std::nullopt;
                        }
                        const auto session = co_await m_context->auth->authenticate(accessLoop, *token);
                        if (!session.has_value())
                        {
                            co_return std::nullopt;
                        }
                        co_return EdgePulse::Device::AccessContext{session->userId,       session->tenantId, session->platformAdmin, session->username,
                                                                   session->departmentId, session->roles,    session->permissions};
                    };
                    EdgePulse::Device::registerRoutes(router, loop, *m_nodeService, access, nullptr, m_context->tenants.get());
                    EdgePulse::Device::registerRoutes(router, loop, *m_agentRegistry, *m_nodes, *m_context->tenants);
                    EdgePulse::Device::NodeGroupDependencies groups;
                    groups.auth   = m_context->auth.get();
                    groups.groups = m_groups.get();
                    EdgePulse::Device::registerNodeGroupRoutes(router, loop, groups);

                    EdgePulse::Telemetry::TelemetryDependencies telemetry;
                    telemetry.auth      = m_context->auth.get();
                    telemetry.nodes     = m_nodes.get();
                    telemetry.telemetry = m_telemetry.get();
                    telemetry.alarms    = m_alarmService.get();
                    EdgePulse::Telemetry::registerTelemetryBrowserRoutes(router, loop, telemetry);
                    EdgePulse::Telemetry::registerTelemetryAgentRoutes(router, loop, telemetry);

                    EdgePulse::Alarm::AlarmDependencies alarms;
                    alarms.auth   = m_context->auth.get();
                    alarms.alarms = m_alarmService.get();
                    EdgePulse::Alarm::registerAlarmRoutes(router, loop, alarms);

                    EdgePulse::Command::CommandDependencies commands;
                    commands.auth     = m_context->auth.get();
                    commands.commands = m_commands.get();
                    commands.channels = m_channels.get();
                    commands.nodes    = m_nodes.get();
                    EdgePulse::Command::registerCommandRoutes(router, loop, commands);
                    EdgePulse::Command::registerCommandAgentRoutes(router, loop, commands);
                    EdgePulse::Web::registerAgentWebSocket(router, loop, *m_nodes, *m_commands, *m_channels);
                    // 浏览器端点只用于首条认证超时的用例：令牌校验器从不被调用，未认证连接必须在 5 秒后收到 1008
                    m_browserWebSocket->registerRoute(router, [](std::string_view) -> AsynGyanis::Core::Task<std::optional<EdgePulse::Web::BrowserSubscriptionScope>>
                                                      { co_return std::nullopt; }, {});

                    EdgePulse::Firmware::FirmwareDependencies firmware;
                    firmware.auth               = m_context->auth.get();
                    firmware.firmware           = m_firmware.get();
                    firmware.nodes              = m_nodes.get();
                    firmware.commands           = m_commands.get();
                    firmware.channels           = m_channels.get();
                    firmware.tenants            = m_context->tenants.get();
                    firmware.storageDirectory   = m_storage.string();
                    firmware.maximumUploadBytes = 8U * 1024U * 1024U;
                    EdgePulse::Firmware::registerFirmwareRoutes(router, loop, firmware);
                    EdgePulse::Firmware::registerFirmwareAgentRoutes(router, loop, firmware);
                });
    }

    void TearDown() override
    {
        m_server.reset();
        m_firmware.reset();
        m_commands.reset();
        m_alarmService.reset();
        m_alarmRepo.reset();
        m_telemetry.reset();
        m_groups.reset();
        m_nodeService.reset();
        m_agentRegistry.reset();
        m_nodes.reset();
        m_context.reset();
        std::error_code error;
        std::filesystem::remove_all(m_storage, error);
    }

    std::string loginInTenant(const std::string &username, const std::string &password, const std::string &tenantCode)
    {
        const auto captcha = m_server->get("/api/auth/captcha");
        EXPECT_EQ(200, captcha.status);
        const auto json = captcha.json();
        EXPECT_TRUE(json.has_value());
        const auto response = m_server->post("/api/auth/login", AsynGyanis::Base::ConfigValue::object({{"username", username},
                                                                                                       {"password", password},
                                                                                                       {"captcha", (*json)["data"]["code"]},
                                                                                                       {"captchaUuid", (*json)["data"]["uuid"]},
                                                                                                       {"tenantCode", tenantCode}})
                                                                        .dump());
        EXPECT_EQ(200, response.status) << response.body;
        return (*response.json())["data"]["token"].get<std::string>();
    }

    std::string login()
    {
        const auto captcha = m_server->get("/api/auth/captcha");
        EXPECT_EQ(200, captcha.status);
        const auto json = captcha.json();
        EXPECT_TRUE(json.has_value());
        const auto response = m_server->post(
                "/api/auth/login", AsynGyanis::Base::ConfigValue::object(
                                           {{"username", "admin"}, {"password", "admin123"}, {"captcha", (*json)["data"]["code"]}, {"captchaUuid", (*json)["data"]["uuid"]}})
                                           .dump());
        EXPECT_EQ(200, response.status) << response.body;
        return (*response.json())["data"]["token"].get<std::string>();
    }

    std::int64_t createNode(const std::string &token, const std::string &name)
    {
        const auto response = m_server->post("/api/nodes", AsynGyanis::Base::ConfigValue::object({{"name", name}, {"groupId", 200}}).dump(), "application/json", bearer(token));
        EXPECT_EQ(200, response.status) << response.body;
        return (*response.json())["data"]["id"].get<std::int64_t>();
    }

    static std::vector<std::pair<std::string, std::string>> agentHeaders(const std::string &fingerprint)
    {
        return {{"X-Test-Agent-Fingerprint", fingerprint}};
    }

    void setNodeFingerprint(const std::int64_t nodeId, const std::string &fingerprint)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(m_context->databasePath().string());
        ASSERT_NE(nullptr, connection->execute("UPDATE biz_node SET cert_fingerprint = '" + fingerprint + "' WHERE id = " + std::to_string(nodeId)));
    }

    std::unique_ptr<EdgePulse::TestSupport::AuthTestContext>   m_context;
    EdgePulse::TestSupport::EventLoopRunner                    m_runner;
    std::shared_ptr<EdgePulse::Device::NodeRepository>         m_nodes;
    std::shared_ptr<EdgePulse::Device::NodeService>            m_nodeService;
    std::shared_ptr<EdgePulse::Device::AgentRegistry>          m_agentRegistry;
    std::shared_ptr<EdgePulse::Device::NodeGroupRepository>    m_groups;
    std::shared_ptr<EdgePulse::Telemetry::TelemetryRepository> m_telemetry;
    std::shared_ptr<EdgePulse::Alarm::AlarmRepository>         m_alarmRepo;
    std::shared_ptr<EdgePulse::Alarm::AlarmService>            m_alarmService;
    std::shared_ptr<EdgePulse::Command::CommandRepository>     m_commands;
    std::shared_ptr<EdgePulse::Command::AgentChannelRegistry>  m_channels;
    std::shared_ptr<EdgePulse::Firmware::FirmwareRepository>   m_firmware;
    std::shared_ptr<EdgePulse::Web::BrowserWebSocket>          m_browserWebSocket;
    std::unique_ptr<EdgePulse::TestSupport::ServerHarness>     m_server;
    std::vector<AsynGyanis::Core::Task<>>                      m_probeTasks; ///< WebSocket 探针帧，活到夹具析构
    std::filesystem::path                                      m_storage;
};

// 钉住：遥测上报触发告警，告警列表、确认与规则接口都能读回
TEST_F(FleetEndpointsTest, TelemetryTriggersAndAlarmLifecycle)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "telemetry-node");
    const std::string  fingerprint(64, 'a');
    setNodeFingerprint(nodeId, fingerprint);
    const std::string  body =
            AsynGyanis::Base::ConfigValue::object(
                    {{"nodeId", nodeId},
                     {"samples", AsynGyanis::Base::ConfigValue::array({AsynGyanis::Base::ConfigValue::object({{"metric", "cpu.usage"}, {"value", 96}, {"unit", "%"}})})}})
                    .dump();
    const auto ingest = m_server->post("/api/agent/telemetry", body, "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, ingest.status) << ingest.body;
    EXPECT_EQ(1, (*ingest.json())["data"]["alarms"].size());

    const auto alarms = m_server->get("/api/alarms?pageNum=1&pageSize=10", bearer(token));
    ASSERT_EQ(200, alarms.status) << alarms.body;
    const auto alarmJson = alarms.json();
    ASSERT_TRUE(alarmJson.has_value());
    ASSERT_EQ(1, (*alarmJson)["data"]["rows"].size());
    EXPECT_EQ(1, (*alarmJson)["data"]["openCount"].get<std::int64_t>());
    const std::int64_t alarmId = (*alarmJson)["data"]["rows"][0]["id"].get<std::int64_t>();
    EXPECT_EQ(200, m_server->send("PUT", "/api/alarms/" + std::to_string(alarmId) + "/ack", "{}", "application/json", bearer(token)).status);
    EXPECT_EQ(200, m_server->send("PUT", "/api/alarms/" + std::to_string(alarmId) + "/resolve", "{}", "application/json", bearer(token)).status);

    const auto rules = m_server->get("/api/alarm-rules?pageNum=1&pageSize=10", bearer(token));
    ASSERT_EQ(200, rules.status);
    EXPECT_GE((*rules.json())["data"]["total"].get<std::int64_t>(), 3);
}

// 钉住：命令从浏览器创建、Agent 轮询并回传结果
TEST_F(FleetEndpointsTest, CommandRoundTrip)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "command-node");
    const std::string  fingerprint(64, 'b');
    setNodeFingerprint(nodeId, fingerprint);
    const auto         created =
            m_server->post("/api/commands", AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"type", "reboot"}, {"payload", "{}"}, {"timeoutSeconds", 30}}).dump(),
                           "application/json", bearer(token));
    ASSERT_EQ(200, created.status) << created.body;
    const std::int64_t commandId = (*created.json())["data"]["id"].get<std::int64_t>();

    const auto next = m_server->get("/api/agent/commands/next?nodeId=" + std::to_string(nodeId), agentHeaders(fingerprint));
    ASSERT_EQ(200, next.status) << next.body;
    const auto nextJson = next.json();
    ASSERT_TRUE(nextJson.has_value());
    ASSERT_EQ(commandId, (*nextJson)["data"]["id"].get<std::int64_t>());
    EXPECT_EQ("running", (*nextJson)["data"]["status"].get<std::string>());

    const auto result =
            m_server->post("/api/agent/commands/" + std::to_string(commandId) + "/result",
                           AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"status", "succeeded"}, {"result", R"({"ok":true})"}, {"message", "done"}}).dump(),
                           "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, result.status) << result.body;
    EXPECT_EQ("succeeded", (*result.json())["data"]["status"].get<std::string>());
}

// 钉住：同一个合法 CA 下，没有登记指纹或指纹不符的证书不能冒充节点
TEST_F(FleetEndpointsTest, AgentRequestsRequireCertificateBinding)
{
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                                node = runner.run(m_nodes->create(runner.loop(), "agent-identity", "{}"));
    const std::string                         fingerprint(64, 'a');
    setNodeFingerprint(node.id, fingerprint);

    const std::string heartbeatBody = R"({"nodeId":)" + std::to_string(node.id) + R"(,"agentVersion":"1.0.0"})";
    EXPECT_EQ(403, m_server->post("/api/agent/heartbeat", heartbeatBody).status);
    EXPECT_EQ(403, m_server->post("/api/agent/heartbeat", heartbeatBody, "application/json", agentHeaders(std::string(64, 'b'))).status);
    EXPECT_EQ(200, m_server->post("/api/agent/heartbeat", heartbeatBody, "application/json", agentHeaders(fingerprint)).status);

    // 未登记指纹的节点不接受任何证书，即使证书格式合法
    const auto        unbound     = runner.run(m_nodes->create(runner.loop(), "agent-unbound", "{}"));
    const std::string unboundBody = R"({"nodeId":)" + std::to_string(unbound.id) + R"(,"agentVersion":"1.0.0"})";
    EXPECT_EQ(403, m_server->post("/api/agent/heartbeat", unboundBody, "application/json", agentHeaders(std::string(64, 'f'))).status);
}

// 钉住：Agent 走 WebSocket 时 hello 注册成功，命令由长连接推送、结果回写为 succeeded
TEST_F(FleetEndpointsTest, CommandOverAgentWebSocket)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "ws-command-node");

    auto                              promise = std::make_shared<std::promise<WebSocketProbeResult>>();
    std::future<WebSocketProbeResult> future  = promise->get_future();
    AsynGyanis::Core::Task<>          probe   = performWebSocketProbe(m_server->loop(), m_server->port(), token, nodeId, promise);
    m_server->loop().scheduler().scheduleRemote(probe.handle());
    m_probeTasks.push_back(std::move(probe));

    ASSERT_EQ(std::future_status::ready, future.wait_for(std::chrono::seconds{10})) << "WebSocket 探针未在时限内完成";
    const WebSocketProbeResult result = future.get();
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_TRUE(result.connected);
    EXPECT_NE(std::string::npos, result.commandPayload.find("\"type\":\"command\""));

    std::string status;
    for (int attempt = 0; attempt < 100 && status != "succeeded"; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        const auto list = m_server->get("/api/commands?pageNum=1&pageSize=10&nodeId=" + std::to_string(nodeId), bearer(token));
        ASSERT_EQ(200, list.status) << list.body;
        const auto json = list.json();
        ASSERT_TRUE(json.has_value());
        if ((*json)["data"]["rows"].empty())
        {
            continue;
        }
        status = (*json)["data"]["rows"][0]["status"].get<std::string>();
    }
    EXPECT_EQ("succeeded", status);
}

// 钉住：浏览器连接 5 秒内不认证即被 1008 收口，且不依赖额外的计时线程
TEST_F(FleetEndpointsTest, BrowserWebSocketClosesUnauthenticatedSession)
{
    auto                                promise = std::make_shared<std::promise<AuthTimeoutProbeResult>>();
    std::future<AuthTimeoutProbeResult> future  = promise->get_future();
    AsynGyanis::Core::Task<>            probe   = probeUnauthenticatedBrowserSocket(m_server->loop(), m_server->port(), promise);
    m_server->loop().scheduler().scheduleRemote(probe.handle());
    m_probeTasks.push_back(std::move(probe));

    ASSERT_EQ(std::future_status::ready, future.wait_for(std::chrono::seconds{10})) << "未认证连接未在时限内收口";
    const AuthTimeoutProbeResult result = future.get();
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_TRUE(result.closed);
    EXPECT_EQ(1008, result.closeCode);
}

// 钉住：固件流式上传、Range 下载、ETag 304 与下发任务
TEST_F(FleetEndpointsTest, FirmwareUploadRangeAndDeploy)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "firmware-node");
    const std::string  fingerprint(64, 'c');
    setNodeFingerprint(nodeId, fingerprint);
    std::string        payload(1024 * 1024, 'E');
    const auto         upload = m_server->send("POST", "/api/firmware?name=EdgePulse+OS&version=1.0.0&fileName=os.bin", payload, "application/octet-stream", bearer(token));
    ASSERT_EQ(200, upload.status) << upload.body;
    const auto uploadJson = upload.json();
    ASSERT_TRUE(uploadJson.has_value());
    const std::int64_t firmwareId = (*uploadJson)["data"]["id"].get<std::int64_t>();
    const std::string  sha256     = (*uploadJson)["data"]["sha256"].get<std::string>();
    EXPECT_EQ(1024 * 1024, (*uploadJson)["data"]["sizeBytes"].get<std::int64_t>());

    std::vector<std::pair<std::string, std::string>> rangeHeaders = agentHeaders(fingerprint);
    rangeHeaders.emplace_back("Range", "bytes=10-19");
    const auto range = m_server->get("/api/agent/firmware/" + std::to_string(firmwareId) + "/download?nodeId=" + std::to_string(nodeId), rangeHeaders);
    ASSERT_EQ(206, range.status) << range.body;
    EXPECT_EQ(std::string(10, 'E'), range.body);
    EXPECT_EQ("bytes 10-19/1048576", range.header("content-range"));
    std::vector<std::pair<std::string, std::string>> cachedHeaders = agentHeaders(fingerprint);
    cachedHeaders.emplace_back("If-None-Match", "\"" + sha256 + "\"");
    const auto cached = m_server->get("/api/agent/firmware/" + std::to_string(firmwareId) + "/download?nodeId=" + std::to_string(nodeId), cachedHeaders);
    EXPECT_EQ(304, cached.status);

    const auto deployed =
            m_server->post("/api/firmware/" + std::to_string(firmwareId) + "/deploy",
                           AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeId})}}).dump(), "application/json", bearer(token));
    ASSERT_EQ(200, deployed.status) << deployed.body;
    ASSERT_EQ(1, (*deployed.json())["data"].size());
    const std::int64_t taskId    = (*deployed.json())["data"][0]["taskId"].get<std::int64_t>();
    const std::int64_t commandId = (*deployed.json())["data"][0]["commandId"].get<std::int64_t>();
    const auto         commands  = m_server->get("/api/commands?pageNum=1&pageSize=10&nodeId=" + std::to_string(nodeId), bearer(token));
    ASSERT_EQ(200, commands.status) << commands.body;
    const auto commandsJson = commands.json();
    ASSERT_TRUE(commandsJson.has_value());
    const auto &rows    = (*commandsJson)["data"]["rows"];
    const auto  created = std::find_if(rows.begin(), rows.end(), [commandId](const AsynGyanis::Base::ConfigValue &row) { return row["id"].get<std::int64_t>() == commandId; });
    ASSERT_NE(rows.end(), created) << commands.body;
    // 下发的下载地址必须带上目标节点，否则真实 Agent 按它请求会得到 400
    const auto downloadPayload = AsynGyanis::Base::parseConfigValue((*created)["payload"].get<std::string>());
    ASSERT_TRUE(downloadPayload.has_value());
    EXPECT_EQ("/api/agent/firmware/" + std::to_string(firmwareId) + "/download?nodeId=" + std::to_string(nodeId), (*downloadPayload)["downloadPath"].get<std::string>());
    const auto         status = m_server->post("/api/agent/firmware/" + std::to_string(firmwareId) + "/status",
                                               AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"status", "installed"}, {"progress", 100}, {"message", "ok"}}).dump(),
                                               "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, status.status) << status.body;
    EXPECT_EQ(taskId, (*status.json())["data"]["id"].get<std::int64_t>());

    const auto tasks = m_server->get("/api/firmware/" + std::to_string(firmwareId) + "/tasks", bearer(token));
    ASSERT_EQ(200, tasks.status);
    EXPECT_EQ("installed", (*tasks.json())["data"][0]["status"].get<std::string>());
}

// 钉住：节点分组 CRUD 与占用保护
TEST_F(FleetEndpointsTest, NodeGroupCrud)
{
    const std::string token = login();
    const auto created      = m_server->post("/api/node-groups", AsynGyanis::Base::ConfigValue::object({{"name", "华东"}, {"sort", 5}}).dump(), "application/json", bearer(token));
    ASSERT_EQ(200, created.status) << created.body;
    const std::int64_t groupId = (*created.json())["data"]["id"].get<std::int64_t>();
    createNode(token, "grouped-node");
    const auto tree = m_server->get("/api/node-groups", bearer(token));
    ASSERT_EQ(200, tree.status);
    EXPECT_GE((*tree.json())["data"].size(), 2U);
    EXPECT_EQ(200, m_server->send("DELETE", "/api/node-groups/" + std::to_string(groupId), "", "", bearer(token)).status);
}

// 钉住：Agent 侧没有会话，命令与固件任务必须按节点/固件自身的租户查找，不能落在默认租户
TEST_F(FleetEndpointsTest, AgentRoutesResolveTenantFromNodeAndFirmware)
{
    EdgePulse::System::TenantProvisionRequest request;
    request.code              = "fleet_tenant";
    request.name              = "设备租户";
    request.adminUsername     = "fleet-admin";
    request.adminPasswordHash = EdgePulse::Common::hashPassword("fleet-pass-1");
    const auto provisioned    = m_runner.run(m_context->tenants->provision(m_runner.loop(), std::move(request), 1));
    ASSERT_TRUE(provisioned.has_value()) << provisioned.error();
    const std::int64_t tenantId = provisioned->tenantId;

    const auto tenantNodes    = m_nodes->scoped({tenantId, false});
    const auto node           = m_runner.run(tenantNodes->create(m_runner.loop(), "tenant-b-node", "{}"));
    const std::string fingerprint(64, 'd');
    setNodeFingerprint(node.id, fingerprint);
    const auto tenantCommands = m_commands->scoped({tenantId, false});
    const auto command        = m_runner.run(tenantCommands->create(m_runner.loop(), node.id, "reboot", "{}", 1, 60));
    ASSERT_TRUE(command.has_value());

    const auto next = m_server->get("/api/agent/commands/next?nodeId=" + std::to_string(node.id), agentHeaders(fingerprint));
    ASSERT_EQ(200, next.status) << next.body;
    ASSERT_TRUE((*next.json())["data"].is_object()) << next.body;
    EXPECT_EQ(command->id, (*next.json())["data"]["id"].get<std::int64_t>());

    const auto result = m_server->post("/api/agent/commands/" + std::to_string(command->id) + "/result",
                                       AsynGyanis::Base::ConfigValue::object({{"nodeId", node.id}, {"status", "succeeded"}, {"result", "{}"}}).dump(),
                                       "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, result.status) << result.body;
    EXPECT_EQ("succeeded", (*result.json())["data"]["status"].get<std::string>());

    EdgePulse::Firmware::FirmwareRecord firmware;
    firmware.name                     = "Tenant OS";
    firmware.version                  = "2.0.0";
    firmware.fileName                 = "tenant-os.bin";
    firmware.contentType              = "application/octet-stream";
    firmware.sha256                   = std::string(64, 'b');
    firmware.storagePath              = (m_storage / "tenant-os.bin").string();
    const auto         tenantFirmware = m_firmware->scoped({tenantId, false});
    const std::int64_t firmwareId     = m_runner.run(tenantFirmware->create(m_runner.loop(), firmware));
    ASSERT_GT(firmwareId, 0);
    const std::int64_t taskId = m_runner.run(tenantFirmware->createTask(m_runner.loop(), firmwareId, node.id));
    ASSERT_GT(taskId, 0);
    const auto status = m_server->post("/api/agent/firmware/" + std::to_string(firmwareId) + "/status",
                                       AsynGyanis::Base::ConfigValue::object({{"nodeId", node.id}, {"status", "installed"}, {"progress", 100}}).dump(),
                                       "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, status.status) << status.body;
    EXPECT_EQ(taskId, (*status.json())["data"]["id"].get<std::int64_t>());

    // 默认租户的节点不能借自己的合法证书取走别家租户的固件
    const auto        defaultNode = m_runner.run(m_nodes->create(m_runner.loop(), "tenant-a-node", "{}"));
    const std::string defaultFingerprint(64, '5');
    setNodeFingerprint(defaultNode.id, defaultFingerprint);
    const auto crossTenant = m_server->get("/api/agent/firmware/" + std::to_string(firmwareId) + "/download?nodeId=" + std::to_string(defaultNode.id),
                                           agentHeaders(defaultFingerprint));
    EXPECT_EQ(403, crossTenant.status) << crossTenant.body;
}

// 钉住：批量命令共享批次号；计划时间未到时 Agent 取不到，到点后可取
TEST_F(FleetEndpointsTest, BatchAndScheduledCommands)
{
    const std::string  token  = login();
    const std::int64_t nodeA  = createNode(token, "batch-node-a");
    const std::int64_t nodeB  = createNode(token, "batch-node-b");
    const std::string  fingerprintA(64, 'e');
    const std::string  fingerprintB(64, '1');
    setNodeFingerprint(nodeA, fingerprintA);
    setNodeFingerprint(nodeB, fingerprintB);
    const auto         now    = std::chrono::system_clock::now();
    const std::string  future = EdgePulse::Common::formatDateTimeUtc(now + std::chrono::hours{1});
    const std::string  past   = EdgePulse::Common::formatDateTimeUtc(now - std::chrono::minutes{1});

    const auto scheduled =
            m_server->post("/api/commands/batch",
                           AsynGyanis::Base::ConfigValue::object(
                                   {{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeA, nodeB})}, {"type", "diagnostics.collect"}, {"payload", "{}"}, {"scheduledAt", future}})
                                   .dump(),
                           "application/json", bearer(token));
    ASSERT_EQ(200, scheduled.status) << scheduled.body;
    const auto scheduledJson = scheduled.json();
    ASSERT_TRUE(scheduledJson.has_value());
    ASSERT_EQ(2, (*scheduledJson)["data"]["commands"].size());
    const std::int64_t batchId = (*scheduledJson)["data"]["batchId"].get<std::int64_t>();
    EXPECT_EQ(batchId, (*scheduledJson)["data"]["commands"][0]["batchId"].get<std::int64_t>());
    EXPECT_EQ(future, (*scheduledJson)["data"]["commands"][0]["scheduledAt"].get<std::string>());
    const auto early = m_server->get("/api/agent/commands/next?nodeId=" + std::to_string(nodeA), agentHeaders(fingerprintA));
    ASSERT_EQ(200, early.status) << early.body;
    EXPECT_TRUE((*early.json())["data"].is_null());
    const auto canceled = m_server->send("PUT", "/api/commands/batch/" + std::to_string(batchId) + "/cancel", "{}", "application/json", bearer(token));
    ASSERT_EQ(200, canceled.status) << canceled.body;
    ASSERT_EQ(2, (*canceled.json())["data"].size());
    EXPECT_EQ("canceled", (*canceled.json())["data"][0]["status"].get<std::string>());
    const auto afterCancel = m_server->get("/api/agent/commands/next?nodeId=" + std::to_string(nodeA), agentHeaders(fingerprintA));
    ASSERT_EQ(200, afterCancel.status) << afterCancel.body;
    EXPECT_TRUE((*afterCancel.json())["data"].is_null());

    const auto due = m_server->post(
            "/api/commands/batch",
            AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeA})}, {"type", "reboot"}, {"payload", "{}"}, {"scheduledAt", past}})
                    .dump(),
            "application/json", bearer(token));
    ASSERT_EQ(200, due.status) << due.body;
    const auto next = m_server->get("/api/agent/commands/next?nodeId=" + std::to_string(nodeA), agentHeaders(fingerprintA));
    ASSERT_EQ(200, next.status) << next.body;
    ASSERT_TRUE((*next.json())["data"].is_object()) << next.body;
    EXPECT_EQ("reboot", (*next.json())["data"]["type"].get<std::string>());
}

// 钉住：灰度下发按批次返回剩余节点，回滚把节点切回上一版本
TEST_F(FleetEndpointsTest, FirmwareCanaryAndRollback)
{
    const std::string  token = login();
    const std::int64_t nodeA = createNode(token, "canary-node-a");
    const std::int64_t nodeB = createNode(token, "canary-node-b");
    const std::string  fingerprintA(64, '2');
    const std::string  fingerprintB(64, '3');
    setNodeFingerprint(nodeA, fingerprintA);
    setNodeFingerprint(nodeB, fingerprintB);
    const std::string bytes(1024, 'F');

    const auto uploadFirst = m_server->send("POST", "/api/firmware?name=Canary+OS&version=1.0.0&fileName=a.bin", bytes, "application/octet-stream", bearer(token));
    ASSERT_EQ(200, uploadFirst.status) << uploadFirst.body;
    const std::int64_t firstId = (*uploadFirst.json())["data"]["id"].get<std::int64_t>();
    const auto         deployFirst =
            m_server->post("/api/firmware/" + std::to_string(firstId) + "/deploy",
                           AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeA, nodeB})}}).dump(), "application/json", bearer(token));
    ASSERT_EQ(200, deployFirst.status) << deployFirst.body;
    for (const auto &[nodeId, fingerprint]: {std::pair{nodeA, fingerprintA}, std::pair{nodeB, fingerprintB}})
    {
        ASSERT_EQ(200, m_server->post("/api/agent/firmware/" + std::to_string(firstId) + "/status",
                                      AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"status", "installed"}, {"progress", 100}}).dump(),
                                      "application/json", agentHeaders(fingerprint))
                               .status);
    }

    const auto uploadSecond = m_server->send("POST", "/api/firmware?name=Canary+OS&version=2.0.0&fileName=b.bin", bytes, "application/octet-stream", bearer(token));
    ASSERT_EQ(200, uploadSecond.status) << uploadSecond.body;
    const std::int64_t secondId = (*uploadSecond.json())["data"]["id"].get<std::int64_t>();
    const auto         canary = m_server->post("/api/firmware/" + std::to_string(secondId) + "/deploy",
                                               AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeA, nodeB})}, {"batchSize", 1}}).dump(),
                                               "application/json", bearer(token));
    ASSERT_EQ(200, canary.status) << canary.body;
    const auto canaryJson = canary.json();
    ASSERT_TRUE(canaryJson.has_value());
    ASSERT_EQ(1, (*canaryJson)["data"]["deployed"].size());
    ASSERT_EQ(1, (*canaryJson)["data"]["deferredNodeIds"].size());
    EXPECT_EQ(nodeB, (*canaryJson)["data"]["deferredNodeIds"][0].get<std::int64_t>());
    ASSERT_EQ(200, m_server->post("/api/agent/firmware/" + std::to_string(secondId) + "/status",
                                  AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeA}, {"status", "installed"}, {"progress", 100}}).dump(),
                                  "application/json", agentHeaders(fingerprintA))
                           .status);

    const auto rollback =
            m_server->post("/api/firmware/" + std::to_string(secondId) + "/rollback",
                           AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeA})}}).dump(), "application/json", bearer(token));
    ASSERT_EQ(200, rollback.status) << rollback.body;
    const auto rollbackJson = rollback.json();
    ASSERT_TRUE(rollbackJson.has_value());
    ASSERT_EQ(1, (*rollbackJson)["data"].size());
    EXPECT_EQ(firstId, (*rollbackJson)["data"][0]["toFirmwareId"].get<std::int64_t>());
}

// 钉住：平台管理员可把节点与固件建到指定租户，租户管理员只看到自己的
TEST_F(FleetEndpointsTest, PlatformAdminProvisionsTenantNodesAndFirmware)
{
    EdgePulse::System::TenantProvisionRequest request;
    request.code              = "fleet_b";
    request.name              = "设备租户B";
    request.adminUsername     = "fleet-b-admin";
    request.adminPasswordHash = EdgePulse::Common::hashPassword("fleet-b-pass");
    const auto provisioned    = m_runner.run(m_context->tenants->provision(m_runner.loop(), std::move(request), 1));
    ASSERT_TRUE(provisioned.has_value()) << provisioned.error();
    const std::int64_t tenantId = provisioned->tenantId;

    const std::string platformToken = login();
    const auto createdNode = m_server->post("/api/nodes", AsynGyanis::Base::ConfigValue::object({{"name", "tenant-b-node"}, {"groupId", 200}, {"tenantId", tenantId}}).dump(),
                                            "application/json", bearer(platformToken));
    ASSERT_EQ(200, createdNode.status) << createdNode.body;
    EXPECT_EQ(tenantId, (*createdNode.json())["data"]["tenantId"].get<std::int64_t>());
    createNode(platformToken, "default-tenant-node");

    const auto badTenant = m_server->post("/api/nodes", AsynGyanis::Base::ConfigValue::object({{"name", "bad-tenant-node"}, {"tenantId", 99999999}}).dump(), "application/json",
                                          bearer(platformToken));
    EXPECT_EQ(400, badTenant.status);

    const std::string tenantToken = loginInTenant("fleet-b-admin", "fleet-b-pass", "fleet_b");
    const auto        tenantNodes = m_server->get("/api/nodes?pageNum=1&pageSize=10&keyword=tenant-b-node", bearer(tenantToken));
    ASSERT_EQ(200, tenantNodes.status) << tenantNodes.body;
    EXPECT_EQ(1, (*tenantNodes.json())["data"]["total"].get<std::int64_t>());
    const auto hidden = m_server->get("/api/nodes?pageNum=1&pageSize=10&keyword=default-tenant-node", bearer(tenantToken));
    ASSERT_EQ(200, hidden.status) << hidden.body;
    EXPECT_EQ(0, (*hidden.json())["data"]["total"].get<std::int64_t>());

    const std::string bytes(512, 'T');
    const auto upload = m_server->send("POST", "/api/firmware?name=Tenant+OS&version=3.0.0&fileName=t.bin&tenantId=" + std::to_string(tenantId), bytes, "application/octet-stream",
                                       bearer(platformToken));
    ASSERT_EQ(200, upload.status) << upload.body;
    EXPECT_EQ(tenantId, (*upload.json())["data"]["tenantId"].get<std::int64_t>());
    const auto tenantFirmware = m_server->get("/api/firmware?pageNum=1&pageSize=10&keyword=Tenant", bearer(tenantToken));
    ASSERT_EQ(200, tenantFirmware.status) << tenantFirmware.body;
    EXPECT_EQ(1, (*tenantFirmware.json())["data"]["total"].get<std::int64_t>());
}
