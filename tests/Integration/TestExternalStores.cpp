/**
 * @file TestExternalStores.cpp
 * @brief 真机 MySQL 与 Redis 用例：缺环境变量即跳过，不伪造通过
 * @author Gyanis
 * @date 2026-10-20
 */

#include "Base/Database/Database.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Auth/LoginAudit.h"
#include "Core/Cache/RedisKeyValueStore.h"
#include "Core/System/LogRepository.h"
#include "Core/System/MigrationLock.h"
#include "Core/System/MigrationRunner.h"
#include "Database/Redis/RedisConnection.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <variant>

namespace
{
    [[nodiscard]] std::optional<std::string> environmentText(const char *name)
    {
#if defined(_MSC_VER)
        char  *buffer = nullptr;
        size_t length = 0;
        if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr)
        {
            return std::nullopt;
        }
        std::string value{buffer};
        std::free(buffer);
        if (value.empty())
        {
            return std::nullopt;
        }
        return value;
#else
        const char *value = std::getenv(name);
        if (value == nullptr || *value == '\0')
        {
            return std::nullopt;
        }
        return std::string{value};
#endif
    }

    [[nodiscard]] std::uint16_t environmentPort(const char *name, const std::uint16_t fallback)
    {
        const std::optional<std::string> text = environmentText(name);
        if (!text.has_value())
        {
            return fallback;
        }
        return static_cast<std::uint16_t>(std::stoul(*text));
    }

    [[nodiscard]] std::int64_t queryCount(AsynGyanis::Database::DatabaseConnection &connection, const std::string &sql,
                                          const std::span<const AsynGyanis::Database::DatabaseValue> parameters = {})
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseResult> rows = connection.execute(sql, parameters);
        if (!rows || !rows->next())
        {
            return -1;
        }
        const auto value = rows->getValue(0);
        if (const auto *number = std::get_if<std::int64_t>(&value); number != nullptr)
        {
            return *number;
        }
        return -1;
    }
} // namespace

