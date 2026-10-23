#include "Base/Config/ConfigValue.h"
#include "Core/Auth/AuthService.h"
#include "Core/Device/AgentRegistry.h"
#include "Core/Device/EnrollmentRepository.h"
#include "Core/Device/EnrollmentService.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Device/NodeService.h"
#include "Core/Http/MiddlewareSetup.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Plugins/AuthPlugin/AuthController.h"
#include "Plugins/DevicePlugin/AgentController.h"
#include "Plugins/DevicePlugin/DeviceController.h"
#include "Plugins/DevicePlugin/EnrollmentController.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"
#include "TestSupport/HttpClientHarness.h"

#include <gtest/gtest.h>

#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    std::string loginBody(const std::string &username, const std::string &password, const std::string &captcha, const std::string &uuid)
    {
        const AsynGyanis::Base::ConfigValue body =
                AsynGyanis::Base::ConfigValue::object({{"username", username}, {"password", password}, {"captcha", captcha}, {"captchaUuid", uuid}});
        return body.dump();
    }

    std::vector<std::pair<std::string, std::string>> bearer(const std::string &token)
    {
        return {{"Authorization", "Bearer " + token}};
    }

    std::optional<std::string> bearerToken(const AsynGyanis::Net::HttpRequest &request)
    {
        const std::optional<std::string> header = request.getHeader("authorization");
        if (!header.has_value() || header->size() <= 7)
        {
            return std::nullopt;
        }
        const std::string_view prefix = "Bearer ";
        for (std::size_t index = 0; index < prefix.size(); ++index)
        {
            if (std::tolower(static_cast<unsigned char>((*header)[index])) != std::tolower(static_cast<unsigned char>(prefix[index])))
            {
                return std::nullopt;
            }
        }
        return header->substr(prefix.size());
    }
} // namespace

class EnrollmentEndpointsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_context              = std::make_unique<EdgePulse::TestSupport::AuthTestContext>();
        m_nodeRepository       = std::make_shared<EdgePulse::Device::NodeRepository>(m_context->database);
        m_nodeService          = std::make_unique<EdgePulse::Device::NodeService>(m_nodeRepository);
        m_enrollmentRepository = std::make_shared<EdgePulse::Device::EnrollmentRepository>(m_context->database);
        m_enrollmentService    = std::make_unique<EdgePulse::Device::EnrollmentService>(m_enrollmentRepository);
        EdgePulse::Device::AgentRegistry::Options options;
        options.enrollmentMode = "required";
        options.enrollments    = m_enrollmentService.get();
        m_agentRegistry        = std::make_unique<EdgePulse::Device::AgentRegistry>(m_nodeRepository, options);
        m_server               = std::make_unique<EdgePulse::TestSupport::ServerHarness>(
                [this](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                {
                    EdgePulse::Web::applySecurityHeaders(router);
                    EdgePulse::Auth::registerRoutes(router, loop, *m_context->auth, *m_context->captcha, true);
                    EdgePulse::Device::registerRoutes(router, loop, *m_nodeService, [this](AsynGyanis::Core::EventLoop &accessLoop, AsynGyanis::Net::HttpRequest &request)
                                                      { return resolveAccess(accessLoop, request); });
                    EdgePulse::Device::EnrollmentDependencies dependencies;
                    dependencies.auth        = m_context->auth.get();
                    dependencies.enrollments = m_enrollmentService.get();
                    dependencies.tenants     = m_context->tenants.get();
                    EdgePulse::Device::registerEnrollmentRoutes(router, loop, dependencies);
                    EdgePulse::Device::registerRoutes(router, loop, *m_agentRegistry, *m_nodeRepository, *m_context->tenants, 30);
                });
    }

    void TearDown() override
    {
        m_server.reset();
        m_agentRegistry.reset();
        m_enrollmentService.reset();
        m_enrollmentRepository.reset();
        m_nodeService.reset();
        m_nodeRepository.reset();
        m_context.reset();
    }

    AsynGyanis::Core::Task<std::optional<EdgePulse::Device::AccessContext>> resolveAccess(AsynGyanis::Core::EventLoop &loop, AsynGyanis::Net::HttpRequest &request)
    {
        const std::optional<std::string> token = bearerToken(request);
        if (!token.has_value())
        {
            co_return std::nullopt;
        }
        const auto session = co_await m_context->auth->authenticate(loop, *token);
        if (!session.has_value())
        {
            co_return std::nullopt;
        }
        co_return EdgePulse::Device::AccessContext{session->userId,       session->tenantId, session->platformAdmin, session->username,
                                                   session->departmentId, session->roles,    session->permissions};
    }

    std::string login()
    {
        const auto captcha = m_server->get("/api/auth/captcha");
        EXPECT_EQ(200, captcha.status) << captcha.body;
        const auto captchaJson = captcha.json();
        EXPECT_TRUE(captchaJson.has_value());
        const std::string uuid          = (*captchaJson)["data"]["uuid"].get<std::string>();
        const std::string code          = (*captchaJson)["data"]["code"].get<std::string>();
        const auto        loginResponse = m_server->post("/api/auth/login", loginBody("admin", "admin123", code, uuid), "application/json");
        EXPECT_EQ(200, loginResponse.status) << loginResponse.body;
        const auto loginJson = loginResponse.json();
        EXPECT_TRUE(loginJson.has_value());
        return (*loginJson)["data"]["token"].get<std::string>();
    }

    std::unique_ptr<EdgePulse::TestSupport::AuthTestContext> m_context;
    std::shared_ptr<EdgePulse::Device::NodeRepository>       m_nodeRepository;
    std::unique_ptr<EdgePulse::Device::NodeService>          m_nodeService;
    std::shared_ptr<EdgePulse::Device::EnrollmentRepository> m_enrollmentRepository;
    std::unique_ptr<EdgePulse::Device::EnrollmentService>    m_enrollmentService;
    std::unique_ptr<EdgePulse::Device::AgentRegistry>        m_agentRegistry;
    std::unique_ptr<EdgePulse::TestSupport::ServerHarness>   m_server;
};

