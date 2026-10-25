#include "Core/Auth/AuthService.h"
#include "Base/Database/Database.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/TimeFormat.h"
#include "Base/Security/PasswordHash.h"
#include "Core/System/UserRepository.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <barrier>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
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

    /// 直接落一条已过期的锁定与达到阈值的失败计数，复现“锁定短于失败窗口”的残留状态
    void seedExpiredLock(const std::filesystem::path &databasePath)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
        const std::string now  = EdgePulse::Common::nowDateTimeUtc();
        const std::string past = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() - std::chrono::seconds{60});
        const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection->execute(
                "UPDATE sys_user SET failed_login_count = 2, last_failed_login_at = '" + now + "', locked_until = '" + past + "' WHERE username = 'admin'");
        EXPECT_NE(nullptr, result) << connection->lastError();
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

// 钉住：连续失败达到阈值后账号锁定，触发锁定的那次仍是 401，正确口令在锁定期内也回 423
TEST(AuthService, LocksAccountAfterRepeatedFailures)
{
    // 锁定 90 秒用于核对提示按分钟向上取整：旧算法会显示「1 分钟」，实际还差半分钟才解锁
    EdgePulse::TestSupport::AuthTestContext context({10, true, 900, 2, 90});
    EdgePulse::TestSupport::EventLoopRunner runner;

    const auto first = loginWithCaptcha(context, runner, "wrong-1");
    ASSERT_FALSE(first.has_value());
    EXPECT_EQ(401, first.error().httpStatus);
    const auto second = loginWithCaptcha(context, runner, "wrong-2");
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(401, second.error().httpStatus);
    const auto locked = loginWithCaptcha(context, runner, "admin123");
    ASSERT_FALSE(locked.has_value());
    EXPECT_EQ(423, locked.error().httpStatus);
    EXPECT_NE(std::string::npos, locked.error().message.find("2 分钟"));

    // 管理员重置口令会清空锁定，新口令立即可用
    EdgePulse::System::UserRepository users(context.database);
    const auto                        admin = runner.run(users.findByUsername(runner.loop(), "admin"));
    ASSERT_TRUE(admin.has_value());
    const std::string hash = EdgePulse::Common::hashPassword("admin123");
    ASSERT_TRUE(runner.run(users.scoped({1, false})->resetPassword(runner.loop(), admin->id, hash, 1)));
    const auto recovered = loginWithCaptcha(context, runner, "admin123");
    ASSERT_TRUE(recovered.has_value()) << recovered.error().message;
}

// 钉住：锁定已过期而失败窗口还没过时，先清账号失败态；下一次输错不会立刻重新锁定
TEST(AuthService, ExpiredLockClearsAccountFailureStateBeforeVerification)
{
    // 锁定 300 秒短于失败窗口 900 秒：不清零时，锁过期后计数仍停在阈值上
    EdgePulse::TestSupport::AuthTestContext context({6, false, 900, 2, 300});
    EdgePulse::TestSupport::EventLoopRunner runner;
    seedExpiredLock(context.databasePath());

    const auto wrong = loginWithCaptcha(context, runner, "wrong-after-expiry");
    ASSERT_FALSE(wrong.has_value());
    EXPECT_EQ(401, wrong.error().httpStatus);
    // 修复前这次输错会把计数推回阈值并重新锁定 300 秒，正确口令会收到 423
    const auto recovered = loginWithCaptcha(context, runner, "admin123");
    ASSERT_TRUE(recovered.has_value()) << recovered.error().message;
}

