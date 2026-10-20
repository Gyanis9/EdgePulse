#include "Auth/AuthController.h"
#include "Auth/AuthService.h"
#include "Base/Config/ConfigValue.h"
#include "Device/DeviceController.h"
#include "Device/NodeService.h"
#include "Net/Http/HttpRequest.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/HttpClientHarness.h"
#include "Web/MiddlewareSetup.h"

#include <gtest/gtest.h>

#include <cctype>
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

class NodeEndpointsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_context    = std::make_unique<EdgePulse::TestSupport::AuthTestContext>();
        m_repository = std::make_shared<EdgePulse::Device::NodeRepository>(m_context->database);
        m_nodes      = std::make_unique<EdgePulse::Device::NodeService>(m_repository);
        m_server     = std::make_unique<EdgePulse::TestSupport::ServerHarness>(
                [this](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                {
                    EdgePulse::Web::applySecurityHeaders(router);
                    EdgePulse::Auth::registerRoutes(router, loop, *m_context->auth, *m_context->captcha, true);
                    EdgePulse::Device::registerRoutes(router, loop, *m_nodes, [this](const AsynGyanis::Net::HttpRequest &request) { return resolveAccess(request); });
                });
    }

    void TearDown() override
    {
        m_server.reset();
        m_nodes.reset();
        m_repository.reset();
        m_context.reset();
    }

    std::optional<EdgePulse::Device::AccessContext> resolveAccess(const AsynGyanis::Net::HttpRequest &request)
    {
        // 测试专用：只给列表权限，用来验证写接口的 403 闸门
        if (const auto testHeader = request.getHeader("x-test-permissions"); testHeader == "list-only")
        {
            return EdgePulse::Device::AccessContext{{"device:node:list"}};
        }
        const std::optional<std::string> token = bearerToken(request);
        if (!token.has_value())
        {
            return std::nullopt;
        }
        const auto session = m_context->auth->authenticate(*token);
        if (!session.has_value())
        {
            return std::nullopt;
        }
        return EdgePulse::Device::AccessContext{session->permissions};
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
    std::shared_ptr<EdgePulse::Device::NodeRepository>       m_repository;
    std::unique_ptr<EdgePulse::Device::NodeService>          m_nodes;
    std::unique_ptr<EdgePulse::TestSupport::ServerHarness>   m_server;
};

// 钉住：登录后能创建、分页、更新与删除节点
TEST_F(NodeEndpointsTest, CreatesListsUpdatesAndDeletes)
{
    const std::string token = login();
    ASSERT_FALSE(token.empty());

    const std::string createBody = R"({"name":"edge-1","labels":{"site":"shanghai"}})";
    const auto        created    = m_server->post("/api/nodes", createBody, "application/json", bearer(token));
    ASSERT_EQ(200, created.status) << created.body;
    const auto createdJson = created.json();
    ASSERT_TRUE(createdJson.has_value());
    const std::int64_t nodeId  = (*createdJson)["data"]["id"].get<std::int64_t>();
    const std::int64_t version = (*createdJson)["data"]["version"].get<std::int64_t>();
    EXPECT_EQ("edge-1", (*createdJson)["data"]["name"].get<std::string>());
    EXPECT_EQ("offline", (*createdJson)["data"]["status"].get<std::string>());

    const auto list = m_server->get("/api/nodes?pageNum=1&pageSize=10", bearer(token));
    ASSERT_EQ(200, list.status) << list.body;
    const auto listJson = list.json();
    ASSERT_TRUE(listJson.has_value());
    EXPECT_EQ(1, (*listJson)["data"]["total"].get<std::int64_t>());
    ASSERT_EQ(1U, (*listJson)["data"]["rows"].size());
    EXPECT_EQ("edge-1", (*listJson)["data"]["rows"][0]["name"].get<std::string>());

    const std::string updateBody = R"({"name":"edge-1b","version":)" + std::to_string(version) + "}";
    const auto        updated    = m_server->send("PUT", "/api/nodes/" + std::to_string(nodeId), updateBody, "application/json", bearer(token));
    ASSERT_EQ(200, updated.status) << updated.body;
    const auto updatedJson = updated.json();
    ASSERT_TRUE(updatedJson.has_value());
    EXPECT_EQ("edge-1b", (*updatedJson)["data"]["name"].get<std::string>());
    EXPECT_EQ(version + 1, (*updatedJson)["data"]["version"].get<std::int64_t>());

    const auto removed = m_server->send("DELETE", "/api/nodes/" + std::to_string(nodeId), "", "", bearer(token));
    EXPECT_EQ(200, removed.status) << removed.body;

    const auto afterRemoval = m_server->get("/api/nodes", bearer(token));
    ASSERT_EQ(200, afterRemoval.status);
    const auto afterJson = afterRemoval.json();
    ASSERT_TRUE(afterJson.has_value());
    EXPECT_EQ(0, (*afterJson)["data"]["total"].get<std::int64_t>());
}

// 钉住：匿名 401；只有列表权限时写接口 403
TEST_F(NodeEndpointsTest, EnforcesAuthenticationAndPermission)
{
    const auto anonymous = m_server->get("/api/nodes");
    EXPECT_EQ(401, anonymous.status) << anonymous.body;

    const auto listOnly = m_server->get("/api/nodes", {{"x-test-permissions", "list-only"}});
    EXPECT_EQ(200, listOnly.status) << listOnly.body;

    const auto writeDenied = m_server->post("/api/nodes", R"({"name":"edge-2"})", "application/json", {{"x-test-permissions", "list-only"}});
    EXPECT_EQ(403, writeDenied.status) << writeDenied.body;
}