// 钉住：MySQL 上能完整应用二十四版迁移，V7 归属列与 V15 至 V24 的注册、配置、签名、登录加固、企业身份、证书轮换、租约、灰度与任务命令唯一索引生效，日志仓储按部门范围过滤
TEST(ExternalStores, MySqlAppliesMigrationsAndScopesLogs)
{
    const std::optional<std::string> host = environmentText("EDGEPULSE_TEST_MYSQL_HOST");
    if (!host.has_value())
    {
        GTEST_SKIP() << "缺少 EDGEPULSE_TEST_MYSQL_HOST，跳过真实 MySQL 用例";
    }
    const std::optional<std::string> user     = environmentText("EDGEPULSE_TEST_MYSQL_USER");
    const std::optional<std::string> password = environmentText("EDGEPULSE_TEST_MYSQL_PASSWORD");
    const std::optional<std::string> database = environmentText("EDGEPULSE_TEST_MYSQL_DATABASE");

    EdgePulse::Common::DatabaseOptions options;
    options.driver          = "mysql";
    options.host            = *host;
    options.port            = environmentPort("EDGEPULSE_TEST_MYSQL_PORT", 3306);
    options.userName        = user.value_or("root");
    options.password        = password.value_or("");
    options.database        = database.value_or("edgepulse_test");
    options.maximumPoolSize = 4;

    {
        // GET_LOCK 是连接级的：同一进程里的第二条连接必须等第一条释放才能拿到同名锁
        const auto first = EdgePulse::System::MigrationLock::acquire(options, 5);
        ASSERT_TRUE(first.has_value()) << first.error();
        const auto blocked = EdgePulse::System::MigrationLock::acquire(options, 1);
        EXPECT_FALSE(blocked.has_value());
        (*first)->release();
        const auto reacquired = EdgePulse::System::MigrationLock::acquire(options, 5);
        ASSERT_TRUE(reacquired.has_value()) << reacquired.error();
        (*reacquired)->release();
    }

    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirect(options);
        EdgePulse::System::MigrationRunner::apply(*connection, std::filesystem::path(EDGEPULSE_SOURCE_DIR) / "migrations");
        EdgePulse::System::MigrationRunner::apply(*connection, std::filesystem::path(EDGEPULSE_SOURCE_DIR) / "migrations");
        EXPECT_EQ(24, queryCount(*connection, "SELECT COUNT(*) AS total FROM schema_migrations"));
        EXPECT_EQ(11, queryCount(*connection,
                                 "SELECT COUNT(*) AS total FROM information_schema.tables WHERE table_schema = ? AND table_name IN "
                                 "('biz_agent_enrollment', 'biz_node_config', 'biz_node_config_revision', 'biz_agent_certificate', 'sys_api_token', 'sys_idempotency', "
                                 "'sys_user_mfa', 'sys_lease', 'biz_firmware_rollout', 'biz_firmware_rollout_wave', 'biz_firmware_rollout_target')",
                                 std::array<AsynGyanis::Database::DatabaseValue, 1>{options.database}));
        EXPECT_EQ(3, queryCount(*connection,
                                "SELECT COUNT(*) AS total FROM information_schema.columns WHERE table_schema = ? AND table_name = 'biz_firmware_task' AND "
                                "column_name IN ('rollout_id', 'wave_id', 'command_id')",
                                std::array<AsynGyanis::Database::DatabaseValue, 1>{options.database}));
        EXPECT_EQ(1, queryCount(*connection,
                                "SELECT COUNT(*) AS total FROM information_schema.columns WHERE table_schema = ? AND table_name = 'biz_node' AND "
                                "column_name = 'installed_firmware_version'",
                                std::array<AsynGyanis::Database::DatabaseValue, 1>{options.database}));
        EXPECT_EQ(1, queryCount(*connection, "SELECT COUNT(*) AS total FROM sys_lease WHERE name = 'control-plane' AND holder = '' AND fence = 0"));
        EXPECT_EQ(2, queryCount(*connection,
                                "SELECT COUNT(*) AS total FROM information_schema.columns WHERE table_schema = ? AND table_name = 'biz_node' AND column_name IN "
                                "('previous_cert_fingerprint', 'previous_cert_not_after')",
                                std::array<AsynGyanis::Database::DatabaseValue, 1>{options.database}));
        EXPECT_EQ(1, queryCount(*connection,
                                "SELECT COUNT(*) AS total FROM information_schema.columns WHERE table_schema = ? AND table_name = 'biz_firmware' AND "
                                "column_name = 'signature'",
                                std::array<AsynGyanis::Database::DatabaseValue, 1>{options.database}));
        EXPECT_EQ(1, queryCount(*connection,
                                "SELECT COUNT(*) AS total FROM information_schema.columns WHERE table_schema = ? AND table_name = 'sys_user' AND "
                                "column_name = 'locked_until'",
                                std::array<AsynGyanis::Database::DatabaseValue, 1>{options.database}));
        EXPECT_EQ(1, queryCount(*connection, "SELECT COUNT(*) AS total FROM sys_user WHERE username = 'admin'"));
        EXPECT_EQ(2, queryCount(*connection,
                                "SELECT COUNT(*) AS total FROM information_schema.columns WHERE table_schema = ? AND table_name = 'sys_oper_log' AND column_name IN "
                                "('oper_id', 'dept_id')",
                                std::array<AsynGyanis::Database::DatabaseValue, 1>{options.database}));
        EXPECT_EQ(2, queryCount(*connection,
                                "SELECT COUNT(*) AS total FROM information_schema.columns WHERE table_schema = ? AND table_name = 'sys_login_log' AND column_name IN "
                                "('user_id', 'dept_id')",
                                std::array<AsynGyanis::Database::DatabaseValue, 1>{options.database}));
    }

    const std::shared_ptr<EdgePulse::Common::Database> databaseHandle = EdgePulse::Common::Database::open(options);
    EdgePulse::TestSupport::EventLoopRunner            runner;
    EdgePulse::System::LogRepository                   logs(databaseHandle);
    EdgePulse::Auth::LoginAudit                        audit(databaseHandle);

    const std::int64_t baseDept = 900000 + static_cast<std::int64_t>(std::chrono::steady_clock::now().time_since_epoch().count() % 100000);
    const std::string  tag      = "mysql-test-" + std::to_string(baseDept);

    EdgePulse::System::OperLogRecord first;
    first.module   = tag;
    first.action   = "写入";
    first.operName = "mysql-user";
    first.operId   = 7;
    first.deptId   = baseDept;
    first.operTime = EdgePulse::Common::nowDateTimeUtc();
    ASSERT_TRUE(runner.run(logs.insertOperLog(runner.loop(), first)));

    EdgePulse::System::OperLogRecord second = first;
    second.operName                         = "other-user";
    second.operId                           = 8;
    second.deptId                           = baseDept + 1;
    ASSERT_TRUE(runner.run(logs.insertOperLog(runner.loop(), second)));
    ASSERT_TRUE(runner.run(audit.record(runner.loop(), EdgePulse::Auth::LoginAuditEntry{"mysql-user", "127.0.0.1", true, "ok", tag, 7, baseDept})));

    EdgePulse::System::LogScope scope;
    scope.all            = false;
    scope.selfOnly       = false;
    scope.deptIds        = {baseDept};
    const auto operPage  = runner.run(logs.listOperLogs(runner.loop(), tag, "", EdgePulse::Common::PageParameters{1, 10}, scope));
    const auto loginPage = runner.run(logs.listLoginLogs(runner.loop(), "", "", EdgePulse::Common::PageParameters{1, 10}, scope));
    EXPECT_EQ(1, operPage.total);
    ASSERT_EQ(1U, operPage.rows.size());
    EXPECT_EQ("mysql-user", operPage.rows[0].operName);
    EXPECT_EQ(baseDept, operPage.rows[0].deptId);
    EXPECT_EQ(1, loginPage.total);
    ASSERT_EQ(1U, loginPage.rows.size());
    EXPECT_EQ(7, loginPage.rows[0].userId);
    EXPECT_EQ(baseDept, loginPage.rows[0].deptId);

    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirect(options);
        const std::array<AsynGyanis::Database::DatabaseValue, 1>        parameters{tag};
        ASSERT_NE(nullptr, connection->execute("DELETE FROM sys_oper_log WHERE module = ?", parameters));
        ASSERT_NE(nullptr, connection->execute("DELETE FROM sys_login_log WHERE trace_id = ?", parameters));
    }
}

