#include "Base/Config/ConfigValue.h"
#include "Base/Database/Database.h"
#include "Base/Runtime/TimeFormat.h"
#include "Base/Security/PasswordHash.h"
#include "Core/Auth/AuthService.h"
#include "Core/Command/AgentChannelRegistry.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/AgentRegistry.h"
#include "Core/Device/NodeConfigRepository.h"
#include "Core/Device/NodeConfigService.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Device/NodeService.h"
#include "Core/Http/Authorization.h"
#include "Core/Http/MiddlewareSetup.h"
#include "Core/System/TenantRepository.h"
#include "Plugins/AuthPlugin/AuthController.h"
#include "Plugins/DevicePlugin/AgentController.h"
#include "Plugins/DevicePlugin/DeviceController.h"
#include "Plugins/DevicePlugin/NodeConfigController.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"
#include "TestSupport/HttpClientHarness.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using Headers = std::vector<std::pair<std::string, std::string>>;

    Headers bearer(const std::string &token)
    {
        return {{"Authorization", "Bearer " + token}};
    }

    Headers agentHeaders(const std::string &fingerprint)
    {
        return {{"X-Test-Agent-Fingerprint", fingerprint}};
    }

    /**
     * @brief 取响应 JSON 的 data 节点
     */
    std::optional<AsynGyanis::Base::ConfigValue> dataOf(const EdgePulse::TestSupport::HttpResult &result)
    {
        const auto json = result.json();
        if (!json.has_value() || !json->contains("data") || !(*json)["data"].is_object())
        {
            return std::nullopt;
        }
        return (*json)["data"];
    }
} // namespace

class NodeConfigEndpointsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_context     = std::make_unique<EdgePulse::TestSupport::AuthTestContext>();
        m_nodes       = std::make_shared<EdgePulse::Device::NodeRepository>(m_context->database);
        m_nodeService = std::make_shared<EdgePulse::Device::NodeService>(m_nodes);
        m_configs     = std::make_shared<EdgePulse::Device::NodeConfigRepository>(m_context->database);
        m_commands    = std::make_shared<EdgePulse::Command::CommandRepository>(m_context->database);
        m_channels    = std::make_shared<EdgePulse::Command::AgentChannelRegistry>();
        m_service     = std::make_shared<EdgePulse::Device::NodeConfigService>(m_configs, m_nodes, m_commands, nullptr, nullptr);
        m_registry    = std::make_shared<EdgePulse::Device::AgentRegistry>(m_nodes, EdgePulse::Device::AgentRegistry::Options{});

        m_server = std::make_unique<EdgePulse::TestSupport::ServerHarness>(
                [this](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                {
                    EdgePulse::Web::applySecurityHeaders(router);
                    EdgePulse::TestSupport::installTestAgentIdentity(router);
                    EdgePulse::Auth::registerRoutes(router, loop, *m_context->auth, *m_context->captcha, true);

                    EdgePulse::Device::NodeConfigDependencies configDependencies;
                    configDependencies.auth    = m_context->auth.get();
                    configDependencies.configs = m_configs.get();
                    configDependencies.service = m_service.get();
                    configDependencies.nodes   = m_nodes.get();
                    // 配置路由先注册，节点详情等参数化路由随后挂载
                    EdgePulse::Device::registerNodeConfigRoutes(router, loop, configDependencies);

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
                    EdgePulse::Device::registerRoutes(router, loop, *m_nodeService, access, nullptr, m_context->tenants.get(), nullptr, nullptr, m_configs.get());
                    EdgePulse::Device::registerRoutes(router, loop, *m_registry, *m_nodes, *m_context->tenants, 30, m_configs.get());
                    EdgePulse::Device::registerNodeConfigAgentRoutes(router, loop, configDependencies);
                });
    }

    void TearDown() override
    {
        m_server.reset();
        m_registry.reset();
        m_service.reset();
        m_channels.reset();
        m_commands.reset();
        m_configs.reset();
        m_nodeService.reset();
        m_nodes.reset();
        m_context.reset();
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

    std::int64_t createNode(const std::string &token, const std::string &name)
    {
        const auto response = m_server->post("/api/nodes", AsynGyanis::Base::ConfigValue::object({{"name", name}, {"groupId", 200}}).dump(), "application/json", bearer(token));
        EXPECT_EQ(200, response.status) << response.body;
        return (*response.json())["data"]["id"].get<std::int64_t>();
    }

    void setNodeFingerprint(const std::int64_t nodeId, const std::string &fingerprint)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(m_context->databasePath().string());
        ASSERT_NE(nullptr, connection->execute("UPDATE biz_node SET cert_fingerprint = '" + fingerprint + "' WHERE id = " + std::to_string(nodeId)));
    }

    void executeOnDatabase(const std::string &sql)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(m_context->databasePath().string());
        ASSERT_NE(nullptr, connection->execute(sql)) << connection->lastError();
    }

    std::unique_ptr<EdgePulse::TestSupport::AuthTestContext>  m_context;
    EdgePulse::TestSupport::EventLoopRunner                   m_runner;
    std::shared_ptr<EdgePulse::Device::NodeRepository>        m_nodes;
    std::shared_ptr<EdgePulse::Device::NodeService>           m_nodeService;
    std::shared_ptr<EdgePulse::Device::NodeConfigRepository>  m_configs;
    std::shared_ptr<EdgePulse::Command::CommandRepository>    m_commands;
    std::shared_ptr<EdgePulse::Command::AgentChannelRegistry> m_channels;
    std::shared_ptr<EdgePulse::Device::NodeConfigService>     m_service;
    std::shared_ptr<EdgePulse::Device::AgentRegistry>         m_registry;
    std::unique_ptr<EdgePulse::TestSupport::ServerHarness>    m_server;
};

