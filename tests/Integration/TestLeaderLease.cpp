#include "Base/Database/Database.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/System/LeaderLease.h"
#include "Core/System/MigrationLock.h"
#include "Core/System/MigrationRunner.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

namespace
{
    /**
     * @brief 每个用例独立的临时数据库路径，避免并行时互相覆盖
     * @return std::filesystem::path 临时文件名
     */
    std::filesystem::path makeTemporaryDatabasePath()
    {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        return std::filesystem::temp_directory_path() / ("edgepulse-lease-" + suffix + ".db");
    }

    /**
     * @brief 应用仓库内的全部迁移，建出 sys_lease 与种子行
     * @param databasePath SQLite 文件路径
     */
    void applyMigrations(const std::filesystem::path &databasePath)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
        EdgePulse::System::MigrationRunner::apply(*connection, std::filesystem::path(EDGEPULSE_SOURCE_DIR) / "migrations");
    }

    /**
     * @brief 删除 SQLite 主文件与 WAL 边车文件
     * @param databasePath SQLite 文件路径
     */
    void removeDatabaseFiles(const std::filesystem::path &databasePath)
    {
        std::error_code error;
        std::filesystem::remove(databasePath, error);
        std::filesystem::remove(databasePath.string() + "-wal", error);
        std::filesystem::remove(databasePath.string() + "-shm", error);
    }

    /**
     * @brief 查询一行整数；失败时交回 -1，让断言带上 lastError
     * @param connection 已连接的数据库
     * @param sql 查询文本
     * @param parameters 绑定参数
     * @return std::int64_t 第一列整数
     */
    std::int64_t queryInteger(AsynGyanis::Database::DatabaseConnection &connection, const std::string &sql, const std::vector<AsynGyanis::Database::DatabaseValue> &parameters = {})
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(sql, parameters);
        EXPECT_NE(nullptr, result) << connection.lastError();
        if (!result)
        {
            return -1;
        }
        EXPECT_TRUE(result->next());
        const auto value = result->getValue(0);
        if (const auto *number = std::get_if<std::int64_t>(&value); number != nullptr)
        {
            return *number;
        }
        return -1;
    }

    /**
     * @brief 查询一行文本；失败或 NULL 时交回空串
     * @param connection 已连接的数据库
     * @param sql 查询文本
     * @return std::string 第一列文本
     */
    std::string queryText(AsynGyanis::Database::DatabaseConnection &connection, const std::string &sql)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(sql);
        EXPECT_NE(nullptr, result) << connection.lastError();
        if (!result)
        {
            return {};
        }
        EXPECT_TRUE(result->next());
        const auto value = result->getValue(0);
        if (const auto *text = std::get_if<std::string>(&value); text != nullptr)
        {
            return *text;
        }
        return {};
    }

    /**
     * @brief 轮询等待条件成立，避免用固定睡眠赌调度
     * @param predicate 条件
     * @param timeout 最长等待时间
     * @return bool 是否在时限内成立
     */
    bool waitUntil(const std::function<bool()> &predicate, const std::chrono::seconds timeout = std::chrono::seconds{5})
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return predicate();
    }
} // namespace

// 钉住：同一份 sys_lease 上只有一个 leader；A 释放后 B 才能接手并拿到 fence+1
TEST(LeaderLease, AcquiresAndTransfersOnlyAfterRelease)
{
    const std::filesystem::path databasePath = makeTemporaryDatabasePath();
    applyMigrations(databasePath);

    std::shared_ptr<EdgePulse::Common::Database> databaseA = EdgePulse::Common::Database::openSqlite(databasePath.string());
    std::shared_ptr<EdgePulse::Common::Database> databaseB = EdgePulse::Common::Database::openSqlite(databasePath.string());
    EdgePulse::TestSupport::EventLoopRunner      runner;
    {
        EdgePulse::System::LeaderLease leaseA(databaseA, "instance-a", "control-plane", std::chrono::seconds{10}, std::chrono::milliseconds{200});
        EdgePulse::System::LeaderLease leaseB(databaseB, "instance-b", "control-plane", std::chrono::seconds{10}, std::chrono::milliseconds{100});

        ASSERT_TRUE(runner.run(leaseA.start(runner.loop())));
        EXPECT_TRUE(leaseA.isLeader());
        EXPECT_EQ(1, leaseA.fence());

        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
            EXPECT_EQ("instance-a", queryText(*connection, "SELECT holder FROM sys_lease WHERE name = 'control-plane'"));
            EXPECT_EQ(1, queryInteger(*connection, "SELECT fence FROM sys_lease WHERE name = 'control-plane'"));
        }

        ASSERT_FALSE(runner.run(leaseB.start(runner.loop())));
        EXPECT_FALSE(leaseB.isLeader());
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        EXPECT_FALSE(leaseB.isLeader());
        EXPECT_TRUE(leaseA.isLeader());

        leaseA.stop();
        EXPECT_FALSE(leaseA.isLeader());
        ASSERT_TRUE(waitUntil([&leaseB] { return leaseB.isLeader(); })) << "A 停止后 B 未在时限内接手";
        EXPECT_EQ(2, leaseB.fence());
        EXPECT_EQ(1, leaseA.fence());

        // 重复 stop 不得再次清空或影响新 leader
        leaseA.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
        EXPECT_TRUE(leaseB.isLeader());
        EXPECT_EQ(2, leaseB.fence());
        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
            EXPECT_EQ("instance-b", queryText(*connection, "SELECT holder FROM sys_lease WHERE name = 'control-plane'"));
        }

        leaseB.stop();
        EXPECT_FALSE(leaseB.isLeader());
    }
    // 先关连接池再删文件，Windows 上打开中的 SQLite 文件删不掉
    databaseA.reset();
    databaseB.reset();
    removeDatabaseFiles(databasePath);
}

