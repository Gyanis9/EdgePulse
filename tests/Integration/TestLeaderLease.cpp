#include "Base/Database/Database.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/System/LeaderLease.h"
#include "Core/System/LeaseDeadline.h"
#include "Core/System/MigrationLock.h"
#include "Core/System/MigrationRunner.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
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

    /**
     * @brief 把惰性任务的布尔结果转交给 promise，供不经过 EventLoopRunner::run 的用例等待
     * @param task 待执行的任务
     * @param promise 结果出口，异常同样经它交回
     * @return AsynGyanis::Core::Task<> 包装协程，活到结果就位之后
     */
    AsynGyanis::Core::Task<> forwardBooleanResult(AsynGyanis::Core::Task<bool> task, std::shared_ptr<std::promise<bool>> promise)
    {
        try
        {
            promise->set_value(co_await task);
        } catch (...)
        {
            promise->set_exception(std::current_exception());
        }
    }

    /**
     * @brief 在事件循环线程上占住循环，等到数据库出现指定 holder 后再继续占住一段时间
     * @param databasePath SQLite 文件路径
     * @param holder 期望出现在 sys_lease 里的 holder
     * @param hold 看到 holder 之后继续占用循环的时长
     * @return AsynGyanis::Core::Task<> 占住期间循环上的协程恢复全部被推迟
     * @note 故意在循环线程上做阻塞查询与睡眠：本函数模拟的正是事件循环停摆，
     *       让授权在工作线程算完之后、恢复之前隔开一段可观测的延迟
     */
    AsynGyanis::Core::Task<> stallLoopAfterGrant(const std::filesystem::path &databasePath, const std::string &holder, const std::chrono::milliseconds hold)
    {
        const auto giveUp  = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        bool       granted = false;
        while (std::chrono::steady_clock::now() < giveUp)
        {
            bool matched = false;
            try
            {
                const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
                const std::unique_ptr<AsynGyanis::Database::DatabaseResult>     result     = connection->execute("SELECT holder FROM sys_lease WHERE name = 'control-plane'");
                if (result && result->next())
                {
                    const auto value = result->getValue(0);
                    if (const auto *text = std::get_if<std::string>(&value); text != nullptr)
                    {
                        matched = *text == holder;
                    }
                }
                connection->disconnect();
            } catch (const std::exception &)
            {
                matched = false;
            }
            if (matched)
            {
                granted = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        if (granted)
        {
            std::this_thread::sleep_for(hold);
        }
        co_return;
    }
} // namespace

// 钉住：本地截止锚定在工作线程取到授权的同一时刻，恢复延迟多久都不会把它推后
TEST(LeaseDeadline, AnchorsToWorkerInstantInsteadOfResume)
{
    using namespace std::chrono;
    const auto         wallNow   = system_clock::time_point{seconds{1'800'000'000}} + milliseconds{700};
    const auto         rowExpiry = wallNow + seconds{2};
    const std::int64_t steadyNow = 1'000'000'000;

    const auto deadline = EdgePulse::System::leaseDeadlineNanoseconds(steadyNow, wallNow, rowExpiry, seconds{2});

    // 库内到期只到秒：wallNow 的 700 毫秒被截掉，截止距取到授权的时刻 1.3 秒
    EXPECT_EQ(steadyNow + 1'300'000'000LL, deadline);
    EXPECT_LE(deadline, steadyNow + duration_cast<nanoseconds>(seconds{2}).count()) << "本地截止不能超过 TTL";

    // 授权后循环停摆两秒才恢复：截止早已过去，isLeader 的判据必须为 false
    const std::int64_t resumeAt = steadyNow + duration_cast<nanoseconds>(seconds{2}).count();
    EXPECT_LT(deadline, resumeAt) << "恢复时刻晚于截止时仍被判为 leader";
}

// 钉住：任意亚秒起点的截断都只会让截止更早，不会越过行到期
TEST(LeaseDeadline, TruncationNeverExtendsPastRowExpiry)
{
    using namespace std::chrono;
    const auto         wallBase  = system_clock::time_point{seconds{1'800'000'000}};
    const std::int64_t steadyNow = 5'000'000'000;
    for (int offset = 0; offset < 1000; ++offset)
    {
        const auto wallNow   = wallBase + milliseconds{offset};
        const auto rowExpiry = wallNow + seconds{2};
        const auto deadline  = EdgePulse::System::leaseDeadlineNanoseconds(steadyNow, wallNow, rowExpiry, seconds{2});
        const auto anchored  = steadyNow + duration_cast<nanoseconds>(floor<seconds>(rowExpiry) - wallNow).count();
        EXPECT_LE(deadline, anchored) << "offset=" << offset << " 的截止越过了行到期";
    }
}

// 钉住：取到授权时行已过期，截止就是取到授权的时刻，判据当场为 false
TEST(LeaseDeadline, ExpiredRowYieldsCaptureInstant)
{
    using namespace std::chrono;
    const auto         wallNow   = system_clock::time_point{seconds{1'800'000'000}} + milliseconds{250};
    const std::int64_t steadyNow = 42;

    const auto expired = EdgePulse::System::leaseDeadlineNanoseconds(steadyNow, wallNow, wallNow - milliseconds{1}, seconds{2});
    EXPECT_EQ(steadyNow, expired);
    EXPECT_FALSE(steadyNow < expired) << "行已过期时仍被判为 leader";
    EXPECT_FALSE(steadyNow + 1 < expired);
}

// 钉住：行到期远在 TTL 之外（时钟回拨一类）时截止被夹在 TTL 内，不会拉长 leadership
TEST(LeaseDeadline, ClampsToTtlWhenRowExpiryIsFarAhead)
{
    using namespace std::chrono;
    const auto         wallNow   = system_clock::time_point{seconds{1'800'000'000}} + milliseconds{400};
    const std::int64_t steadyNow = 7;
    const auto         deadline  = EdgePulse::System::leaseDeadlineNanoseconds(steadyNow, wallNow, wallNow + seconds{30}, seconds{2});
    EXPECT_EQ(steadyNow + duration_cast<nanoseconds>(seconds{2}).count(), deadline);
}

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
        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
            EXPECT_EQ("instance-b", queryText(*connection, "SELECT holder FROM sys_lease WHERE name = 'control-plane'")) << "A 的 stop 不能清掉 B 已经接手的 holder";
        }
        // 跨过 B 的多个续约周期：行仍归 B，B 也没有被 A 的 stop 挤下去
        std::this_thread::sleep_for(std::chrono::milliseconds{350});
        EXPECT_TRUE(leaseB.isLeader());
        EXPECT_EQ(2, leaseB.fence());
        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
            EXPECT_EQ("instance-b", queryText(*connection, "SELECT holder FROM sys_lease WHERE name = 'control-plane'"));
            EXPECT_EQ(2, queryInteger(*connection, "SELECT fence FROM sys_lease WHERE name = 'control-plane'"));
        }

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

        // 只改 fence、保持 holder：续约必须因 fence 不匹配而失败，而不是被 holder 分支放行
        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
            const std::vector<AsynGyanis::Database::DatabaseValue>          parameters{std::int64_t{2}, "control-plane"};
            ASSERT_NE(nullptr, connection->execute("UPDATE sys_lease SET fence = ? WHERE name = ?", parameters)) << connection->lastError();
        }

        ASSERT_TRUE(waitUntil([&lease] { return !lease.isLeader(); })) << "旧 holder 的续约 CAS 未在时限内失败";
        EXPECT_EQ(1, lease.fence()) << "失去租约后本地 fence 应停在最后一次成功值";
        EXPECT_FALSE(lease.isLeader());
        // holder 仍是自己且未过期：后续获取也不能把自己重新抬成 leader
        std::this_thread::sleep_for(std::chrono::milliseconds{350});
        EXPECT_FALSE(lease.isLeader()) << "holder 未变时不应凭 holder=自己 重新获取";
        EXPECT_EQ(1, lease.fence());
        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
            EXPECT_EQ("instance-a", queryText(*connection, "SELECT holder FROM sys_lease WHERE name = 'control-plane'"));
            EXPECT_EQ(2, queryInteger(*connection, "SELECT fence FROM sys_lease WHERE name = 'control-plane'"));
        }

        lease.stop();
    }
    database.reset();
    removeDatabaseFiles(databasePath);
}

// 钉住：两个进程配了同一个 instance_id 时，获取谓词不接受 holder=自己，后来者必须等 TTL 过期
TEST(LeaderLease, DuplicatedInstanceIdWaitsForExpiry)
{
    const std::filesystem::path databasePath = makeTemporaryDatabasePath();
    applyMigrations(databasePath);

    std::shared_ptr<EdgePulse::Common::Database> databaseA = EdgePulse::Common::Database::openSqlite(databasePath.string());
    std::shared_ptr<EdgePulse::Common::Database> databaseB = EdgePulse::Common::Database::openSqlite(databasePath.string());
    EdgePulse::TestSupport::EventLoopRunner      runner;
    {
        // A 首次获取后 5 秒才续约一次，TTL 2 秒：行在 A 仍自认 leader 时进入可接手状态，
        // 续约点也远晚于 B 的接手点，避免两个动作在 TTL 边界抢同一秒
        EdgePulse::System::LeaderLease leaseA(databaseA, "instance-a", "control-plane", std::chrono::seconds{2}, std::chrono::seconds{5});
        EdgePulse::System::LeaderLease leaseB(databaseB, "instance-a", "control-plane", std::chrono::seconds{2}, std::chrono::milliseconds{100});

        ASSERT_TRUE(runner.run(leaseA.start(runner.loop())));
        EXPECT_EQ(1, leaseA.fence());
        ASSERT_FALSE(runner.run(leaseB.start(runner.loop())));
        // TTL 未到时，同一个 instance_id 不能共享 holder
        std::this_thread::sleep_for(std::chrono::milliseconds{800});
        EXPECT_FALSE(leaseB.isLeader()) << "TTL 未到时同一 instance_id 不应抢到租约";
        EXPECT_TRUE(leaseA.isLeader());

        // TTL 过后 B 接手；A 在下一次续约时发现 fence CAS 失败并降级
        ASSERT_TRUE(waitUntil([&leaseB] { return leaseB.isLeader(); }, std::chrono::seconds{5})) << "TTL 过后 B 未接手";
        EXPECT_EQ(2, leaseB.fence());
        ASSERT_TRUE(waitUntil([&leaseA] { return !leaseA.isLeader(); }, std::chrono::seconds{6})) << "A 的续约在 B 接手后未失败";
        EXPECT_EQ(1, leaseA.fence());

        // B 继续续约：跨过多个续约周期仍保持 leader，且行里的 fence 不再变化
        std::this_thread::sleep_for(std::chrono::milliseconds{350});
        EXPECT_TRUE(leaseB.isLeader());
        EXPECT_EQ(2, leaseB.fence());
        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
            EXPECT_EQ("instance-a", queryText(*connection, "SELECT holder FROM sys_lease WHERE name = 'control-plane'"));
            EXPECT_EQ(2, queryInteger(*connection, "SELECT fence FROM sys_lease WHERE name = 'control-plane'"));
        }

        leaseB.stop();
    }
    databaseA.reset();
    databaseB.reset();
    removeDatabaseFiles(databasePath);
}

// 钉住：续约周期远长于 TTL 时，本地单调截止一到 isLeader 就转 false，不等下一次续约
TEST(LeaderLease, LocalDeadlineDemotesWithoutRenewTick)
{
    const std::filesystem::path databasePath = makeTemporaryDatabasePath();
    applyMigrations(databasePath);

    std::shared_ptr<EdgePulse::Common::Database> database = EdgePulse::Common::Database::openSqlite(databasePath.string());
    EdgePulse::TestSupport::EventLoopRunner      runner;
    {
        // 循环只在启动时续约一次，之后 30 秒不会再动；截止必须由本地 steady_clock 判定
        EdgePulse::System::LeaderLease lease(database, "instance-a", "control-plane", std::chrono::seconds{1}, std::chrono::seconds{30});
        ASSERT_TRUE(runner.run(lease.start(runner.loop())));
        EXPECT_TRUE(lease.isLeader());
        EXPECT_EQ(1, lease.fence());

        ASSERT_TRUE(waitUntil([&lease] { return !lease.isLeader(); }, std::chrono::seconds{4})) << "本地截止已过，isLeader 仍报 leader";
        EXPECT_FALSE(lease.isLeader());
        EXPECT_EQ(1, lease.fence()) << "本地截止降级不应改写 fence";
        lease.stop();
    }
    database.reset();
    removeDatabaseFiles(databasePath);
}

// 钉住：授权在工作线程算完后事件循环被拖住，恢复时本地截止不得越过库内到期
TEST(LeaderLease, DeferredResumeCannotExtendLocalDeadline)
{
    const std::filesystem::path databasePath = makeTemporaryDatabasePath();
    applyMigrations(databasePath);

    std::shared_ptr<EdgePulse::Common::Database> database = EdgePulse::Common::Database::openSqlite(databasePath.string());
    EdgePulse::TestSupport::EventLoopRunner      runner;
    {
        // TTL 2 秒、续约 30 秒：测试期间不会再有续约刷新截止，只看首次授权
        EdgePulse::System::LeaderLease lease(database, "instance-a", "control-plane", std::chrono::seconds{2}, std::chrono::seconds{30});

        // 先按住写锁，保证授权 SQL 一定在循环被拖住之后才落库：恢复必然晚于授权
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> writer = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
        ASSERT_NE(nullptr, writer->execute("BEGIN IMMEDIATE")) << writer->lastError();

        const auto                   promise   = std::make_shared<std::promise<bool>>();
        std::future<bool>            future    = promise->get_future();
        AsynGyanis::Core::Task<bool> startTask = lease.start(runner.loop());
        AsynGyanis::Core::Task<>     forward   = forwardBooleanResult(std::move(startTask), promise);
        // 占住循环 2.6 秒，超过 TTL 与一秒的时间截断余量，恢复点一定落在行到期之后
        AsynGyanis::Core::Task<> stall = stallLoopAfterGrant(databasePath, "instance-a", std::chrono::milliseconds{2600});
        runner.loop().scheduler().scheduleRemote(forward.handle());
        runner.loop().scheduler().scheduleRemote(stall.handle());

        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        ASSERT_NE(nullptr, writer->execute("COMMIT")) << writer->lastError();
        writer->disconnect();

        ASSERT_EQ(std::future_status::ready, future.wait_for(std::chrono::seconds{8})) << "租约协程未在时限内恢复";
        const bool acquired = future.get();

        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
            EXPECT_EQ("instance-a", queryText(*connection, "SELECT holder FROM sys_lease WHERE name = 'control-plane'")) << "授权没有落库，用例没有走到延迟恢复的场景";
        }

        // start 的返回值来自恢复时刻的 isLeader()：旧实现把剩余有效期加在恢复时刻上，
        // 这里会报告成功；本地截止若已锚定在授权时刻，恢复时必然已经过去
        EXPECT_FALSE(acquired) << "恢复晚于库内到期时 start 仍报告获取成功，说明本地截止被延长到了行到期之后";
        lease.stop();
    }
    database.reset();
    removeDatabaseFiles(databasePath);
}

// 钉住：行已被改成过期后，延迟到达的续约不能延长它；只能重新获取并递增 fence
TEST(LeaderLease, DelayedRenewCannotExtendExpiredRow)
{
    const std::filesystem::path databasePath = makeTemporaryDatabasePath();
    applyMigrations(databasePath);

    std::shared_ptr<EdgePulse::Common::Database> database = EdgePulse::Common::Database::openSqlite(databasePath.string());
    EdgePulse::TestSupport::EventLoopRunner      runner;
    {
        // 本地截止 2 秒、续约 500 毫秒：手工把行改成过期后，下一次续约仍发生在本地截止之内
        EdgePulse::System::LeaderLease lease(database, "instance-a", "control-plane", std::chrono::seconds{2}, std::chrono::milliseconds{500});
        ASSERT_TRUE(runner.run(lease.start(runner.loop())));
        EXPECT_TRUE(lease.isLeader());
        EXPECT_EQ(1, lease.fence());

        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
            const std::vector<AsynGyanis::Database::DatabaseValue>          parameters{"2000-01-01 00:00:00", "control-plane"};
            ASSERT_NE(nullptr, connection->execute("UPDATE sys_lease SET expires_at = ? WHERE name = ?", parameters)) << connection->lastError();
            EXPECT_EQ("2000-01-01 00:00:00", queryText(*connection, "SELECT expires_at FROM sys_lease WHERE name = 'control-plane'"));
        }

        // 续约必须因 expires_at > now 不成立而失败；重新获取会递增 fence，
        // 旧实现会把过期行直接续到未来，fence 永远停在 1
        ASSERT_TRUE(waitUntil([&lease] { return lease.fence() == 2; }, std::chrono::seconds{6})) << "过期行被续约延长，或过期后没有重新获取";
        EXPECT_TRUE(lease.isLeader());
        EXPECT_EQ(2, lease.fence());
        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
            EXPECT_EQ("instance-a", queryText(*connection, "SELECT holder FROM sys_lease WHERE name = 'control-plane'"));
            EXPECT_EQ(2, queryInteger(*connection, "SELECT fence FROM sys_lease WHERE name = 'control-plane'"));
        }
        lease.stop();
    }
    database.reset();
    removeDatabaseFiles(databasePath);
}

