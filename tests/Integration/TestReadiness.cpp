#include "Core/Cache/MemoryKeyValueStore.h"
#include "Core/Http/ReadinessState.h"
#include "Core/Http/RealtimeBus.h"
#include "Core/Http/SseBroadcaster.h"
#include "Core/Http/WebSocketEndpoints.h"
#include "Core/System/LeaderGate.h"
#include "TestSupport/EventLoopRunner.h"
#include "TestSupport/TemporaryDirectory.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>

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

// 钉住：follower 的每个周期 tick 都不执行工作，只有当选后计数才前进
TEST(LeaderOnlyLoops, FollowerSkipsWorkUntilLeader)
{
    std::atomic<bool>             leader{false};
    std::atomic<int>              workCount{0};
    EdgePulse::System::LeaderGate gate("测试周期任务", "test-node", [&leader] { return leader.load(std::memory_order_acquire); }, [] { return std::int64_t{7}; });

    for (int tick = 0; tick < 3; ++tick)
    {
        if (gate.allowsWork())
        {
            workCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
    EXPECT_EQ(0, workCount.load(std::memory_order_relaxed));

    leader.store(true, std::memory_order_release);
    for (int tick = 0; tick < 2; ++tick)
    {
        if (gate.allowsWork())
        {
            workCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
    EXPECT_EQ(2, workCount.load(std::memory_order_relaxed));

    leader.store(false, std::memory_order_release);
    for (int tick = 0; tick < 2; ++tick)
    {
        if (gate.allowsWork())
        {
            workCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
    EXPECT_EQ(2, workCount.load(std::memory_order_relaxed));
}

// 钉住：同一个 gate 只在状态切换时改变判定，follower 期间重复判定不产生工作
TEST(LeaderOnlyLoops, RepeatedTicksAreIdempotentForFollowers)
{
    std::atomic<bool>             leader{false};
    EdgePulse::System::LeaderGate gate("测试周期任务", "test-node", [&leader] { return leader.load(std::memory_order_acquire); });

    for (int tick = 0; tick < 10; ++tick)
    {
        EXPECT_FALSE(gate.allowsWork());
    }
    leader.store(true, std::memory_order_release);
    EXPECT_TRUE(gate.allowsWork());
    EXPECT_TRUE(gate.allowsWork());
}
