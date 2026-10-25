#include "Base/Config/ConfigValue.h"
#include "Core/Cache/MemoryKeyValueStore.h"
#include "Core/Cache/RedisProbe.h"
#include "Core/Http/PublisherHealth.h"
#include "Core/Http/ReadinessState.h"
#include "Core/Http/RealtimeBus.h"
#include "Core/Http/SseBroadcaster.h"
#include "Core/Http/WebSocketEndpoints.h"
#include "Core/System/LeaderGate.h"
#include "Core/System/LeaderTick.h"
#include "Database/Redis/RedisConnection.h"
#include "TestSupport/EventLoopRunner.h"
#include "TestSupport/TemporaryDirectory.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <stdexcept>

using EdgePulse::TestSupport::EventLoopRunner;
using EdgePulse::TestSupport::TemporaryDirectory;

namespace
{
    /**
     * @brief 造一份全部依赖就绪的快照，用例只翻转其中一格
     * @return EdgePulse::Web::ReadinessDependencies 五格均为 true
     */
    [[nodiscard]] EdgePulse::Web::ReadinessDependencies allReady()
    {
        EdgePulse::Web::ReadinessDependencies dependencies;
        dependencies.databaseReady          = true;
        dependencies.serviceReady           = true;
        dependencies.cacheReady             = true;
        dependencies.realtimeHealthy        = true;
        dependencies.firmwareDirectoryReady = true;
        return dependencies;
    }

    /**
     * @brief 只实现探针用到的那几个虚函数的 Redis 连接替身
     * @details connect/probeAlive 按脚本返回，用来驱动「未连接 -> 连接」「PING 失败 -> 重连」
     *          两条恢复路径；不执行任何网络往返
     */
    class FakeRedisConnection final : public AsynGyanis::Database::RedisConnection
    {
    public:
        FakeRedisConnection() : RedisConnection(AsynGyanis::Database::ConnectionConfig::redisDefault())
        {
        }

        bool             connectResult{true};     ///< 下一次 connect 的结论
        bool             sessionConnected{false}; ///< 本端连接标志
        int              connectCalls{0};         ///< connect 调用次数
        int              disconnectCalls{0};      ///< disconnect 调用次数
        std::deque<bool> pingResults;             ///< 每次 probeAlive 的脚本结论

        /**
         * @brief 按脚本建立连接
         * @return bool connectResult
         */
        bool connect() override
        {
            ++connectCalls;
            if (!connectResult)
            {
                return false;
            }
            sessionConnected = true;
            return true;
        }

        /**
         * @brief 断开并清掉本端标志
         */
        void disconnect() override
        {
            ++disconnectCalls;
            sessionConnected = false;
        }

        /**
         * @brief 只读本端连接标志，和真实驱动一样不主动探测
         * @return bool 本端是否仍标记为已连接
         */
        [[nodiscard]] bool isConnected() const override
        {
            return sessionConnected;
        }

        /**
         * @brief 消耗一条 PING 脚本结论；失败时模拟驱动断开
         * @return bool 这一次 PING 是否收到回复
         */
        [[nodiscard]] bool probeAlive() noexcept override
        {
            if (pingResults.empty())
            {
                return false;
            }
            const bool alive = pingResults.front();
            pingResults.pop_front();
            if (!alive)
            {
                sessionConnected = false;
            }
            return alive;
        }

        /**
         * @brief 固定报告 Redis 类型
         * @return AsynGyanis::Database::DatabaseType Redis
         */
        [[nodiscard]] AsynGyanis::Database::DatabaseType databaseType() const override
        {
            return AsynGyanis::Database::DatabaseType::Redis;
        }
    };
} // namespace

// 钉住：五格依赖全绿时才就绪，reason 为空
TEST(Readiness, AllDependenciesReady)
{
    const AsynGyanis::Net::ReadinessCheckResult result = EdgePulse::Web::evaluateReadiness(allReady());
    EXPECT_TRUE(result.ready);
    EXPECT_TRUE(result.reason.empty());
}