// 钉住：令牌明文只在签发响应出现一次；列表只给状态，不泄露明文或哈希；吊销后不可再用
TEST_F(EnrollmentEndpointsTest, IssuesListsAndRevokesTokens)
{
    const std::string token = login();
    ASSERT_FALSE(token.empty());

    const auto created = m_server->post("/api/enrollment-tokens", R"({"nodeName":"edge-cert","action":"enroll","expiresInSeconds":3600})", "application/json", bearer(token));
    ASSERT_EQ(200, created.status) << created.body;
    const auto createdJson = created.json();
    ASSERT_TRUE(createdJson.has_value());
    const std::int64_t tokenId = (*createdJson)["data"]["id"].get<std::int64_t>();
    EXPECT_EQ("edge-cert", (*createdJson)["data"]["nodeName"].get<std::string>());
    EXPECT_EQ("enroll", (*createdJson)["data"]["action"].get<std::string>());
    EXPECT_EQ("active", (*createdJson)["data"]["status"].get<std::string>());
    const std::string plaintext = (*createdJson)["data"]["token"].get<std::string>();
    EXPECT_TRUE(plaintext.starts_with("ep_enroll_"));

    const auto listed = m_server->get("/api/enrollment-tokens", bearer(token));
    ASSERT_EQ(200, listed.status) << listed.body;
    const auto listedJson = listed.json();
    ASSERT_TRUE(listedJson.has_value());
    ASSERT_EQ(1U, (*listedJson)["data"].size());
    const auto &row = (*listedJson)["data"][0];
    EXPECT_EQ(tokenId, row["id"].get<std::int64_t>());
    EXPECT_EQ("active", row["status"].get<std::string>());
    EXPECT_FALSE(row.contains("token"));
    EXPECT_FALSE(row.contains("tokenHash"));
    EXPECT_FALSE(row.contains("token_hash"));

    const auto revoked = m_server->send("DELETE", "/api/enrollment-tokens/" + std::to_string(tokenId), "", "", bearer(token));
    EXPECT_EQ(200, revoked.status) << revoked.body;

    const auto after = m_server->get("/api/enrollment-tokens?status=revoked", bearer(token));
    ASSERT_EQ(200, after.status) << after.body;
    const auto afterJson = after.json();
    ASSERT_TRUE(afterJson.has_value());
    ASSERT_EQ(1U, (*afterJson)["data"].size());
    EXPECT_EQ("revoked", (*afterJson)["data"][0]["status"].get<std::string>());

    const auto repeated = m_server->send("DELETE", "/api/enrollment-tokens/" + std::to_string(tokenId), "", "", bearer(token));
    EXPECT_EQ(404, repeated.status) << repeated.body;
}

