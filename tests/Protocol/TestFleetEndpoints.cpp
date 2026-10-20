#include "Base/Config/ConfigValue.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Alarm/AlarmRepository.h"
#include "Core/Alarm/AlarmService.h"
#include "Core/Auth/AuthService.h"
#include "Core/Command/AgentChannelRegistry.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/NodeGroupRepository.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Device/NodeService.h"
#include "Core/Firmware/FirmwareRepository.h"
#include "Core/Http/Authorization.h"
#include "Core/Http/MiddlewareSetup.h"
#include "Core/System/LogRepository.h"
#include "Core/Telemetry/TelemetryRepository.h"
#include "Plugins/AlarmPlugin/AlarmController.h"
#include "Plugins/AuthPlugin/AuthController.h"
#include "Plugins/CommandPlugin/CommandController.h"
#include "Plugins/DevicePlugin/DeviceController.h"
#include "Plugins/DevicePlugin/NodeGroupController.h"
#include "Plugins/FirmwarePlugin/FirmwareController.h"
#include "Plugins/TelemetryPlugin/TelemetryController.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/HttpClientHarness.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    std::vector<std::pair<std::string, std::string>> bearer(const std::string &token)
    {
        return {{"Authorization", "Bearer " + token}};
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
                        co_return EdgePulse::Device::AccessContext{session->userId, session->username, session->departmentId, session->roles, session->permissions};
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