// 钉住：任一格不就绪都给出 503 原因，且原因点名具体依赖
TEST(Readiness, ReportsEachUnreadyDependency)
{
    {
        EdgePulse::Web::ReadinessDependencies dependencies = allReady();
        dependencies.databaseReady                         = false;
        const AsynGyanis::Net::ReadinessCheckResult result = EdgePulse::Web::evaluateReadiness(dependencies);
        EXPECT_FALSE(result.ready);
        EXPECT_EQ("数据库探针失败", result.reason);
    }
    {
        EdgePulse::Web::ReadinessDependencies dependencies = allReady();
        dependencies.serviceReady                          = false;
        const AsynGyanis::Net::ReadinessCheckResult result = EdgePulse::Web::evaluateReadiness(dependencies);
        EXPECT_FALSE(result.ready);
        EXPECT_EQ("服务正在停机或尚未就绪", result.reason);
    }
    {
        EdgePulse::Web::ReadinessDependencies dependencies = allReady();
        dependencies.cacheReady                            = false;
        const AsynGyanis::Net::ReadinessCheckResult result = EdgePulse::Web::evaluateReadiness(dependencies);
        EXPECT_FALSE(result.ready);
        EXPECT_EQ("缓存探针失败", result.reason);
    }
    {
        EdgePulse::Web::ReadinessDependencies dependencies = allReady();
        dependencies.realtimeHealthy                       = false;
        const AsynGyanis::Net::ReadinessCheckResult result = EdgePulse::Web::evaluateReadiness(dependencies);
        EXPECT_FALSE(result.ready);
        EXPECT_EQ("实时事件总线不可用", result.reason);
    }
    {
        EdgePulse::Web::ReadinessDependencies dependencies = allReady();
        dependencies.firmwareDirectoryReady                = false;
        const AsynGyanis::Net::ReadinessCheckResult result = EdgePulse::Web::evaluateReadiness(dependencies);
        EXPECT_FALSE(result.ready);
        EXPECT_EQ("固件目录不可写", result.reason);
    }
}

// 钉住：就绪状态是原子快照，setter 改一格后 check 立刻反映
TEST(Readiness, StateSnapshotTracksSetters)
{
    EdgePulse::Web::ReadinessState state;
    EXPECT_FALSE(state.check().ready);

    state.setDatabaseReady(true);
    state.setServiceReady(true);
    state.setCacheReady(true);
    state.setRealtimeHealthy(true);
    state.setFirmwareDirectoryReady(true);
    EXPECT_TRUE(state.check().ready);
    EXPECT_TRUE(state.check().reason.empty());

    state.setFirmwareDirectoryReady(false);
    const AsynGyanis::Net::ReadinessCheckResult result = state.check();
    EXPECT_FALSE(result.ready);
    EXPECT_EQ("固件目录不可写", result.reason);
}

// 钉住：目录探针只在目录存在且能落一个临时文件时返回 true，缺失目录为 false
TEST(Readiness, ProbeDirectoryWritable)
{
    TemporaryDirectory directory;
    EXPECT_TRUE(EdgePulse::Web::probeDirectoryWritable(directory.path()));
    EXPECT_FALSE(EdgePulse::Web::probeDirectoryWritable(directory.path() / "missing"));

    std::error_code error;
    EXPECT_TRUE(std::filesystem::is_empty(directory.path(), error));
    EXPECT_FALSE(error);
}

// 钉住：Memory 后端的 probe 恒为 true
TEST(Readiness, MemoryCacheProbeSucceeds)
{
    EventLoopRunner                       runner;
    EdgePulse::Cache::MemoryKeyValueStore cache;
    EXPECT_TRUE(runner.run(cache.probe(runner.loop())));
}

// 钉住：总线未启用时恒健康；启用但从未 start 时发布与订阅连接都不可用
TEST(Readiness, RealtimeBusHealthRequiresConnectionsWhenEnabled)
{
    EventLoopRunner                  runner;
    EdgePulse::Web::BrowserWebSocket webSocket;
    EdgePulse::Web::SseBroadcaster   sse;

    EdgePulse::Web::RealtimeBusOptions disabledOptions;
    disabledOptions.enabled = false;
    EdgePulse::Web::RealtimeBus disabledBus(runner.loop(), webSocket, sse, disabledOptions);
    EXPECT_TRUE(disabledBus.isHealthy());

    EdgePulse::Web::RealtimeBusOptions enabledOptions;
    enabledOptions.enabled = true;
    enabledOptions.host    = "127.0.0.1";
    enabledOptions.port    = 1;
    EdgePulse::Web::RealtimeBus enabledBus(runner.loop(), webSocket, sse, enabledOptions);
    EXPECT_FALSE(enabledBus.isHealthy());
}