// 钉住：参数与筛选值不合法时 400；未登录 401
TEST_F(EnrollmentEndpointsTest, RejectsInvalidRequests)
{
    const std::string token = login();
    ASSERT_FALSE(token.empty());

    EXPECT_EQ(400, m_server->post("/api/enrollment-tokens", R"({"action":"rename"})", "application/json", bearer(token)).status);
    EXPECT_EQ(400, m_server->post("/api/enrollment-tokens", R"({"expiresInSeconds":10})", "application/json", bearer(token)).status);
    EXPECT_EQ(400, m_server->post("/api/enrollment-tokens", R"({"nodeName":42})", "application/json", bearer(token)).status);
    EXPECT_EQ(400, m_server->get("/api/enrollment-tokens?status=unknown", bearer(token)).status);

    const auto anonymous = m_server->get("/api/enrollment-tokens");
    EXPECT_EQ(401, anonymous.status) << anonymous.body;
}

// 钉住：节点详情带证书字段；吊销清空指纹、置离线、递增版本并记录吊销时间
TEST_F(EnrollmentEndpointsTest, NodeDetailShowsCertificateLifecycle)
{
    const std::string token = login();
    ASSERT_FALSE(token.empty());

    const auto created = m_server->post("/api/nodes", R"({"name":"edge-detail"})", "application/json", bearer(token));
    ASSERT_EQ(200, created.status) << created.body;
    const auto createdJson = created.json();
    ASSERT_TRUE(createdJson.has_value());
    const std::int64_t nodeId  = (*createdJson)["data"]["id"].get<std::int64_t>();
    const std::int64_t version = (*createdJson)["data"]["version"].get<std::int64_t>();
    EXPECT_TRUE((*createdJson)["data"]["certNotAfter"].get<std::string>().empty());
    EXPECT_TRUE((*createdJson)["data"]["certRevokedAt"].get<std::string>().empty());

    const auto detail = m_server->get("/api/nodes/" + std::to_string(nodeId), bearer(token));
    ASSERT_EQ(200, detail.status) << detail.body;
    const auto detailJson = detail.json();
    ASSERT_TRUE(detailJson.has_value());
    EXPECT_EQ("edge-detail", (*detailJson)["data"]["name"].get<std::string>());
    EXPECT_TRUE((*detailJson)["data"]["certNotAfter"].get<std::string>().empty());
    EXPECT_TRUE((*detailJson)["data"]["certRevokedAt"].get<std::string>().empty());

    const auto revoked = m_server->post("/api/nodes/" + std::to_string(nodeId) + "/certificate/revoke", "", "application/json", bearer(token));
    ASSERT_EQ(200, revoked.status) << revoked.body;
    const auto revokedJson = revoked.json();
    ASSERT_TRUE(revokedJson.has_value());
    EXPECT_TRUE((*revokedJson)["data"]["certFingerprint"].get<std::string>().empty());
    EXPECT_FALSE((*revokedJson)["data"]["certRevokedAt"].get<std::string>().empty());
    EXPECT_EQ("offline", (*revokedJson)["data"]["status"].get<std::string>());
    EXPECT_EQ(version + 1, (*revokedJson)["data"]["version"].get<std::int64_t>());

    const auto missing = m_server->get("/api/nodes/999999", bearer(token));
    EXPECT_EQ(404, missing.status) << missing.body;
}