// 钉住：同一个 instance_id 的两个实例，A 的 stop 只清自己 fence 的行，不能清掉 B 已接手的行
TEST(LeaderLease, StopWithStaleFenceDoesNotClearDuplicateHolder)
{
    const std::filesystem::path databasePath = makeTemporaryDatabasePath();
    applyMigrations(databasePath);

    std::shared_ptr<EdgePulse::Common::Database> databaseA = EdgePulse::Common::Database::openSqlite(databasePath.string());
    std::shared_ptr<EdgePulse::Common::Database> databaseB = EdgePulse::Common::Database::openSqlite(databasePath.string());
    EdgePulse::TestSupport::EventLoopRunner      runner;
    {
        EdgePulse::System::LeaderLease leaseA(databaseA, "shared-instance", "control-plane", std::chrono::seconds{2}, std::chrono::seconds{10});
        EdgePulse::System::LeaderLease leaseB(databaseB, "shared-instance", "control-plane", std::chrono::seconds{10}, std::chrono::milliseconds{100});

        ASSERT_TRUE(runner.run(leaseA.start(runner.loop())));
        EXPECT_EQ(1, leaseA.fence());
        ASSERT_FALSE(runner.run(leaseB.start(runner.loop())));
        ASSERT_TRUE(waitUntil([&leaseB] { return leaseB.isLeader(); }, std::chrono::seconds{8})) << "A 过期后 B 未接手";
        EXPECT_EQ(2, leaseB.fence());

        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection       = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
        const std::string                                               expiryBeforeStop = queryText(*connection, "SELECT expires_at FROM sys_lease WHERE name = 'control-plane'");

        leaseA.stop();
        EXPECT_EQ("shared-instance", queryText(*connection, "SELECT holder FROM sys_lease WHERE name = 'control-plane'")) << "A 的 stop 清掉了 B 同 instance_id 的行";
        EXPECT_EQ(2, queryInteger(*connection, "SELECT fence FROM sys_lease WHERE name = 'control-plane'"));
        ASSERT_TRUE(waitUntil([&connection, &expiryBeforeStop]
                              { return queryText(*connection, "SELECT expires_at FROM sys_lease WHERE name = 'control-plane'") > expiryBeforeStop; }, std::chrono::seconds{3}))
                << "A 停止后 B 的续约没有继续";
        EXPECT_TRUE(leaseB.isLeader());
        EXPECT_EQ(2, leaseB.fence());

        leaseB.stop();
    }
    databaseA.reset();
    databaseB.reset();
    removeDatabaseFiles(databasePath);
}

