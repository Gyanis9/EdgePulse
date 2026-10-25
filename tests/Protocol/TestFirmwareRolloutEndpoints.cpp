#include "Base/Config/ConfigValue.h"
#include "Core/Auth/ApiTokenService.h"
#include "Core/Auth/AuthService.h"
#include "Core/Alarm/AlarmRepository.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Firmware/FirmwareRepository.h"
#include "Core/Firmware/FirmwareRolloutRepository.h"
#include "Core/Firmware/FirmwareRolloutService.h"
#include "Core/Firmware/FirmwareSigningPolicy.h"
#include "Core/Http/MiddlewareSetup.h"
#include "Core/System/TenantRepository.h"
#include "Core/System/UserRepository.h"
#include "Core/Telemetry/TelemetryRepository.h"
#include "Plugins/AuthPlugin/AuthController.h"
#include "Plugins/FirmwarePlugin/FirmwareController.h"
#include "Plugins/SystemPlugin/ApiTokenController.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"
#include "TestSupport/HttpClientHarness.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using AsynGyanis::Base::ConfigValue;
    using EdgePulse::TestSupport::HttpResult;

    std::vector<std::pair<std::string, std::string>> bearer(const std::string &token)
    {
        return {{"Authorization", "Bearer " + token}};
    }

    class FirmwareRolloutEndpointsTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_context   = std::make_shared<EdgePulse::TestSupport::AuthTestContext>();
            m_runner    = std::make_shared<EdgePulse::TestSupport::EventLoopRunner>();
            m_firmware  = std::make_shared<EdgePulse::Firmware::FirmwareRepository>(m_context->database);
            m_rollouts  = std::make_shared<EdgePulse::Firmware::FirmwareRolloutRepository>(m_context->database);
            m_commands  = std::make_shared<EdgePulse::Command::CommandRepository>(m_context->database);
            m_nodes     = std::make_shared<EdgePulse::Device::NodeRepository>(m_context->database);
            m_telemetry = std::make_shared<EdgePulse::Telemetry::TelemetryRepository>(m_context->database);
            m_alarms    = std::make_shared<EdgePulse::Alarm::AlarmRepository>(m_context->database);
            m_users     = std::make_shared<EdgePulse::System::UserRepository>(m_context->database);
            m_tokens    = std::make_shared<EdgePulse::Auth::ApiTokenService>(m_context->database, m_users, m_context->tenants);
            m_signing   = std::make_shared<EdgePulse::Firmware::FirmwareSigningPolicy>(EdgePulse::Firmware::FirmwareSigningPolicy{false, nullptr});
            m_storage   = std::filesystem::temp_directory_path() / ("edgepulse-rollout-api-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            m_context->auth->setApiTokenService(m_tokens);

            EdgePulse::Firmware::FirmwareRolloutDependencies dependencies;
            dependencies.firmware                 = m_firmware;
            dependencies.rollouts                 = m_rollouts;
            dependencies.commands                 = m_commands;
            dependencies.nodes                    = m_nodes;
            dependencies.telemetry                = m_telemetry;
            dependencies.alarms                   = m_alarms;
            dependencies.signingPolicy            = m_signing;
            dependencies.heartbeatIntervalSeconds = 1;
            dependencies.publisher                = [](const EdgePulse::Command::CommandRecord &) -> AsynGyanis::Core::Task<> { co_return; };
            m_service                             = std::make_shared<EdgePulse::Firmware::FirmwareRolloutService>(std::move(dependencies));

            m_server = std::make_unique<EdgePulse::TestSupport::ServerHarness>(
                    [this](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                    {
                        EdgePulse::Web::applySecurityHeaders(router);
                        EdgePulse::Auth::registerRoutes(router, loop, *m_context->auth, *m_context->captcha, true);
                        EdgePulse::System::ApiTokenDependencies tokenDependencies;
                        tokenDependencies.auth   = m_context->auth.get();
                        tokenDependencies.tokens = m_tokens.get();
                        EdgePulse::System::registerApiTokenRoutes(router, loop, tokenDependencies);

                        EdgePulse::Firmware::FirmwareDependencies firmware;
                        firmware.auth               = m_context->auth.get();
                        firmware.firmware           = m_firmware.get();
                        firmware.nodes              = m_nodes.get();
                        firmware.commands           = m_commands.get();
                        firmware.tenants            = m_context->tenants.get();
                        firmware.storageDirectory   = m_storage.string();
                        firmware.maximumUploadBytes = 8U * 1024U * 1024U;
                        firmware.signingPolicy      = m_signing;
                        firmware.rollouts           = m_service.get();
                        EdgePulse::Firmware::registerFirmwareRoutes(router, loop, firmware);
                    });
        }

        void TearDown() override
        {
            m_server.reset();
            m_service.reset();
            m_tokens.reset();
            m_users.reset();
            m_alarms.reset();
            m_telemetry.reset();
            m_nodes.reset();
            m_commands.reset();
            m_rollouts.reset();
            m_firmware.reset();
            m_runner.reset();
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
                    "/api/auth/login",
                    ConfigValue::object({{"username", "admin"}, {"password", "admin123"}, {"captcha", (*json)["data"]["code"]}, {"captchaUuid", (*json)["data"]["uuid"]}}).dump());
            EXPECT_EQ(200, response.status) << response.body;
            return (*response.json())["data"]["token"].get<std::string>();
        }

        std::int64_t createFirmware(const std::string &version)
        {
            EdgePulse::Firmware::FirmwareRecord record;
            record.name        = "EdgePulse OS " + version;
            record.version     = version;
            record.fileName    = "os-" + version + ".bin";
            record.sizeBytes   = 4096;
            record.sha256      = std::string(64, 'a');
            record.storagePath = (m_storage / ("os-" + version + ".bin")).string();
            return m_runner->run(m_firmware->create(m_runner->loop(), record));
        }

        std::int64_t createNode(const std::string &name)
        {
            return m_runner->run(m_nodes->create(m_runner->loop(), name, "{}")).id;
        }

        static ConfigValue rolloutPolicy()
        {
            const std::optional<ConfigValue> parsed =
                    AsynGyanis::Base::parseConfigValue(R"({"waves":[{"percent":50},{"percent":100}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30})");
            return parsed.value_or(ConfigValue::object());
        }

        HttpResult createRollout(const std::string &token, const std::int64_t firmwareId, const std::string &name, const std::vector<std::int64_t> &nodeIds,
                                 const ConfigValue &policy = rolloutPolicy())
        {
            return m_server->post("/api/firmware/" + std::to_string(firmwareId) + "/rollouts",
                                  ConfigValue::object({{"name", name}, {"nodeIds", nodeIds}, {"policy", policy}}).dump(), "application/json", bearer(token));
        }

        HttpResult action(const std::string &token, const std::int64_t rolloutId, const std::string &name, const ConfigValue &body = ConfigValue::object())
        {
            return m_server->post("/api/firmware/rollouts/" + std::to_string(rolloutId) + "/" + name, body.dump(), "application/json", bearer(token));
        }

        std::shared_ptr<EdgePulse::TestSupport::AuthTestContext>        m_context;
        std::shared_ptr<EdgePulse::TestSupport::EventLoopRunner>        m_runner;
        std::shared_ptr<EdgePulse::Firmware::FirmwareRepository>        m_firmware;
        std::shared_ptr<EdgePulse::Firmware::FirmwareRolloutRepository> m_rollouts;
        std::shared_ptr<EdgePulse::Command::CommandRepository>          m_commands;
        std::shared_ptr<EdgePulse::Device::NodeRepository>              m_nodes;
        std::shared_ptr<EdgePulse::Telemetry::TelemetryRepository>      m_telemetry;
        std::shared_ptr<EdgePulse::Alarm::AlarmRepository>              m_alarms;
        std::shared_ptr<EdgePulse::System::UserRepository>              m_users;
        std::shared_ptr<EdgePulse::Auth::ApiTokenService>               m_tokens;
        std::shared_ptr<EdgePulse::Firmware::FirmwareSigningPolicy>     m_signing;
        std::shared_ptr<EdgePulse::Firmware::FirmwareRolloutService>    m_service;
        std::unique_ptr<EdgePulse::TestSupport::ServerHarness>          m_server;
        std::filesystem::path                                           m_storage;
    };
} // namespace

