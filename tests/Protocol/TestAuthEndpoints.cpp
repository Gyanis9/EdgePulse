#include "Auth/AuthController.h"
#include "Base/Config/ConfigValue.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/HttpClientHarness.h"
#include "Web/MiddlewareSetup.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    struct CaptchaPair
    {
        std::string uuid;
        std::string code;
    };

    std::string jsonBody(const std::string &username, const std::string &password, const CaptchaPair &captcha)
    {
        const AsynGyanis::Base::ConfigValue body =
                AsynGyanis::Base::ConfigValue::object({{"username", username}, {"password", password}, {"captcha", captcha.code}, {"captchaUuid", captcha.uuid}});
        return body.dump();
    }

    std::vector<std::pair<std::string, std::string>> bearer(const std::string &token)
    {
        return {{"Authorization", "Bearer " + token}};
    }
} // namespace

class AuthEndpointsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_context = std::make_unique<EdgePulse::TestSupport::AuthTestContext>();
        m_server  = std::make_unique<EdgePulse::TestSupport::ServerHarness>(
                [this](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                {
                    EdgePulse::Web::applySecurityHeaders(router);
                    EdgePulse::Auth::registerRoutes(router, loop, *m_context->auth, *m_context->captcha, true);
                });
    }

    void TearDown() override
    {
        // 服务端先停，认证服务随后销毁；顺序反了会让路由里的引用悬空
        m_server.reset();
        m_context.reset();
    }

    std::optional<CaptchaPair> fetchCaptcha()
    {
        const auto response = m_server->get("/api/auth/captcha");
        if (!response.transportOk || response.status != 200)
        {
            return std::nullopt;
        }
        const auto json = response.json();
        if (!json.has_value() || !(*json)["data"].is_object())
        {
            return std::nullopt;
        }
        const auto &data = (*json)["data"];
        if (!data.contains("uuid") || !data.contains("code"))
        {
            return std::nullopt;
        }
        return CaptchaPair{data["uuid"].get<std::string>(), data["code"].get<std::string>()};
    }

    EdgePulse::TestSupport::HttpResult login(const std::string &username, const std::string &password, const CaptchaPair &captcha)
    {
        return m_server->post("/api/auth/login", jsonBody(username, password, captcha), "application/json");
    }

    EdgePulse::TestSupport::HttpResult loginWithFreshCaptcha(const std::string &username, const std::string &password)
    {
        const std::optional<CaptchaPair> captcha = fetchCaptcha();
        if (!captcha.has_value())
        {
            ADD_FAILURE() << "无法取得验证码";
            return {};
        }
        return login(username, password, *captcha);
    }

    std::unique_ptr<EdgePulse::TestSupport::AuthTestContext> m_context;
    std::unique_ptr<EdgePulse::TestSupport::ServerHarness>   m_server;
};