// 钉住：一次续约数据库异常不立即降级，本地截止内保留 leadership，恢复后沿用原 fence
TEST(LeaderLease, TransientRenewFailureKeepsLeaderUntilDeadline)
{
    const std::filesystem::path databasePath = makeTemporaryDatabasePath();
    applyMigrations(databasePath);

    std::shared_ptr<EdgePulse::Common::Database> database = EdgePulse::Common::Database::openSqlite(databasePath.string());
    EdgePulse::TestSupport::EventLoopRunner      runner;
    {
        EdgePulse::System::LeaderLease lease(database, "instance-a", "control-plane", std::chrono::seconds{5}, std::chrono::milliseconds{100});
        ASSERT_TRUE(runner.run(lease.start(runner.loop())));
        EXPECT_TRUE(lease.isLeader());
        EXPECT_EQ(1, lease.fence());

        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
        const std::string expiryBeforeFailure                                      = queryText(*connection, "SELECT expires_at FROM sys_lease WHERE name = 'control-plane'");

        // 表暂时不可见，期间至少跨过两个续约周期，续约只可能抛异常
        ASSERT_NE(nullptr, connection->execute("ALTER TABLE sys_lease RENAME TO sys_lease_offline")) << connection->lastError();
        std::this_thread::sleep_for(std::chrono::milliseconds{350});
        EXPECT_TRUE(lease.isLeader()) << "一次续约异常就降级，没有按本地截止保留 leadership";
        EXPECT_EQ(1, lease.fence());

        ASSERT_NE(nullptr, connection->execute("ALTER TABLE sys_lease_offline RENAME TO sys_lease")) << connection->lastError();
        ASSERT_TRUE(waitUntil([&connection, &expiryBeforeFailure]
                              { return queryText(*connection, "SELECT expires_at FROM sys_lease WHERE name = 'control-plane'") > expiryBeforeFailure; }, std::chrono::seconds{5}))
                << "异常恢复后续约没有继续";
        EXPECT_TRUE(lease.isLeader());
        EXPECT_EQ(1, lease.fence()) << "续约异常后不应重新走获取流程";
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