// 钉住：匿名请求一律 401；创建阶段拒绝空目标、空波次与不存在的固件
TEST_F(FirmwareRolloutEndpointsTest, RejectsAnonymousAndInvalidCreate)
{
    EXPECT_EQ(401, m_server->get("/api/firmware/1/rollouts").status);
    EXPECT_EQ(401, m_server->post("/api/firmware/1/rollouts", R"({"name":"x","nodeIds":[1]})").status);
    EXPECT_EQ(401, m_server->get("/api/firmware/rollouts/1").status);
    EXPECT_EQ(401, m_server->post("/api/firmware/rollouts/1/pause", "{}").status);

    const std::string  token      = login();
    const std::int64_t nodeId     = createNode("rollout-api-node");
    const std::int64_t firmwareId = createFirmware("1.0.0");

    const auto emptyTargets = createRollout(token, firmwareId, "空目标", {});
    EXPECT_EQ(400, emptyTargets.status) << emptyTargets.body;

    ConfigValue emptyWaves    = ConfigValue::object({{"waves", ConfigValue::array()}});
    const auto  invalidPolicy = createRollout(token, firmwareId, "空波次", {nodeId}, emptyWaves);
    EXPECT_EQ(400, invalidPolicy.status) << invalidPolicy.body;

    const auto missingFirmware = createRollout(token, 999999999, "不存在", {nodeId});
    EXPECT_EQ(404, missingFirmware.status) << missingFirmware.body;
}