// 钉住：Redis 键值存储的读、写、删除、原子消费、自增与过期语义走真实服务端
TEST(ExternalStores, RedisKeyValueStoreRoundTrip)
{
    const std::optional<std::string> host = environmentText("EDGEPULSE_TEST_REDIS_HOST");
    if (!host.has_value())
    {
        GTEST_SKIP() << "缺少 EDGEPULSE_TEST_REDIS_HOST，跳过真实 Redis 用例";
    }
    const std::optional<std::string> password = environmentText("EDGEPULSE_TEST_REDIS_PASSWORD");

    EdgePulse::Cache::RedisKeyValueStore    store(*host, environmentPort("EDGEPULSE_TEST_REDIS_PORT", 6379), password.value_or(""), 15);
    EdgePulse::TestSupport::EventLoopRunner runner;
    EXPECT_TRUE(runner.run(store.probe(runner.loop())));

    const std::string base = "edgepulse:acceptance:" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    EXPECT_FALSE(runner.run(store.get(runner.loop(), base)).has_value());
    ASSERT_TRUE(runner.run(store.set(runner.loop(), base, "值一", std::chrono::seconds{30})));
    const auto stored = runner.run(store.get(runner.loop(), base));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ("值一", *stored);

    // 一次性消费走服务端 GETDEL：第二次取值必须为空，读接口也不能再看到该键
    const std::string oneTime = base + ":take";
    ASSERT_TRUE(runner.run(store.set(runner.loop(), oneTime, "一次性", std::chrono::seconds{30})));
    const auto consumed = runner.run(store.take(runner.loop(), oneTime));
    ASSERT_TRUE(consumed.has_value());
    EXPECT_EQ("一次性", *consumed);
    EXPECT_FALSE(runner.run(store.take(runner.loop(), oneTime)).has_value());
    EXPECT_FALSE(runner.run(store.get(runner.loop(), oneTime)).has_value());

    const std::string counter = base + ":count";
    EXPECT_EQ(1, runner.run(store.increment(runner.loop(), counter, std::chrono::seconds{30})));
    EXPECT_EQ(2, runner.run(store.increment(runner.loop(), counter, std::chrono::seconds{30})));

    const std::string expiring = base + ":ttl";
    ASSERT_TRUE(runner.run(store.set(runner.loop(), expiring, "temporary", std::chrono::seconds{1})));
    std::optional<std::string> remaining;
    for (int attempt = 0; attempt < 30; ++attempt)
    {
        remaining = runner.run(store.get(runner.loop(), expiring));
        if (!remaining.has_value())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    EXPECT_FALSE(remaining.has_value()) << "键未在 1 秒过期后消失";

    EXPECT_TRUE(runner.run(store.erase(runner.loop(), base)));
    EXPECT_FALSE(runner.run(store.get(runner.loop(), base)).has_value());
    EXPECT_TRUE(runner.run(store.erase(runner.loop(), counter)));
}

// 钉住：连接被服务端单方面丢弃后，probe 会重连并重新 PING，随后普通缓存操作照常可用
TEST(ExternalStores, RedisCacheProbeRecoversAfterConnectionLoss)
{
    const std::optional<std::string> host = environmentText("EDGEPULSE_TEST_REDIS_HOST");
    if (!host.has_value())
    {
        GTEST_SKIP() << "缺少 EDGEPULSE_TEST_REDIS_HOST，跳过真实 Redis 用例";
    }
    const std::optional<std::string> password = environmentText("EDGEPULSE_TEST_REDIS_PASSWORD");
    const std::uint16_t              port     = environmentPort("EDGEPULSE_TEST_REDIS_PORT", 6379);

    EdgePulse::Cache::RedisKeyValueStore    store(*host, port, password.value_or(""), 15);
    EdgePulse::TestSupport::EventLoopRunner runner;
    ASSERT_TRUE(runner.run(store.probe(runner.loop())));

    AsynGyanis::Database::ConnectionConfig adminConfig = AsynGyanis::Database::ConnectionConfig::redisDefault();
    adminConfig.host                                   = *host;
    adminConfig.port                                   = port;
    adminConfig.password                               = password.value_or("");
    adminConfig.database                               = "15";
    AsynGyanis::Database::RedisConnection admin(adminConfig);
    ASSERT_TRUE(admin.connect()) << admin.lastError();

    // SKIPME 保住管理连接本身，只踢掉同一服务端上的普通客户端，也就是缓存存储那条连接
    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> killed = admin.executeCommand({"CLIENT", "KILL", "TYPE", "normal", "SKIPME", "yes"});
    ASSERT_NE(nullptr, killed) << admin.lastError();
    ASSERT_TRUE(killed->next());
    const auto  killedValue = killed->getValue(0);
    const auto *killedCount = std::get_if<std::int64_t>(&killedValue);
    ASSERT_NE(nullptr, killedCount);
    EXPECT_GE(*killedCount, 1);

    EXPECT_TRUE(runner.run(store.probe(runner.loop()))) << "探针没有在连接丢失后恢复";

    const std::string key = "edgepulse:probe-recovery:" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    ASSERT_TRUE(runner.run(store.set(runner.loop(), key, "恢复后", std::chrono::seconds{30})));
    const auto stored = runner.run(store.get(runner.loop(), key));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ("恢复后", *stored);
    EXPECT_TRUE(runner.run(store.erase(runner.loop(), key)));
}