// 钉住：管理端下发、Agent 拉取与回报、修订列表、回滚与详情摘要串成一条闭环
TEST_F(NodeConfigEndpointsTest, ConfigLifecycleAcrossBrowserAndAgent)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "config-lifecycle-node");
    const std::string  fingerprint(64, 'a');
    setNodeFingerprint(nodeId, fingerprint);

    const auto initial = m_server->get("/api/nodes/" + std::to_string(nodeId) + "/config", bearer(token));
    ASSERT_EQ(200, initial.status) << initial.body;
    const auto initialData = dataOf(initial);
    ASSERT_TRUE(initialData.has_value());
    EXPECT_EQ(0, (*initialData)["desired"]["version"].get<std::int64_t>());
    EXPECT_TRUE((*initialData)["inSync"].get<bool>());

    const auto applied = m_server->send(
            "PUT", "/api/nodes/" + std::to_string(nodeId) + "/config",
            AsynGyanis::Base::ConfigValue::object({{"content", AsynGyanis::Base::ConfigValue::object({{"interval", 30}})}, {"expectedVersion", 0}, {"note", "初始配置"}}).dump(),
            "application/json", bearer(token));
    ASSERT_EQ(200, applied.status) << applied.body;
    const auto appliedData = dataOf(applied);
    ASSERT_TRUE(appliedData.has_value());
    EXPECT_EQ(1, (*appliedData)["desired"]["version"].get<std::int64_t>());
    EXPECT_EQ(30, (*appliedData)["desired"]["content"]["interval"].get<std::int64_t>());
    EXPECT_FALSE((*appliedData)["inSync"].get<bool>());

    // 离线节点：config.apply 留在待执行队列，等 Agent 轮询或重连
    const auto commands = m_runner.run(m_commands->scoped({0, true})->list(m_runner.loop(), "", nodeId, {1, 100}));
    ASSERT_EQ(1U, commands.rows.size());
    EXPECT_EQ("config.apply", commands.rows[0].type);
    EXPECT_EQ("pending", commands.rows[0].status);
    EXPECT_EQ(60, commands.rows[0].timeoutSeconds);
    const auto payload = AsynGyanis::Base::parseConfigValue(commands.rows[0].payload);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(1, (*payload)["version"].get<std::int64_t>());

    const auto detail = m_server->get("/api/nodes/" + std::to_string(nodeId), bearer(token));
    ASSERT_EQ(200, detail.status) << detail.body;
    const auto detailData = dataOf(detail);
    ASSERT_TRUE(detailData.has_value());
    EXPECT_EQ(1, (*detailData)["desiredConfigVersion"].get<std::int64_t>());
    EXPECT_EQ(0, (*detailData)["reportedConfigVersion"].get<std::int64_t>());
    EXPECT_FALSE((*detailData)["configInSync"].get<bool>());

    const auto agentView = m_server->get("/api/agent/nodes/" + std::to_string(nodeId) + "/config", agentHeaders(fingerprint));
    ASSERT_EQ(200, agentView.status) << agentView.body;
    const auto agentData = dataOf(agentView);
    ASSERT_TRUE(agentData.has_value());
    EXPECT_EQ(1, (*agentData)["version"].get<std::int64_t>());
    EXPECT_EQ(30, (*agentData)["content"]["interval"].get<std::int64_t>());
    EXPECT_EQ(64U, (*agentData)["hash"].get<std::string>().size());

    const auto report =
            m_server->post("/api/agent/nodes/" + std::to_string(nodeId) + "/config/report",
                           AsynGyanis::Base::ConfigValue::object(
                                   {{"version", 1}, {"content", AsynGyanis::Base::ConfigValue::object({{"interval", 30}})}, {"status", "applied"}, {"message", "已生效"}})
                                   .dump(),
                           "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, report.status) << report.body;
    const auto reportData = dataOf(report);
    ASSERT_TRUE(reportData.has_value());
    EXPECT_TRUE((*reportData)["inSync"].get<bool>());

    const auto synced = m_server->get("/api/nodes/" + std::to_string(nodeId) + "/config", bearer(token));
    ASSERT_EQ(200, synced.status) << synced.body;
    const auto syncedData = dataOf(synced);
    ASSERT_TRUE(syncedData.has_value());
    EXPECT_TRUE((*syncedData)["inSync"].get<bool>());
    EXPECT_EQ(1, (*syncedData)["reported"]["version"].get<std::int64_t>());
    EXPECT_EQ("applied", (*syncedData)["reported"]["status"].get<std::string>());
    EXPECT_EQ("已生效", (*syncedData)["reported"]["message"].get<std::string>());

    const auto revisions = m_server->get("/api/nodes/" + std::to_string(nodeId) + "/config/revisions?pageNum=1&pageSize=10", bearer(token));
    ASSERT_EQ(200, revisions.status) << revisions.body;
    const auto revisionsData = dataOf(revisions);
    ASSERT_TRUE(revisionsData.has_value());
    ASSERT_EQ(1, (*revisionsData)["total"].get<std::int64_t>());
    ASSERT_EQ(1U, (*revisionsData)["rows"].size());
    EXPECT_EQ(1, (*revisionsData)["rows"][0]["version"].get<std::int64_t>());
    EXPECT_EQ("初始配置", (*revisionsData)["rows"][0]["note"].get<std::string>());
    const std::int64_t revisionId = (*revisionsData)["rows"][0]["id"].get<std::int64_t>();

    const auto rolled = m_server->post("/api/nodes/" + std::to_string(nodeId) + "/config/rollback",
                                       AsynGyanis::Base::ConfigValue::object({{"revisionId", revisionId}, {"note", "回到 30"}}).dump(), "application/json", bearer(token));
    ASSERT_EQ(200, rolled.status) << rolled.body;
    const auto rolledData = dataOf(rolled);
    ASSERT_TRUE(rolledData.has_value());
    EXPECT_EQ(2, (*rolledData)["desired"]["version"].get<std::int64_t>());
    EXPECT_FALSE((*rolledData)["inSync"].get<bool>());

    // 旧版本号再写入必须冲突，而不是覆盖回版本 1
    const auto stale =
            m_server->send("PUT", "/api/nodes/" + std::to_string(nodeId) + "/config",
                           AsynGyanis::Base::ConfigValue::object({{"content", AsynGyanis::Base::ConfigValue::object({{"interval", 60}})}, {"expectedVersion", 0}}).dump(),
                           "application/json", bearer(token));
    EXPECT_EQ(409, stale.status) << stale.body;
}