// 钉住：LeaderTick 在每个 tick 都询问门控；follower 不执行工作，当选后计数才前进
TEST(LeaderOnlyLoops, FollowerSkipsWorkUntilLeader)
{
    EventLoopRunner               runner;
    std::atomic<bool>             leader{false};
    std::atomic<int>              gateCalls{0};
    int                           workCount = 0;
    EdgePulse::System::LeaderGate gate(
            "测试周期任务", "test-node",
            [&leader, &gateCalls]
            {
                gateCalls.fetch_add(1, std::memory_order_relaxed);
                return leader.load(std::memory_order_acquire);
            },
            [] { return std::int64_t{7}; });
    EdgePulse::System::LeaderTick tick(gate,
                                       [&workCount]() -> AsynGyanis::Core::Task<>
                                       {
                                           ++workCount;
                                           co_return;
                                       });

    for (int round = 0; round < 3; ++round)
    {
        runner.run(tick.run());
    }
    EXPECT_EQ(0, workCount);
    EXPECT_EQ(3, gateCalls.load(std::memory_order_relaxed));

    leader.store(true, std::memory_order_release);
    for (int round = 0; round < 2; ++round)
    {
        runner.run(tick.run());
    }
    EXPECT_EQ(2, workCount);
    EXPECT_EQ(5, gateCalls.load(std::memory_order_relaxed));

    leader.store(false, std::memory_order_release);
    for (int round = 0; round < 2; ++round)
    {
        runner.run(tick.run());
    }
    EXPECT_EQ(2, workCount);
    EXPECT_EQ(7, gateCalls.load(std::memory_order_relaxed));
}

// 钉住：fleet 的离线扫描只在 leader 上执行，指标刷新作为每个 tick 都执行的工作保留在 follower 上
TEST(LeaderOnlyLoops, FleetSweepIsLeaderOnlyButMetricsRunEverywhere)
{
    EventLoopRunner               runner;
    std::atomic<bool>             leader{false};
    int                           sweepCount  = 0;
    int                           metricCount = 0;
    EdgePulse::System::LeaderGate gate("离线扫描与告警评估", "test-node", [&leader] { return leader.load(std::memory_order_acquire); });
    EdgePulse::System::LeaderTick tick(
            gate,
            [&sweepCount]() -> AsynGyanis::Core::Task<>
            {
                ++sweepCount;
                co_return;
            },
            [&metricCount]() -> AsynGyanis::Core::Task<>
            {
                ++metricCount;
                co_return;
            });

    for (int round = 0; round < 3; ++round)
    {
        runner.run(tick.run());
    }
    EXPECT_EQ(0, sweepCount);
    EXPECT_EQ(3, metricCount);

    leader.store(true, std::memory_order_release);
    runner.run(tick.run());
    EXPECT_EQ(1, sweepCount);
    EXPECT_EQ(4, metricCount);

    leader.store(false, std::memory_order_release);
    runner.run(tick.run());
    EXPECT_EQ(1, sweepCount);
    EXPECT_EQ(5, metricCount);
}

// 钉住：没有 leader 工作体的 tick 构造即拒绝，避免门控被空实现绕过
TEST(LeaderOnlyLoops, RejectsEmptyLeaderWork)
{
    std::atomic<bool>             leader{false};
    EdgePulse::System::LeaderGate gate("测试周期任务", "test-node", [&leader] { return leader.load(std::memory_order_acquire); });
    EXPECT_THROW(EdgePulse::System::LeaderTick(gate, {}), std::invalid_argument);
}