// 钉住：注册消费令牌后管理端能看到 used 状态，待用列表不再包含它
TEST_F(EnrollmentEndpointsTest, RegistrationConsumesToken)
{
    const std::string token = login();
    ASSERT_FALSE(token.empty());

    const auto created = m_server->post("/api/enrollment-tokens", R"({"nodeName":"edge-agent","action":"enroll"})", "application/json", bearer(token));
    ASSERT_EQ(200, created.status) << created.body;
    const auto createdJson = created.json();
    ASSERT_TRUE(createdJson.has_value());
    const std::string plaintext = (*createdJson)["data"]["token"].get<std::string>();

    EdgePulse::Device::AgentRegistry::Options options;
    options.enrollmentMode = "required";
    options.enrollments    = m_enrollmentService.get();
    EdgePulse::Device::AgentRegistry registry(m_nodeRepository, options);

    EdgePulse::Device::AgentRegistration registration;
    registration.name            = "edge-agent";
    registration.agentVersion    = "1.0.0";
    registration.certFingerprint = std::string(64, 'a');
    registration.enrollmentToken = plaintext;
    // 注册走独立事件循环：管理端 HTTP 服务器不在这条线程上，两者只共享数据库
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                              registered = runner.run(registry.registerAgent(runner.loop(), registration));
    ASSERT_TRUE(registered.has_value()) << registered.error().message;

    const auto active = m_server->get("/api/enrollment-tokens?status=active", bearer(token));
    ASSERT_EQ(200, active.status) << active.body;
    const auto activeJson = active.json();
    ASSERT_TRUE(activeJson.has_value());
    EXPECT_TRUE((*activeJson)["data"].empty());

    const auto used = m_server->get("/api/enrollment-tokens?status=used", bearer(token));
    ASSERT_EQ(200, used.status) << used.body;
    const auto usedJson = used.json();
    ASSERT_TRUE(usedJson.has_value());
    ASSERT_EQ(1U, (*usedJson)["data"].size());
    EXPECT_EQ("used", (*usedJson)["data"][0]["status"].get<std::string>());
    EXPECT_FALSE((*usedJson)["data"][0].contains("token"));
}

// 钉住：Agent 注册路由解析 enrollmentToken；类型不符 400，required 模式缺令牌 403，带令牌成功
TEST_F(EnrollmentEndpointsTest, AgentRegistrationAcceptsEnrollmentToken)
{
    const std::string fingerprint(64, 'a');
    const std::string invalid = R"({"name":"edge-token","agentVersion":"1.0.0","certFingerprint":")" + fingerprint + R"(","enrollmentToken":42})";
    EXPECT_EQ(400, m_server->post("/api/agent/register", invalid, "application/json").status);

    const std::string missing  = R"({"name":"edge-token","agentVersion":"1.0.0","certFingerprint":")" + fingerprint + R"("})";
    const auto        rejected = m_server->post("/api/agent/register", missing, "application/json");
    EXPECT_EQ(403, rejected.status) << rejected.body;

    const std::string token = login();
    ASSERT_FALSE(token.empty());
    const auto issued = m_server->post("/api/enrollment-tokens", R"({"nodeName":"edge-token","action":"enroll"})", "application/json", bearer(token));
    ASSERT_EQ(200, issued.status) << issued.body;
    const auto issuedJson = issued.json();
    ASSERT_TRUE(issuedJson.has_value());
    const std::string plaintext = (*issuedJson)["data"]["token"].get<std::string>();

    const std::string accepted   = R"({"name":"edge-token","agentVersion":"1.0.0","certFingerprint":")" + fingerprint + R"(","enrollmentToken":")" + plaintext + R"("})";
    const auto        registered = m_server->post("/api/agent/register", accepted, "application/json");
    ASSERT_EQ(200, registered.status) << registered.body;
    const auto registeredJson = registered.json();
    ASSERT_TRUE(registeredJson.has_value());
    EXPECT_EQ("online", (*registeredJson)["data"]["status"].get<std::string>());
    EXPECT_EQ(fingerprint, (*registeredJson)["data"]["certFingerprint"].get<std::string>());
}