// 钉住：Agent 配置路由先绑定证书身份，没有证书或指纹不符的请求一律拒绝
TEST_F(NodeConfigEndpointsTest, AgentConfigRoutesRequireCertificateBinding)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "config-identity-node");
    const std::string  fingerprint(64, 'b');

    EXPECT_EQ(403, m_server->get("/api/agent/nodes/" + std::to_string(nodeId) + "/config", agentHeaders(fingerprint)).status);

    setNodeFingerprint(nodeId, fingerprint);
    EXPECT_EQ(403, m_server->get("/api/agent/nodes/" + std::to_string(nodeId) + "/config").status);
    EXPECT_EQ(403, m_server->get("/api/agent/nodes/" + std::to_string(nodeId) + "/config", agentHeaders(std::string(64, 'c'))).status);
    EXPECT_EQ(200, m_server->get("/api/agent/nodes/" + std::to_string(nodeId) + "/config", agentHeaders(fingerprint)).status);

    const auto applied = m_runner.run(m_service->applyDesired(m_runner.loop(), {1, false}, nodeId, R"({"interval":30})", 0, "", 1));
    ASSERT_TRUE(applied.has_value()) << applied.error().message;

    const auto body = AsynGyanis::Base::ConfigValue::object({{"version", 1}, {"status", "applied"}}).dump();
    EXPECT_EQ(403, m_server->post("/api/agent/nodes/" + std::to_string(nodeId) + "/config/report", body).status);
    EXPECT_EQ(403, m_server->post("/api/agent/nodes/" + std::to_string(nodeId) + "/config/report", body, "application/json", agentHeaders(std::string(64, 'd'))).status);
    ASSERT_EQ(200, m_server->post("/api/agent/nodes/" + std::to_string(nodeId) + "/config/report", body, "application/json", agentHeaders(fingerprint)).status);

    // 别家租户的证书同样不能冒充本节点
    EdgePulse::System::TenantProvisionRequest request;
    request.code              = "config_identity_tenant";
    request.name              = "身份租户";
    request.adminUsername     = "config-identity-admin";
    request.adminPasswordHash = EdgePulse::Common::hashPassword("identity-pass-1");
    const auto provisioned    = m_runner.run(m_context->tenants->provision(m_runner.loop(), std::move(request), 1));
    ASSERT_TRUE(provisioned.has_value());
    const auto foreignNode = m_runner.run(m_nodes->scoped({provisioned->tenantId, false})->create(m_runner.loop(), "foreign-config-node", "{}"));
    setNodeFingerprint(foreignNode.id, std::string(64, 'e'));
    EXPECT_EQ(403, m_server->get("/api/agent/nodes/" + std::to_string(nodeId) + "/config", agentHeaders(std::string(64, 'e'))).status);
}