// 钉住：创建返回完整聚合，列表与详情可读，未知标识返回 404
TEST_F(FirmwareRolloutEndpointsTest, CreatesListsAndReadsRollouts)
{
    const std::string  token      = login();
    const std::int64_t first      = createNode("rollout-api-a");
    const std::int64_t second     = createNode("rollout-api-b");
    const std::int64_t firmwareId = createFirmware("2.0.0");

    const auto created = createRollout(token, firmwareId, "接口灰度", {first, second});
    ASSERT_EQ(200, created.status) << created.body;
    const auto createdJson = created.json();
    ASSERT_TRUE(createdJson.has_value());
    const ConfigValue &data      = (*createdJson)["data"];
    const std::int64_t rolloutId = data["id"].get<std::int64_t>();
    EXPECT_GT(rolloutId, 0);
    EXPECT_EQ("scheduled", data["status"].get<std::string>());
    EXPECT_EQ(firmwareId, data["firmwareId"].get<std::int64_t>());
    EXPECT_EQ(2, data["totalWaves"].get<int>());
    ASSERT_TRUE(data["waves"].is_array());
    ASSERT_EQ(2U, data["waves"].size());
    EXPECT_EQ(1, data["waves"][0]["targetCount"].get<int>());
    ASSERT_EQ(2U, data["targets"].size());
    EXPECT_TRUE(data["targets"][0]["health"].is_object());
    ASSERT_TRUE(data["policy"]["waves"].is_array());
    EXPECT_EQ(2U, data["policy"]["waves"].size());

    const auto listed = m_server->get("/api/firmware/" + std::to_string(firmwareId) + "/rollouts?pageNum=1&pageSize=10", bearer(token));
    ASSERT_EQ(200, listed.status) << listed.body;
    const auto listedJson = listed.json();
    ASSERT_TRUE(listedJson.has_value());
    EXPECT_EQ(1, (*listedJson)["data"]["total"].get<std::int64_t>());
    ASSERT_EQ(1U, (*listedJson)["data"]["rows"].size());
    EXPECT_EQ(rolloutId, (*listedJson)["data"]["rows"][0]["id"].get<std::int64_t>());

    const auto detail = m_server->get("/api/firmware/rollouts/" + std::to_string(rolloutId), bearer(token));
    ASSERT_EQ(200, detail.status) << detail.body;
    const auto detailJson = detail.json();
    ASSERT_TRUE(detailJson.has_value());
    EXPECT_EQ(rolloutId, (*detailJson)["data"]["id"].get<std::int64_t>());
    ASSERT_EQ(2U, (*detailJson)["data"]["targets"].size());
    EXPECT_EQ(first, (*detailJson)["data"]["targets"][0]["nodeId"].get<std::int64_t>());

    EXPECT_EQ(404, m_server->get("/api/firmware/rollouts/999999999", bearer(token)).status);
    EXPECT_EQ(400, m_server->get("/api/firmware/rollouts/not-a-number", bearer(token)).status);
}

