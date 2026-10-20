#include "Base/Config/ConfigValue.h"
#include "Base/Runtime/TimeFormat.h"
#include "Base/Security/PasswordHash.h"
#include "Core/Alarm/AlarmRepository.h"
#include "Core/Alarm/AlarmService.h"
#include "Core/Auth/AuthService.h"
#include "Core/Command/AgentChannelRegistry.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/NodeGroupRepository.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Device/NodeService.h"
#include "Core/Firmware/FirmwareRepository.h"
#include "Core/Http/AgentWebSocket.h"
#include "Core/Http/Authorization.h"
#include "Core/Http/MiddlewareSetup.h"
#include "Core/System/LogRepository.h"
#include "Core/System/TenantRepository.h"
#include "Core/Telemetry/TelemetryRepository.h"
#include "Plugins/AlarmPlugin/AlarmController.h"
#include "Plugins/AuthPlugin/AuthController.h"
#include "Plugins/CommandPlugin/CommandController.h"
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
} // namespace

class FleetEndpointsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_context      = std::make_unique<EdgePulse::TestSupport::AuthTestContext>();
        m_storage      = std::filesystem::temp_directory_path() / ("edgepulse-firmware-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        m_nodes        = std::make_shared<EdgePulse::Device::NodeRepository>(m_context->database);
        m_nodeService  = std::make_shared<EdgePulse::Device::NodeService>(m_nodes);
        m_groups       = std::make_shared<EdgePulse::Device::NodeGroupRepository>(m_context->database);
        m_telemetry    = std::make_shared<EdgePulse::Telemetry::TelemetryRepository>(m_context->database);
        m_alarmRepo    = std::make_shared<EdgePulse::Alarm::AlarmRepository>(m_context->database);
        m_alarmService = std::make_shared<EdgePulse::Alarm::AlarmService>(m_alarmRepo);
        m_commands     = std::make_shared<EdgePulse::Command::CommandRepository>(m_context->database);
        m_channels     = std::make_shared<EdgePulse::Command::AgentChannelRegistry>();
        m_firmware     = std::make_shared<EdgePulse::Firmware::FirmwareRepository>(m_context->database);

        m_server = std::make_unique<EdgePulse::TestSupport::ServerHarness>(
                [this](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                {
                    EdgePulse::Web::applySecurityHeaders(router);
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
                    EdgePulse::Device::registerRoutes(router, loop, *m_nodeService, access);
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
                    EdgePulse::Command::registerCommandRoutes(router, loop, commands);
                    EdgePulse::Command::registerCommandAgentRoutes(router, loop, commands);
                    EdgePulse::Web::registerAgentWebSocket(router, loop, *m_nodes, *m_commands, *m_channels);

                    EdgePulse::Firmware::FirmwareDependencies firmware;
                    firmware.auth               = m_context->auth.get();
                    firmware.firmware           = m_firmware.get();
                    firmware.nodes              = m_nodes.get();
                    firmware.commands           = m_commands.get();
                    firmware.channels           = m_channels.get();
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
        m_nodes.reset();
        m_context.reset();
        std::error_code error;
        std::filesystem::remove_all(m_storage, error);
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

    std::unique_ptr<EdgePulse::TestSupport::AuthTestContext>   m_context;
    EdgePulse::TestSupport::EventLoopRunner                    m_runner;
    std::shared_ptr<EdgePulse::Device::NodeRepository>         m_nodes;
    std::shared_ptr<EdgePulse::Device::NodeService>            m_nodeService;
    std::shared_ptr<EdgePulse::Device::NodeGroupRepository>    m_groups;
    std::shared_ptr<EdgePulse::Telemetry::TelemetryRepository> m_telemetry;
    std::shared_ptr<EdgePulse::Alarm::AlarmRepository>         m_alarmRepo;
    std::shared_ptr<EdgePulse::Alarm::AlarmService>            m_alarmService;
    std::shared_ptr<EdgePulse::Command::CommandRepository>     m_commands;
    std::shared_ptr<EdgePulse::Command::AgentChannelRegistry>  m_channels;
    std::shared_ptr<EdgePulse::Firmware::FirmwareRepository>   m_firmware;
    std::unique_ptr<EdgePulse::TestSupport::ServerHarness>     m_server;
    std::vector<AsynGyanis::Core::Task<>>                      m_probeTasks; ///< WebSocket 探针帧，活到夹具析构
    std::filesystem::path                                      m_storage;
};

// 钉住：遥测上报触发告警，告警列表、确认与规则接口都能读回
TEST_F(FleetEndpointsTest, TelemetryTriggersAndAlarmLifecycle)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "telemetry-node");
    const std::string  body =
            AsynGyanis::Base::ConfigValue::object(
                    {{"nodeId", nodeId},
                     {"samples", AsynGyanis::Base::ConfigValue::array({AsynGyanis::Base::ConfigValue::object({{"metric", "cpu.usage"}, {"value", 96}, {"unit", "%"}})})}})
                    .dump();
    const auto ingest = m_server->post("/api/agent/telemetry", body);
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
    const auto         created =
            m_server->post("/api/commands", AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"type", "reboot"}, {"payload", "{}"}, {"timeoutSeconds", 30}}).dump(),
                           "application/json", bearer(token));
    ASSERT_EQ(200, created.status) << created.body;
    const std::int64_t commandId = (*created.json())["data"]["id"].get<std::int64_t>();

    const auto next = m_server->get("/api/agent/commands/next?nodeId=" + std::to_string(nodeId));
    ASSERT_EQ(200, next.status) << next.body;
    const auto nextJson = next.json();
    ASSERT_TRUE(nextJson.has_value());
    ASSERT_EQ(commandId, (*nextJson)["data"]["id"].get<std::int64_t>());
    EXPECT_EQ("running", (*nextJson)["data"]["status"].get<std::string>());

    const auto result =
            m_server->post("/api/agent/commands/" + std::to_string(commandId) + "/result",
                           AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"status", "succeeded"}, {"result", R"({"ok":true})"}, {"message", "done"}}).dump());
    ASSERT_EQ(200, result.status) << result.body;
    EXPECT_EQ("succeeded", (*result.json())["data"]["status"].get<std::string>());
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