// 钉住：心跳响应携带节点当前的期望配置版本，Agent 不必额外轮询才知道该拉哪一版
TEST_F(NodeConfigEndpointsTest, HeartbeatCarriesDesiredConfigVersion)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "config-heartbeat-node");
    const std::string  fingerprint(64, 'f');
    setNodeFingerprint(nodeId, fingerprint);
    const std::string heartbeatBody = R"({"nodeId":)" + std::to_string(nodeId) + R"(,"agentVersion":"1.0.0"})";

    const auto before = m_server->post("/api/agent/heartbeat", heartbeatBody, "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, before.status) << before.body;
    const auto beforeData = dataOf(before);
    ASSERT_TRUE(beforeData.has_value());
    EXPECT_EQ(0, (*beforeData)["desiredConfigVersion"].get<std::int64_t>());
    EXPECT_EQ(30, (*beforeData)["heartbeatIntervalSeconds"].get<std::int64_t>());

    const auto applied =
            m_server->send("PUT", "/api/nodes/" + std::to_string(nodeId) + "/config",
                           AsynGyanis::Base::ConfigValue::object({{"content", AsynGyanis::Base::ConfigValue::object({{"interval", 30}})}, {"expectedVersion", 0}}).dump(),
                           "application/json", bearer(token));
    ASSERT_EQ(200, applied.status) << applied.body;

    const auto after = m_server->post("/api/agent/heartbeat", heartbeatBody, "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, after.status) << after.body;
    const auto afterData = dataOf(after);
    ASSERT_TRUE(afterData.has_value());
    EXPECT_EQ(1, (*afterData)["desiredConfigVersion"].get<std::int64_t>());
}

// 钉住：非法正文、超限内容与非法上报状态在控制器与服务层被拒绝
TEST_F(NodeConfigEndpointsTest, RejectsInvalidConfigPayloads)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "config-invalid-node");
    const std::string  path   = "/api/nodes/" + std::to_string(nodeId) + "/config";

    // 配置路由同样要求会话，未登录不能读也不能写
    EXPECT_EQ(401, m_server->get(path).status);
    EXPECT_EQ(401, m_server->send("PUT", path, "{}", "application/json").status);

    const auto arrayContent =
            m_server->send("PUT", path, AsynGyanis::Base::ConfigValue::object({{"content", AsynGyanis::Base::ConfigValue::array()}, {"expectedVersion", 0}}).dump(),
                           "application/json", bearer(token));
    EXPECT_EQ(400, arrayContent.status) << arrayContent.body;

    const auto missingVersion = m_server->send("PUT", path, AsynGyanis::Base::ConfigValue::object({{"content", AsynGyanis::Base::ConfigValue::object({{"interval", 30}})}}).dump(),
                                               "application/json", bearer(token));
    EXPECT_EQ(400, missingVersion.status) << missingVersion.body;

    const auto oversized = m_server->send(
            "PUT", path,
            AsynGyanis::Base::ConfigValue::object({{"content", AsynGyanis::Base::ConfigValue::object({{"blob", std::string(70U * 1024U, 'x')}})}, {"expectedVersion", 0}}).dump(),
            "application/json", bearer(token));
    EXPECT_EQ(400, oversized.status) << oversized.body;

    EXPECT_EQ(404, m_server->get("/api/nodes/999999/config", bearer(token)).status);

    const std::string fingerprint(64, '1');
    setNodeFingerprint(nodeId, fingerprint);
    const auto applied = m_runner.run(m_service->applyDesired(m_runner.loop(), {1, false}, nodeId, R"({"interval":30})", 0, "", 1));
    ASSERT_TRUE(applied.has_value()) << applied.error().message;
    const auto reportPath = "/api/agent/nodes/" + std::to_string(nodeId) + "/config/report";
    EXPECT_EQ(400, m_server->post(reportPath, AsynGyanis::Base::ConfigValue::object({{"version", 1}, {"status", "unknown"}}).dump(), "application/json", agentHeaders(fingerprint))
                           .status);
    EXPECT_EQ(400, m_server->post(reportPath, AsynGyanis::Base::ConfigValue::object({{"version", -1}, {"status", "failed"}}).dump(), "application/json", agentHeaders(fingerprint))
                           .status);
}

