#include "Core/Auth/AuthService.h"
#include "Base/Database/Database.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace
{
    [[nodiscard]] std::int64_t countLoginLogs(const std::filesystem::path &databasePath, const int status)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
        const std::unique_ptr<AsynGyanis::Database::DatabaseResult>     result = connection->execute("SELECT COUNT(*) FROM sys_login_log WHERE status = " + std::to_string(status));
        EXPECT_NE(nullptr, result) << connection->lastError();
        if (!result || !result->next())
        {
            return -1;
        }
        const auto value = result->getValue(0);
        return std::holds_alternative<std::int64_t>(value) ? std::get<std::int64_t>(value) : -1;
    }

    [[nodiscard]] std::expected<EdgePulse::Auth::LoginResult, EdgePulse::Auth::AuthError>
    loginWithCaptcha(EdgePulse::TestSupport::AuthTestContext &context, EdgePulse::TestSupport::EventLoopRunner &runner, const std::string &password)
    {
        const auto challenge = runner.run(context.captcha->issue(runner.loop(), "127.0.0.1"));
        if (!challenge.has_value())
        {
            return std::unexpected(EdgePulse::Auth::AuthError{429, 429, "测试验证码签发失败"});
        }
        return runner.run(context.auth->login(runner.loop(), EdgePulse::Auth::LoginCredentials{"admin", password, challenge->code, challenge->uuid, "127.0.0.1", "test-trace"}));
    }
} // namespace

// 钉住：种子账号能登录，身份、角色、权限、菜单与成功审计都成立
TEST(AuthService, LogsInAndExposesIdentityRoutesAndAudit)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;

    auto result = loginWithCaptcha(context, runner, "admin123");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(64U, result->token.size());
    EXPECT_EQ(7200, result->expiresIn);

    const std::optional<EdgePulse::Auth::SessionRecord> session = runner.run(context.auth->authenticate(runner.loop(), result->token));
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ("admin", session->username);
    EXPECT_NE(session->permissions.end(), std::find(session->permissions.begin(), session->permissions.end(), "device:node:list"));
    EXPECT_NE(session->permissions.end(), std::find(session->permissions.begin(), session->permissions.end(), "device:node:write"));

    const std::vector<EdgePulse::System::MenuNode> tree = runner.run(context.auth->routes(runner.loop(), *session));
    const auto deviceGroup                              = std::find_if(tree.begin(), tree.end(), [](const EdgePulse::System::MenuNode &node) { return node.name == "边缘运维"; });
    ASSERT_NE(tree.end(), deviceGroup);
    const auto node = std::find_if(deviceGroup->children.begin(), deviceGroup->children.end(), [](const EdgePulse::System::MenuNode &child) { return child.name == "节点"; });
    ASSERT_NE(deviceGroup->children.end(), node);
    EXPECT_EQ("device/Nodes", node->component);

    EXPECT_EQ(1, countLoginLogs(context.databasePath(), 1));
    runner.run(context.auth->logout(runner.loop(), result->token));
    EXPECT_FALSE(runner.run(context.auth->authenticate(runner.loop(), result->token)).has_value());
}

// 钉住：错误口令与错误验证码都按可恢复错误返回，正确凭据随后仍能登录
TEST(AuthService, RejectsWrongCaptchaAndWrongPassword)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;

    // 先消费一条验证码，再用同一个 uuid 登录：必须被拒
    const auto firstChallenge = runner.run(context.captcha->issue(runner.loop(), "127.0.0.1"));
    ASSERT_TRUE(firstChallenge.has_value());
    const auto wrongCaptcha =
            runner.run(context.auth->login(runner.loop(), EdgePulse::Auth::LoginCredentials{"admin", "admin123", "XXXX", firstChallenge->uuid, "127.0.0.1", "trace"}));
    ASSERT_FALSE(wrongCaptcha.has_value());
    EXPECT_EQ(400, wrongCaptcha.error().httpStatus);

    const auto wrongPassword = loginWithCaptcha(context, runner, "not-the-password");
    ASSERT_FALSE(wrongPassword.has_value());
    EXPECT_EQ(401, wrongPassword.error().httpStatus);

    const auto success = loginWithCaptcha(context, runner, "admin123");
    ASSERT_TRUE(success.has_value()) << success.error().message;
}

// 钉住：改密后旧口令失效、新口令可用；登出会删除会话
TEST(AuthService, ChangesPasswordAndInvalidatesOldCredential)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;

    const auto firstLogin = loginWithCaptcha(context, runner, "admin123");
    ASSERT_TRUE(firstLogin.has_value()) << firstLogin.error().message;
    const std::optional<EdgePulse::Auth::SessionRecord> session = runner.run(context.auth->authenticate(runner.loop(), firstLogin->token));
    ASSERT_TRUE(session.has_value());

    const auto changed = runner.run(context.auth->changePassword(runner.loop(), *session, "admin123", "new-pass-123"));
    ASSERT_TRUE(changed.has_value()) << changed.error().message;

    runner.run(context.auth->logout(runner.loop(), firstLogin->token));
    EXPECT_FALSE(runner.run(context.auth->authenticate(runner.loop(), firstLogin->token)).has_value());

    const auto oldCredential = loginWithCaptcha(context, runner, "admin123");
    ASSERT_FALSE(oldCredential.has_value());
    EXPECT_EQ(401, oldCredential.error().httpStatus);

    const auto newCredential = loginWithCaptcha(context, runner, "new-pass-123");
    ASSERT_TRUE(newCredential.has_value()) << newCredential.error().message;
}