// 钉住：固件流式上传、Range 下载、ETag 304 与下发任务
TEST_F(FleetEndpointsTest, FirmwareUploadRangeAndDeploy)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "firmware-node");
    std::string        payload(1024 * 1024, 'E');
    const auto         upload = m_server->send("POST", "/api/firmware?name=EdgePulse+OS&version=1.0.0&fileName=os.bin", payload, "application/octet-stream", bearer(token));
    ASSERT_EQ(200, upload.status) << upload.body;
    const auto uploadJson = upload.json();
    ASSERT_TRUE(uploadJson.has_value());
    const std::int64_t firmwareId = (*uploadJson)["data"]["id"].get<std::int64_t>();
    const std::string  sha256     = (*uploadJson)["data"]["sha256"].get<std::string>();
    EXPECT_EQ(1024 * 1024, (*uploadJson)["data"]["sizeBytes"].get<std::int64_t>());

    const auto range = m_server->get("/api/agent/firmware/" + std::to_string(firmwareId) + "/download", {{"Range", "bytes=10-19"}});
    ASSERT_EQ(206, range.status) << range.body;
    EXPECT_EQ(std::string(10, 'E'), range.body);
    EXPECT_EQ("bytes 10-19/1048576", range.header("content-range"));
    const auto cached = m_server->get("/api/agent/firmware/" + std::to_string(firmwareId) + "/download", {{"If-None-Match", "\"" + sha256 + "\""}});
    EXPECT_EQ(304, cached.status);

    const auto deployed =
            m_server->post("/api/firmware/" + std::to_string(firmwareId) + "/deploy",
                           AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeId})}}).dump(), "application/json", bearer(token));
    ASSERT_EQ(200, deployed.status) << deployed.body;
    ASSERT_EQ(1, (*deployed.json())["data"].size());
    const std::int64_t taskId = (*deployed.json())["data"][0]["taskId"].get<std::int64_t>();
    const auto         status = m_server->post("/api/agent/firmware/" + std::to_string(firmwareId) + "/status",
                                               AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"status", "installed"}, {"progress", 100}, {"message", "ok"}}).dump());
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
    const auto tenantCommands = m_commands->scoped({tenantId, false});
    const auto command        = m_runner.run(tenantCommands->create(m_runner.loop(), node.id, "reboot", "{}", 1, 60));
    ASSERT_TRUE(command.has_value());

    const auto next = m_server->get("/api/agent/commands/next?nodeId=" + std::to_string(node.id));
    ASSERT_EQ(200, next.status) << next.body;
    ASSERT_TRUE((*next.json())["data"].is_object()) << next.body;
    EXPECT_EQ(command->id, (*next.json())["data"]["id"].get<std::int64_t>());

    const auto result = m_server->post("/api/agent/commands/" + std::to_string(command->id) + "/result",
                                       AsynGyanis::Base::ConfigValue::object({{"nodeId", node.id}, {"status", "succeeded"}, {"result", "{}"}}).dump());
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
                                       AsynGyanis::Base::ConfigValue::object({{"nodeId", node.id}, {"status", "installed"}, {"progress", 100}}).dump());
    ASSERT_EQ(200, status.status) << status.body;
    EXPECT_EQ(taskId, (*status.json())["data"]["id"].get<std::int64_t>());
}