// 钉住：暂停、恢复、提升、强制提升、中止与回滚动作都按状态机返回最新聚合
TEST_F(FirmwareRolloutEndpointsTest, RunsManualActions)
{
    const std::string  token      = login();
    const std::int64_t nodeId     = createNode("rollout-api-action");
    const std::int64_t firmwareId = createFirmware("2.0.1");
    ConfigValue        oneWave =
            ConfigValue::object({{"waves", ConfigValue::array({ConfigValue::object({{"size", 1}})})}, {"settleSeconds", 2}, {"observeSeconds", 4}, {"maxWaveSeconds", 30}});

    const auto created = createRollout(token, firmwareId, "动作灰度", {nodeId}, oneWave);
    ASSERT_EQ(200, created.status) << created.body;
    const std::int64_t rolloutId = (*created.json())["data"]["id"].get<std::int64_t>();

    const auto paused = action(token, rolloutId, "pause", ConfigValue::object({{"reason", "维护"}}));
    ASSERT_EQ(200, paused.status) << paused.body;
    EXPECT_EQ("paused", (*paused.json())["data"]["status"].get<std::string>());
    EXPECT_EQ("维护", (*paused.json())["data"]["pauseReason"].get<std::string>());

    const auto resumed = action(token, rolloutId, "resume");
    ASSERT_EQ(200, resumed.status) << resumed.body;
    EXPECT_EQ("running", (*resumed.json())["data"]["status"].get<std::string>());

    const auto promoted = action(token, rolloutId, "promote");
    ASSERT_EQ(200, promoted.status) << promoted.body;
    EXPECT_EQ("completed", (*promoted.json())["data"]["status"].get<std::string>());
    EXPECT_EQ(409, action(token, rolloutId, "pause").status);

    const auto abortCreated = createRollout(token, firmwareId, "中止灰度", {nodeId}, oneWave);
    ASSERT_EQ(200, abortCreated.status) << abortCreated.body;
    const std::int64_t abortId = (*abortCreated.json())["data"]["id"].get<std::int64_t>();
    const auto         aborted = action(token, abortId, "abort");
    ASSERT_EQ(200, aborted.status) << aborted.body;
    EXPECT_EQ("aborted", (*aborted.json())["data"]["status"].get<std::string>());

    const auto forceCreated = createRollout(token, firmwareId, "强制灰度", {nodeId}, oneWave);
    ASSERT_EQ(200, forceCreated.status) << forceCreated.body;
    const std::int64_t forceId = (*forceCreated.json())["data"]["id"].get<std::int64_t>();
    ASSERT_EQ(200, action(token, forceId, "pause").status);
    EXPECT_EQ(400, action(token, forceId, "force-promote", ConfigValue::object()).status);
    const auto forced = action(token, forceId, "force-promote", ConfigValue::object({{"reason", "现场确认"}}));
    ASSERT_EQ(200, forced.status) << forced.body;
    EXPECT_EQ("completed", (*forced.json())["data"]["status"].get<std::string>());
    EXPECT_NE(std::string::npos, (*forced.json())["data"]["waves"][0]["gate"].dump().find("现场确认"));

    const auto rollbackCreated = createRollout(token, firmwareId, "回滚灰度", {nodeId}, oneWave);
    ASSERT_EQ(200, rollbackCreated.status) << rollbackCreated.body;
    const std::int64_t rollbackId = (*rollbackCreated.json())["data"]["id"].get<std::int64_t>();
    ASSERT_EQ(200, action(token, rollbackId, "pause").status);
    const auto rolledBack = action(token, rollbackId, "rollback");
    ASSERT_EQ(200, rolledBack.status) << rolledBack.body;
    EXPECT_EQ("rolling_back", (*rolledBack.json())["data"]["status"].get<std::string>());

    EXPECT_EQ(404, action(token, 999999999, "pause").status);
}