// 钉住：发布连接只在已判定不健康时探针；恢复后周期检查不再触发网络往返，失败标记后重新开始
TEST(RealtimeBus, PublisherHealthProbesOnlyWhenUnhealthy)
{
    EdgePulse::Web::PublisherHealth health;
    int                             probes    = 0;
    bool                            reachable = false;

    EXPECT_FALSE(health.isHealthy());
    EXPECT_FALSE(health.probe(
            [&probes, &reachable]
            {
                ++probes;
                return reachable;
            }));
    EXPECT_EQ(1, probes);

    reachable = true;
    EXPECT_TRUE(health.probe(
            [&probes, &reachable]
            {
                ++probes;
                return reachable;
            }));
    EXPECT_EQ(2, probes);
    EXPECT_TRUE(health.isHealthy());

    EXPECT_TRUE(health.probe(
            [&probes]
            {
                ++probes;
                return false;
            }));
    EXPECT_EQ(2, probes);

    health.markUnhealthy();
    EXPECT_TRUE(health.probe(
            [&probes]
            {
                ++probes;
                return true;
            }));
    EXPECT_EQ(3, probes);
}

// 钉住：跨实例信封带配置的实例标识，缺省实例标识非空且不会把空串写进信封
TEST(RealtimeBus, EnvelopeCarriesConfiguredInstanceId)
{
    EventLoopRunner                  runner;
    EdgePulse::Web::BrowserWebSocket webSocket;
    EdgePulse::Web::SseBroadcaster   sse;

    EdgePulse::Web::RealtimeBusOptions options;
    options.instanceId = "node-a";
    EdgePulse::Web::RealtimeBus bus(runner.loop(), webSocket, sse, options);
    EXPECT_EQ("node-a", bus.instanceId());

    const std::string envelope = EdgePulse::Web::makeRealtimeEnvelope(bus.instanceId(), "nodes", 9, R"({"type":"node.status"})");
    const auto        parsed   = AsynGyanis::Base::parseConfigValue(envelope);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->is_object());
    EXPECT_EQ("node-a", parsed->value("instance", std::string{}));
    EXPECT_EQ("nodes", parsed->value("base", std::string{}));
    EXPECT_EQ(9, parsed->value("tenantId", std::int64_t{0}));
    EXPECT_EQ(R"({"type":"node.status"})", parsed->value("payload", std::string{}));

    EdgePulse::Web::RealtimeBusOptions randomOptions;
    EdgePulse::Web::RealtimeBus        randomBus(runner.loop(), webSocket, sse, randomOptions);
    EXPECT_FALSE(randomBus.instanceId().empty());
    EXPECT_NE("node-a", randomBus.instanceId());
}

// 钉住：未连接时探针自己建立连接并 PING，成功后报告可用
TEST(RedisCacheProbe, ConnectsAndPingsDisconnectedStore)
{
    FakeRedisConnection connection;
    connection.pingResults = {true};
    EXPECT_TRUE(EdgePulse::Cache::probeRedisConnection(connection, "127.0.0.1:6379"));
    EXPECT_EQ(1, connection.connectCalls);
    EXPECT_EQ(0, connection.disconnectCalls);
}

// 钉住：PING 失败（连接已被服务端丢弃）时断开并只重连一次，重连后的 PING 成功即恢复
TEST(RedisCacheProbe, ReconnectsAfterPingFailure)
{
    FakeRedisConnection connection;
    connection.sessionConnected = true;
    connection.pingResults      = {false, true};
    EXPECT_TRUE(EdgePulse::Cache::probeRedisConnection(connection, "127.0.0.1:6379"));
    EXPECT_EQ(1, connection.connectCalls);
    ASSERT_EQ(1, connection.disconnectCalls);
}

// 钉住：重连失败返回 false；探针不能抛异常
TEST(RedisCacheProbe, ReportsFailureWhenReconnectFails)
{
    FakeRedisConnection connection;
    connection.sessionConnected = true;
    connection.pingResults      = {false};
    connection.connectResult    = false;
    EXPECT_FALSE(EdgePulse::Cache::probeRedisConnection(connection, "127.0.0.1:6379"));
    EXPECT_EQ(1, connection.connectCalls);
    EXPECT_EQ(1, connection.disconnectCalls);
}

// 钉住：首次建连失败同样返回 false，不再重复尝试
TEST(RedisCacheProbe, ReportsFailureWhenInitialConnectFails)
{
    FakeRedisConnection connection;
    connection.connectResult = false;
    EXPECT_FALSE(EdgePulse::Cache::probeRedisConnection(connection, "127.0.0.1:6379"));
    EXPECT_EQ(1, connection.connectCalls);
    EXPECT_EQ(0, connection.disconnectCalls);
}