// 钉住：租户管理员看不到也改不了别家租户的节点配置，平台管理员可以跨租户查看
TEST_F(NodeConfigEndpointsTest, TenantAdminCannotReachOtherTenantConfig)
{
    EdgePulse::System::TenantProvisionRequest request;
    request.code              = "config_scope_tenant";
    request.name              = "配置租户";
    request.adminUsername     = "config-scope-admin";
    request.adminPasswordHash = EdgePulse::Common::hashPassword("scope-pass-1");
    const auto provisioned    = m_runner.run(m_context->tenants->provision(m_runner.loop(), std::move(request), 1));
    ASSERT_TRUE(provisioned.has_value());

    const std::string  platformToken = login();
    const std::int64_t foreignNodeId = createNode(platformToken, "config-scope-node");
    const std::string  tenantToken   = loginInTenant("config-scope-admin", "scope-pass-1", "config_scope_tenant");
    const std::string  path          = "/api/nodes/" + std::to_string(foreignNodeId);

    EXPECT_EQ(404, m_server->get(path + "/config", bearer(tenantToken)).status);
    EXPECT_EQ(404, m_server->get(path + "/config/revisions", bearer(tenantToken)).status);
    EXPECT_EQ(404, m_server->send("PUT", path + "/config",
                                  AsynGyanis::Base::ConfigValue::object({{"content", AsynGyanis::Base::ConfigValue::object({{"interval", 30}})}, {"expectedVersion", 0}}).dump(),
                                  "application/json", bearer(tenantToken))
                           .status);
    EXPECT_EQ(404, m_server->post(path + "/config/rollback", AsynGyanis::Base::ConfigValue::object({{"revisionId", 1}}).dump(), "application/json", bearer(tenantToken)).status);

    // 平台管理员按全租户视图看到同一节点（尚无配置时返回版本 0 且视为同步）
    const auto platformView = m_server->get(path + "/config", bearer(platformToken));
    ASSERT_EQ(200, platformView.status) << platformView.body;
    const auto platformData = dataOf(platformView);
    ASSERT_TRUE(platformData.has_value());
    EXPECT_EQ(0, (*platformData)["desired"]["version"].get<std::int64_t>());
    EXPECT_TRUE((*platformData)["inSync"].get<bool>());
}