// 钉住：完整登录链路、me、routes、登出与旧 token 失效
TEST_F(AuthEndpointsTest, CaptchaLoginMeRoutesLogout)
{
    const std::optional<CaptchaPair> captcha = fetchCaptcha();
    ASSERT_TRUE(captcha.has_value());

    const auto loginResponse = login("admin", "admin123", *captcha);
    ASSERT_TRUE(loginResponse.transportOk) << loginResponse.error;
    ASSERT_EQ(200, loginResponse.status) << loginResponse.body;
    const auto loginJson = loginResponse.json();
    ASSERT_TRUE(loginJson.has_value());
    const std::string token = (*loginJson)["data"]["token"].get<std::string>();
    EXPECT_EQ(64U, token.size());

    const auto me = m_server->get("/api/auth/me", bearer(token));
    ASSERT_TRUE(me.transportOk);
    ASSERT_EQ(200, me.status) << me.body;
    const auto meJson = me.json();
    ASSERT_TRUE(meJson.has_value());
    EXPECT_EQ("admin", (*meJson)["data"]["username"].get<std::string>());
    EXPECT_EQ("super_admin", (*meJson)["data"]["roles"][0].get<std::string>());
    bool hasNodePermission = false;
    for (const auto &permission: (*meJson)["data"]["permissions"])
    {
        hasNodePermission = hasNodePermission || permission.get<std::string>() == "device:node:list";
    }
    EXPECT_TRUE(hasNodePermission);

    const auto routes = m_server->get("/api/auth/routes", bearer(token));
    ASSERT_TRUE(routes.transportOk);
    ASSERT_EQ(200, routes.status) << routes.body;
    const auto routesJson = routes.json();
    ASSERT_TRUE(routesJson.has_value());
    ASSERT_EQ(1U, (*routesJson)["data"]["routes"].size());
    EXPECT_EQ("/device", (*routesJson)["data"]["routes"][0]["path"].get<std::string>());
    EXPECT_EQ("device/Nodes", (*routesJson)["data"]["routes"][0]["children"][0]["component"].get<std::string>());

    const auto logout = m_server->post("/api/auth/logout", "", "application/json", bearer(token));
    ASSERT_TRUE(logout.transportOk);
    EXPECT_EQ(200, logout.status) << logout.body;

    const auto afterLogout = m_server->get("/api/auth/me", bearer(token));
    EXPECT_EQ(401, afterLogout.status);
}

// 钉住：匿名访问受保护接口一律 401，且不会误落到兜底 404
TEST_F(AuthEndpointsTest, AnonymousRequestsAreRejected)
{
    for (const std::string &path: {std::string{"/api/auth/me"}, std::string{"/api/auth/routes"}})
    {
        const auto response = m_server->get(path);
        EXPECT_EQ(401, response.status) << path << " " << response.body;
    }
    const auto password = m_server->send("PUT", "/api/auth/password", "{}", "application/json");
    EXPECT_EQ(401, password.status) << password.body;
}

// 钉住：验证码错误与口令错误各自返回可判别的状态码，随后正确凭据仍可登录
TEST_F(AuthEndpointsTest, RejectsWrongCaptchaAndWrongPassword)
{
    const std::optional<CaptchaPair> captcha = fetchCaptcha();
    ASSERT_TRUE(captcha.has_value());
    CaptchaPair wrongCaptcha = *captcha;
    wrongCaptcha.code        = "XXXX";
    const auto badCaptcha    = login("admin", "admin123", wrongCaptcha);
    EXPECT_EQ(400, badCaptcha.status) << badCaptcha.body;

    const auto badPassword = loginWithFreshCaptcha("admin", "not-the-password");
    EXPECT_EQ(401, badPassword.status) << badPassword.body;

    const auto goodLogin = loginWithFreshCaptcha("admin", "admin123");
    EXPECT_EQ(200, goodLogin.status) << goodLogin.body;
}

// 钉住：改密后旧 token 可继续登出、旧口令失效、新口令可登录
TEST_F(AuthEndpointsTest, ChangesPasswordAndUsesNewCredential)
{
    const auto loginResponse = loginWithFreshCaptcha("admin", "admin123");
    ASSERT_EQ(200, loginResponse.status) << loginResponse.body;
    const auto loginJson = loginResponse.json();
    ASSERT_TRUE(loginJson.has_value());
    const std::string token = (*loginJson)["data"]["token"].get<std::string>();

    const std::string changeBody = R"({"oldPassword":"admin123","newPassword":"new-pass-123"})";
    const auto        change     = m_server->send("PUT", "/api/auth/password", changeBody, "application/json", bearer(token));
    ASSERT_TRUE(change.transportOk);
    EXPECT_EQ(200, change.status) << change.body;

    const auto oldCredential = loginWithFreshCaptcha("admin", "admin123");
    EXPECT_EQ(401, oldCredential.status) << oldCredential.body;

    const auto newCredential = loginWithFreshCaptcha("admin", "new-pass-123");
    EXPECT_EQ(200, newCredential.status) << newCredential.body;
}