// 钉住：两条连接并发记录失败时计数不会互相覆盖，最终至少达到阈值且写入锁定
TEST(AuthService, ConcurrentFailuresReachThresholdWithoutOverwrite)
{
    EdgePulse::TestSupport::AuthTestContext context({6, false, 900, 4, 60});
    EdgePulse::TestSupport::EventLoopRunner runnerA;
    EdgePulse::TestSupport::EventLoopRunner runnerB;
    auto                                    usersA = std::make_shared<EdgePulse::System::UserRepository>(context.database, EdgePulse::System::TenantScope{1, false});
    auto                                    usersB = std::make_shared<EdgePulse::System::UserRepository>(context.database, EdgePulse::System::TenantScope{1, false});

    const auto admin = runnerA.run(usersA->findByUsername(runnerA.loop(), "admin"));
    ASSERT_TRUE(admin.has_value());

    constexpr int perWorker       = 3;
    constexpr int maximumFailures = 4;
    std::barrier  barrier{2};
    std::exception_ptr failureA;
    std::exception_ptr failureB;
    // 两条线程都到齐后才开始写，避免退化成先后执行
    const auto worker = [&barrier, userId = admin->id, perWorker, maximumFailures](EdgePulse::TestSupport::EventLoopRunner &runner, EdgePulse::System::UserRepository &users,
                                                                                    std::exception_ptr &failure)
    {
        barrier.arrive_and_wait();
        try
        {
            for (int attempt = 0; attempt < perWorker; ++attempt)
            {
                // 返回值只表示是否触发锁定；本用例关心落库后的最终不变量
                static_cast<void>(runner.run(users.recordLoginFailure(runner.loop(), userId, 900, maximumFailures, 60)));
            }
        } catch (...)
        {
            failure = std::current_exception();
        }
    };
    std::thread threadA(worker, std::ref(runnerA), std::ref(*usersA), std::ref(failureA));
    std::thread threadB(worker, std::ref(runnerB), std::ref(*usersB), std::ref(failureB));
    threadA.join();
    threadB.join();

    for (const std::exception_ptr &failure: {failureA, failureB})
    {
        if (failure != nullptr)
        {
            try
            {
                std::rethrow_exception(failure);
            } catch (const std::exception &error)
            {
                FAIL() << "并发记录登录失败抛出异常：" << error.what();
            } catch (...)
            {
                FAIL() << "并发记录登录失败抛出未知异常";
            }
        }
    }

    const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(context.databasePath().string());
    const auto                                                      result =
            connection->execute("SELECT failed_login_count, locked_until FROM sys_user WHERE id = " + std::to_string(admin->id));
    ASSERT_NE(nullptr, result) << connection->lastError();
    ASSERT_TRUE(result->next());
    EXPECT_GE(EdgePulse::Sql::integer(*result, "failed_login_count"), maximumFailures);
    const std::string lockedUntil = EdgePulse::Sql::dateTimeText(*result, "locked_until");
    EXPECT_FALSE(lockedUntil.empty());
    EXPECT_GT(lockedUntil, EdgePulse::Common::nowDateTimeUtc());
}

// 钉住：成功登录会清空账号失败计数，此前的输错不参与后续累计
TEST(AuthService, SuccessfulLoginResetsFailureCount)
{
    EdgePulse::TestSupport::AuthTestContext context({6, false, 900, 2, 60});
    EdgePulse::TestSupport::EventLoopRunner runner;

    EXPECT_FALSE(loginWithCaptcha(context, runner, "wrong-1").has_value());
    ASSERT_TRUE(loginWithCaptcha(context, runner, "admin123").has_value());
    EXPECT_FALSE(loginWithCaptcha(context, runner, "wrong-2").has_value());
    const auto success = loginWithCaptcha(context, runner, "admin123");
    ASSERT_TRUE(success.has_value()) << success.error().message;
}

// 钉住：改密与建号共用同一口令策略，弱口令在落库前被拒
TEST(AuthService, RejectsWeakPasswordOnChangeUnderStrictPolicy)
{
    EdgePulse::TestSupport::AuthTestContext context({10, true, 900, 5, 900});
    EdgePulse::TestSupport::EventLoopRunner runner;

    const auto login = loginWithCaptcha(context, runner, "admin123");
    ASSERT_TRUE(login.has_value()) << login.error().message;
    const std::optional<EdgePulse::Auth::SessionRecord> session = runner.run(context.auth->authenticate(runner.loop(), login->token));
    ASSERT_TRUE(session.has_value());

    const auto weak = runner.run(context.auth->changePassword(runner.loop(), *session, "admin123", "weakpass1"));
    ASSERT_FALSE(weak.has_value());
    EXPECT_EQ(400, weak.error().httpStatus);

    const auto strong = runner.run(context.auth->changePassword(runner.loop(), *session, "admin123", "Strong-pass1"));
    ASSERT_TRUE(strong.has_value()) << strong.error().message;
}