// 钉住：尚无期望配置时上报只能拿到 404，不能凭空创建一行影子
TEST_F(NodeConfigEndpointsTest, ReportBeforeDesiredConfigReturnsNotFound)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "config-no-desired-node");
    const std::string  fingerprint(64, '9');
    setNodeFingerprint(nodeId, fingerprint);

    const auto report = m_server->post("/api/agent/nodes/" + std::to_string(nodeId) + "/config/report",
                                       AsynGyanis::Base::ConfigValue::object({{"version", 1}, {"status", "applied"}}).dump(), "application/json", agentHeaders(fingerprint));
    EXPECT_EQ(404, report.status) << report.body;
}

// 钉住：存储的正文被外部改坏时接口报错，不能把损坏数据当成空配置交给界面
TEST_F(NodeConfigEndpointsTest, CorruptStoredConfigFailsInsteadOfRenderingEmptyObject)
{
    const std::string  token   = login();
    const std::int64_t nodeId  = createNode(token, "config-corrupt-node");
    const auto         applied = m_runner.run(m_service->applyDesired(m_runner.loop(), {1, false}, nodeId, R"({"interval":30})", 0, "", 1));
    ASSERT_TRUE(applied.has_value()) << applied.error().message;

    executeOnDatabase("UPDATE biz_node_config SET desired_json = '{bad' WHERE node_id = " + std::to_string(nodeId));
    const auto corruptConfig = m_server->get("/api/nodes/" + std::to_string(nodeId) + "/config", bearer(token));
    // 安全头中间件把未捕获异常收成 500，响应里不带存储原文
    EXPECT_EQ(500, corruptConfig.status) << corruptConfig.body;

    // 修订正文走同一条判据：修回期望正文后单独弄坏修订
    executeOnDatabase(R"(UPDATE biz_node_config SET desired_json = '{"interval":30}' WHERE node_id = )" + std::to_string(nodeId));
    executeOnDatabase("UPDATE biz_node_config_revision SET content_json = '{bad' WHERE node_id = " + std::to_string(nodeId));
    const auto corruptRevision = m_server->get("/api/nodes/" + std::to_string(nodeId) + "/config/revisions", bearer(token));
    EXPECT_EQ(500, corruptRevision.status) << corruptRevision.body;
}