// 钉住：holder 不续约时 TTL 到期即可被接手，fence 递增；旧实例的 stop 只清自己的 holder
TEST(LeaderLease, ExpiredHolderYieldsToNextInstance)
{
    const std::filesystem::path databasePath = makeTemporaryDatabasePath();
    applyMigrations(databasePath);

    std::shared_ptr<EdgePulse::Common::Database> databaseA = EdgePulse::Common::Database::openSqlite(databasePath.string());
    std::shared_ptr<EdgePulse::Common::Database> databaseB = EdgePulse::Common::Database::openSqlite(databasePath.string());
    EdgePulse::TestSupport::EventLoopRunner      runner;
    {
        // A 的续约周期远长于 TTL：首次获取后不再延长，模拟进程停顿
        EdgePulse::System::LeaderLease leaseA(databaseA, "instance-a", "control-plane", std::chrono::seconds{2}, std::chrono::seconds{10});
        EdgePulse::System::LeaderLease leaseB(databaseB, "instance-b", "control-plane", std::chrono::seconds{2}, std::chrono::milliseconds{100});

        ASSERT_TRUE(runner.run(leaseA.start(runner.loop())));
        EXPECT_EQ(1, leaseA.fence());
        ASSERT_FALSE(runner.run(leaseB.start(runner.loop())));
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        EXPECT_FALSE(leaseB.isLeader()) << "A 的租约在 TTL 内不应被 B 抢走";

        ASSERT_TRUE(waitUntil([&leaseB] { return leaseB.isLeader(); }, std::chrono::seconds{8})) << "A 的租约到期后 B 未接手";
        EXPECT_EQ(2, leaseB.fence());

        // A 已失去 holder，stop 只能按 holder=instance-a 清空，不能碰 B 的行
        leaseA.stop();
        EXPECT_TRUE(leaseB.isLeader());
        EXPECT_EQ(2, leaseB.fence());

        leaseB.stop();
    }
    databaseA.reset();
    databaseB.reset();
    removeDatabaseFiles(databasePath);
}

// 钉住：holder/fence CAS 让旧 leader 的续约失败，isLeader 与本地 fence 一起停在旧值
TEST(LeaderLease, StaleHolderRenewalFailsAndFenceStays)
{
    const std::filesystem::path databasePath = makeTemporaryDatabasePath();
    applyMigrations(databasePath);

    std::shared_ptr<EdgePulse::Common::Database> database = EdgePulse::Common::Database::openSqlite(databasePath.string());
    EdgePulse::TestSupport::EventLoopRunner      runner;
    {
        EdgePulse::System::LeaderLease lease(database, "instance-a", "control-plane", std::chrono::seconds{10}, std::chrono::milliseconds{100});
        ASSERT_TRUE(runner.run(lease.start(runner.loop())));
        EXPECT_TRUE(lease.isLeader());
        EXPECT_EQ(1, lease.fence());

        // 模拟另一个实例在 A 停顿超过 TTL 后接手：holder 与 fence 一起被改写
        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
            const std::vector<AsynGyanis::Database::DatabaseValue> parameters{"instance-b", std::int64_t{2},
                                                                              EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::seconds{10}),
                                                                              EdgePulse::Common::nowDateTimeUtc(), "control-plane"};
            ASSERT_NE(nullptr, connection->execute("UPDATE sys_lease SET holder = ?, fence = ?, expires_at = ?, updated_at = ? WHERE name = ?", parameters))
                    << connection->lastError();
        }

        ASSERT_TRUE(waitUntil([&lease] { return !lease.isLeader(); })) << "旧 holder 的续约 CAS 未在时限内失败";
        EXPECT_EQ(1, lease.fence()) << "失去租约后本地 fence 应停在最后一次成功值";
        EXPECT_FALSE(lease.isLeader());

        lease.stop();
    }
    database.reset();
    removeDatabaseFiles(databasePath);
}

// 钉住：SQLite 是单写者，迁移锁返回无害的 no-op，release 与重复 release 都不抛
TEST(MigrationLock, SqliteIsNoOpAndReleaseIsIdempotent)
{
    const std::filesystem::path        databasePath = makeTemporaryDatabasePath();
    EdgePulse::Common::DatabaseOptions options;
    options.driver = "sqlite";
    options.path   = databasePath.string();

    {
        const auto first = EdgePulse::System::MigrationLock::acquire(options, 0);
        ASSERT_TRUE(first.has_value()) << first.error();
        EXPECT_NE(nullptr, first->get());

        // SQLite 不承诺多实例，第二个锁同样成功；这里只钉住不会把单实例卡在启动期
        const auto second = EdgePulse::System::MigrationLock::acquire(options, 0);
        EXPECT_TRUE(second.has_value()) << second.error();

        (*first)->release();
        (*first)->release();
        (*second)->release();
    }
    removeDatabaseFiles(databasePath);
}