// 钉住：列表只需要 device:firmware:list，创建与动作还需要 device:firmware:upload
TEST_F(FirmwareRolloutEndpointsTest, EnforcesPermissions)
{
    const std::string  session    = login();
    const std::int64_t nodeId     = createNode("rollout-api-permission");
    const std::int64_t firmwareId = createFirmware("2.0.2");
    ConfigValue        oneWave =
            ConfigValue::object({{"waves", ConfigValue::array({ConfigValue::object({{"size", 1}})})}, {"settleSeconds", 2}, {"observeSeconds", 4}, {"maxWaveSeconds", 30}});
    const auto created = createRollout(session, firmwareId, "权限灰度", {nodeId}, oneWave);
    ASSERT_EQ(200, created.status) << created.body;
    const std::int64_t rolloutId = (*created.json())["data"]["id"].get<std::int64_t>();

    const auto createToken = [this](const std::string &token, const std::string &name, const std::vector<std::string> &scopes)
    {
        ConfigValue body = ConfigValue::object({{"name", name}, {"scopes", ConfigValue::array()}});
        for (const std::string &scope: scopes)
        {
            body["scopes"].push_back(scope);
        }
        return m_server->post("/api/system/api-tokens", body.dump(), "application/json", bearer(token));
    };

    const auto listToken = createToken(session, "rollout-reader", {"device:firmware:list"});
    ASSERT_EQ(200, listToken.status) << listToken.body;
    const std::string listPlaintext = (*listToken.json())["data"]["token"].get<std::string>();
    EXPECT_EQ(200, m_server->get("/api/firmware/" + std::to_string(firmwareId) + "/rollouts", bearer(listPlaintext)).status);
    EXPECT_EQ(200, m_server->get("/api/firmware/rollouts/" + std::to_string(rolloutId), bearer(listPlaintext)).status);
    EXPECT_EQ(403, createRollout(listPlaintext, firmwareId, "越权创建", {nodeId}, oneWave).status);
    EXPECT_EQ(403, action(listPlaintext, rolloutId, "pause").status);

    const auto uploadToken = createToken(session, "rollout-writer", {"device:firmware:upload"});
    ASSERT_EQ(200, uploadToken.status) << uploadToken.body;
    const std::string uploadPlaintext = (*uploadToken.json())["data"]["token"].get<std::string>();
    EXPECT_EQ(403, m_server->get("/api/firmware/" + std::to_string(firmwareId) + "/rollouts", bearer(uploadPlaintext)).status);
    const auto uploadCreated = createRollout(uploadPlaintext, firmwareId, "写入灰度", {nodeId}, oneWave);
    EXPECT_EQ(200, uploadCreated.status) << uploadCreated.body;
}