// 钉住：种子管理员仍是 admin123 时，环境变量口令在启动检查里替换哈希，旧口令随即失效
TEST(AuthService, DefaultAdminPasswordGuardAppliesEnvironmentPassword)
{
    EdgePulse::TestSupport::AuthTestContext        context;
    EdgePulse::TestSupport::EventLoopRunner        runner;
    const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection =
            EdgePulse::Common::Database::openDirectSqlite(context.databasePath().string());
    ASSERT_NE(nullptr, connection);

    const auto result = EdgePulse::System::UserRepository::enforceDefaultAdminPassword(
            *connection, EdgePulse::System::DefaultAdminPasswordGuardOptions{.password = "Ha-guard-pass1", .developmentMode = false});
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_TRUE(result.applied);
    EXPECT_FALSE(loginWithCaptcha(context, runner, "admin123").has_value());
    EXPECT_TRUE(loginWithCaptcha(context, runner, "Ha-guard-pass1").has_value());
}

// 钉住：生产档没有环境变量口令且仍是种子口令时拒绝启动；开发档保留 admin123
TEST(AuthService, DefaultAdminPasswordGuardFailsClosedInProductionAndAllowsDev)
{
    EdgePulse::TestSupport::AuthTestContext        context;
    EdgePulse::TestSupport::EventLoopRunner        runner;
    const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection =
            EdgePulse::Common::Database::openDirectSqlite(context.databasePath().string());
    ASSERT_NE(nullptr, connection);

    const auto production = EdgePulse::System::UserRepository::enforceDefaultAdminPassword(
            *connection, EdgePulse::System::DefaultAdminPasswordGuardOptions{.password = "", .developmentMode = false});
    EXPECT_FALSE(production.applied);
    EXPECT_NE(std::string::npos, production.error.find("EDGEPULSE_ADMIN_PASSWORD")) << production.error;
    EXPECT_NE(std::string::npos, production.error.find("admin123")) << production.error;
    EXPECT_TRUE(loginWithCaptcha(context, runner, "admin123").has_value());

    const auto weak = EdgePulse::System::UserRepository::enforceDefaultAdminPassword(
            *connection, EdgePulse::System::DefaultAdminPasswordGuardOptions{.password = "short", .developmentMode = false});
    EXPECT_FALSE(weak.applied);
    EXPECT_NE(std::string::npos, weak.error.find("口令策略")) << weak.error;

    const auto development = EdgePulse::System::UserRepository::enforceDefaultAdminPassword(
            *connection, EdgePulse::System::DefaultAdminPasswordGuardOptions{.password = "", .developmentMode = true});
    EXPECT_TRUE(development.error.empty()) << development.error;
    EXPECT_FALSE(development.applied);
    EXPECT_TRUE(loginWithCaptcha(context, runner, "admin123").has_value());
}

// 钉住：管理员已改口令时忽略环境变量，既不覆盖新哈希也不阻断启动
TEST(AuthService, DefaultAdminPasswordGuardLeavesChangedPasswordUntouched)
{
    EdgePulse::TestSupport::AuthTestContext        context;
    EdgePulse::TestSupport::EventLoopRunner        runner;
    const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection =
            EdgePulse::Common::Database::openDirectSqlite(context.databasePath().string());
    ASSERT_NE(nullptr, connection);
    const std::string changedHash = EdgePulse::Common::hashPassword("Changed-pass1");
    ASSERT_NE(nullptr, connection->execute("UPDATE sys_user SET password = '" + changedHash + "' WHERE username = 'admin'")) << connection->lastError();

    const auto result = EdgePulse::System::UserRepository::enforceDefaultAdminPassword(
            *connection, EdgePulse::System::DefaultAdminPasswordGuardOptions{.password = "Ignored-pass1", .developmentMode = false});
    EXPECT_TRUE(result.error.empty()) << result.error;
    EXPECT_FALSE(result.applied);
    EXPECT_FALSE(loginWithCaptcha(context, runner, "Ignored-pass1").has_value());
    EXPECT_TRUE(loginWithCaptcha(context, runner, "Changed-pass1").has_value());
}
