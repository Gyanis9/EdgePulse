#include "Base/Database/Database.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Alarm/AlarmRepository.h"
#include "Core/Alarm/AlarmService.h"
#include "Core/Device/NodeLifecycleService.h"
#include "Core/Device/NodeRepository.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace
{
    void setLastSeen(const EdgePulse::TestSupport::AuthTestContext &context, const std::int64_t nodeId, const std::string &value)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(context.databasePath().string());
        ASSERT_NE(nullptr, connection->execute("UPDATE biz_node SET last_seen = '" + value + "' WHERE id = " + std::to_string(nodeId)));
    }
} // namespace

// 钉住：扫描只把在线且超时的节点转离线一次，并只建一条系统告警
TEST(NodeLifecycle, SweepTransitionsOnceAndRaisesOneSystemAlarm)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    nodes         = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto                                    alarms        = std::make_shared<EdgePulse::Alarm::AlarmRepository>(context.database);
    int                                     notifications = 0;
    EdgePulse::Alarm::AlarmService          alarmService(alarms,
                                                         [&notifications](const EdgePulse::Alarm::AlarmRecord &) -> AsynGyanis::Core::Task<>
                                                         {
                                                    ++notifications;
                                                    co_return;
                                                         });
    int                                     published = 0;
    EdgePulse::Device::NodeLifecycleService lifecycle(
            *nodes, alarmService,
            [&published](const EdgePulse::Device::NodeRecord &) -> AsynGyanis::Core::Task<>
            {
                ++published;
                co_return;
            },
            90);

    const auto created = runner.run(nodes->create(runner.loop(), "lifecycle-node", "{}"));
    const auto beat    = runner.run(nodes->heartbeat(runner.loop(), created.id, "1.0.0"));
    ASSERT_TRUE(beat.has_value());
    EXPECT_TRUE(beat->becameOnline);

    const std::string future = "9999-12-31 23:59:59";
    runner.run(lifecycle.sweep(runner.loop(), future));
    runner.run(lifecycle.sweep(runner.loop(), future));

    const auto offline = runner.run(nodes->findById(runner.loop(), created.id));
    ASSERT_TRUE(offline.has_value());
    EXPECT_EQ("offline", offline->status);
    EXPECT_EQ(1, published);
    EXPECT_EQ(1, notifications);
    EXPECT_EQ(1, runner.run(alarms->openCount(runner.loop())));
    const auto offlineCounts = runner.run(nodes->countByStatus(runner.loop()));
    EXPECT_EQ(0, offlineCounts.online);
    EXPECT_EQ(1, offlineCounts.offline);

    // 心跳恢复：在线转换、告警销案、再次发布
    const auto recovered = runner.run(nodes->heartbeat(runner.loop(), created.id, "1.0.0"));
    ASSERT_TRUE(recovered.has_value());
    EXPECT_TRUE(recovered->becameOnline);
    runner.run(lifecycle.onHeartbeat(runner.loop(), recovered->node, recovered->becameOnline));
    EXPECT_EQ(2, published);
    EXPECT_EQ(2, notifications);
    EXPECT_EQ(0, runner.run(alarms->openCount(runner.loop())));
    const auto onlineCounts = runner.run(nodes->countByStatus(runner.loop()));
    EXPECT_EQ(1, onlineCounts.online);
    EXPECT_EQ(0, onlineCounts.offline);

    // 已是在线状态时第二次心跳不再触发在线转换
    const auto second = runner.run(nodes->heartbeat(runner.loop(), created.id, "1.0.0"));
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(second->becameOnline);
}

// 钉住：转离线与写告警之间中断的节点，下一轮扫描补一条系统告警，重复扫描不叠加
TEST(NodeLifecycle, SweepReconcilesOfflineNodeWithoutSystemAlarm)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    nodes         = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto                                    alarms        = std::make_shared<EdgePulse::Alarm::AlarmRepository>(context.database);
    int                                     notifications = 0;
    EdgePulse::Alarm::AlarmService          alarmService(alarms,
                                                         [&notifications](const EdgePulse::Alarm::AlarmRecord &) -> AsynGyanis::Core::Task<>
                                                         {
                                                    ++notifications;
                                                    co_return;
                                                         });
    int                                     published = 0;
    EdgePulse::Device::NodeLifecycleService lifecycle(
            *nodes, alarmService,
            [&published](const EdgePulse::Device::NodeRecord &) -> AsynGyanis::Core::Task<>
            {
                ++published;
                co_return;
            },
            90);

    const auto created = runner.run(nodes->create(runner.loop(), "reconcile-node", "{}"));
    const auto beat    = runner.run(nodes->heartbeat(runner.loop(), created.id, "1.0.0"));
    ASSERT_TRUE(beat.has_value());
    ASSERT_TRUE(beat->becameOnline);

    // 模拟上一轮只完成了转离线：直接调用仓储，不经过生命周期服务
    const std::string future       = "9999-12-31 23:59:59";
    const auto        transitioned = runner.run(nodes->markStaleOffline(runner.loop(), future, 200));
    ASSERT_EQ(1U, transitioned.size());
    EXPECT_EQ("offline", transitioned.front().status);
    EXPECT_EQ(0, runner.run(alarms->openCount(runner.loop())));

    runner.run(lifecycle.sweep(runner.loop(), future));
    EXPECT_EQ(1, runner.run(alarms->openCount(runner.loop())));
    EXPECT_EQ(1, notifications);
    // 补告警路径与正常离线路径一致，也补发一次离线状态
    EXPECT_EQ(1, published);

    runner.run(lifecycle.sweep(runner.loop(), future));
    EXPECT_EQ(1, runner.run(alarms->openCount(runner.loop())));
    EXPECT_EQ(1, notifications);
}

// 钉住：批次处理到下一个节点前它已恢复在线，旧快照不能把它覆盖成离线
TEST(NodeLifecycle, SweepSkipsNodeThatRecoveredDuringBatch)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    nodes  = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto                                    alarms = std::make_shared<EdgePulse::Alarm::AlarmRepository>(context.database);
    EdgePulse::Alarm::AlarmService          alarmService(alarms);

    const auto first  = runner.run(nodes->create(runner.loop(), "batch-first", "{}"));
    const auto second = runner.run(nodes->create(runner.loop(), "batch-second", "{}"));
    ASSERT_TRUE(runner.run(nodes->heartbeat(runner.loop(), first.id, "1.0.0")).has_value());
    ASSERT_TRUE(runner.run(nodes->heartbeat(runner.loop(), second.id, "1.0.0")).has_value());
    // 固定扫描顺序：first 先于 second
    setLastSeen(context, first.id, "2000-01-01 00:00:00");
    setLastSeen(context, second.id, "2000-01-02 00:00:00");

    int                                     published = 0;
    EdgePulse::Device::NodeLifecycleService lifecycle(
            *nodes, alarmService,
            [&published, &nodes, &runner, second](const EdgePulse::Device::NodeRecord &node) -> AsynGyanis::Core::Task<>
            {
                ++published;
                if (node.id == second.id)
                {
                    co_return;
                }
                // 处理 first 时模拟 second 的心跳插进来，把 second 拉回在线
                static_cast<void>(co_await nodes->heartbeat(runner.loop(), second.id, "1.0.0"));
                co_return;
            },
            90);

    runner.run(lifecycle.sweep(runner.loop(), "9999-12-31 23:59:59"));

    EXPECT_EQ(1, published);
    const auto recovered = runner.run(nodes->findById(runner.loop(), second.id));
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ("online", recovered->status);
    EXPECT_TRUE(runner.run(alarms->findOpen(runner.loop(), 0, first.id)).has_value());
    EXPECT_FALSE(runner.run(alarms->findOpen(runner.loop(), 0, second.id)).has_value());
    EXPECT_EQ(1, runner.run(alarms->openCount(runner.loop())));
}

// 钉住：心跳没有在线转换时也要销案，清掉扫描在心跳之后插入的过期离线告警
TEST(NodeLifecycle, HeartbeatWithoutOnlineTransitionResolvesOfflineAlarm)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    nodes  = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto                                    alarms = std::make_shared<EdgePulse::Alarm::AlarmRepository>(context.database);
    EdgePulse::Alarm::AlarmService          alarmService(alarms);
    int                                     published = 0;
    EdgePulse::Device::NodeLifecycleService lifecycle(
            *nodes, alarmService,
            [&published](const EdgePulse::Device::NodeRecord &) -> AsynGyanis::Core::Task<>
            {
                ++published;
                co_return;
            },
            90);

    const auto created = runner.run(nodes->create(runner.loop(), "resolve-node", "{}"));
    const auto beat    = runner.run(nodes->heartbeat(runner.loop(), created.id, "1.0.0"));
    ASSERT_TRUE(beat.has_value());
    ASSERT_TRUE(beat->becameOnline);
    ASSERT_TRUE(runner.run(alarmService.reportNodeOffline(runner.loop(), beat->node, 90)).has_value());
    ASSERT_EQ(1, runner.run(alarms->openCount(runner.loop())));

    const auto second = runner.run(nodes->heartbeat(runner.loop(), created.id, "1.0.0"));
    ASSERT_TRUE(second.has_value());
    ASSERT_FALSE(second->becameOnline);
    runner.run(lifecycle.onHeartbeat(runner.loop(), second->node, second->becameOnline));

    EXPECT_EQ(0, runner.run(alarms->openCount(runner.loop())));
    EXPECT_EQ(0, published);
}

// 钉住：已吊销证书的节点不会再心跳，补告警扫描不能为它制造无法销案的 critical 告警
TEST(NodeLifecycle, RevokedNodeGetsNoOfflineAlarm)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    nodes  = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto                                    alarms = std::make_shared<EdgePulse::Alarm::AlarmRepository>(context.database);
    EdgePulse::Alarm::AlarmService          alarmService(alarms);
    int                                     published = 0;
    EdgePulse::Device::NodeLifecycleService lifecycle(
            *nodes, alarmService,
            [&published](const EdgePulse::Device::NodeRecord &) -> AsynGyanis::Core::Task<>
            {
                ++published;
                co_return;
            },
            90);

    const auto created = runner.run(nodes->create(runner.loop(), "revoked-node", "{}"));
    const auto beat    = runner.run(nodes->heartbeat(runner.loop(), created.id, "1.0.0"));
    ASSERT_TRUE(beat.has_value());
    ASSERT_TRUE(beat->becameOnline);

    const auto revoked = runner.run(nodes->revokeCertificate(runner.loop(), created.id));
    ASSERT_TRUE(revoked.has_value());
    ASSERT_FALSE(revoked->certRevokedAt.empty());

    runner.run(lifecycle.sweep(runner.loop(), "9999-12-31 23:59:59"));

    EXPECT_EQ(0, runner.run(alarms->openCount(runner.loop())));
    EXPECT_EQ(0, published);
}

// 钉住：补告警批次处理期间节点被吊销时，复查要跳过它，不能为永远不会再心跳的节点建告警
TEST(NodeLifecycle, SweepReconciliationSkipsNodeRevokedDuringBatch)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    nodes  = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto                                    alarms = std::make_shared<EdgePulse::Alarm::AlarmRepository>(context.database);
    EdgePulse::Alarm::AlarmService          alarmService(alarms);

    const auto first  = runner.run(nodes->create(runner.loop(), "reconcile-first", "{}"));
    const auto second = runner.run(nodes->create(runner.loop(), "reconcile-second", "{}"));
    // 固定补告警顺序：first 先于 second
    setLastSeen(context, first.id, "2000-01-01 00:00:00");
    setLastSeen(context, second.id, "2000-01-02 00:00:00");

    int                                     published = 0;
    EdgePulse::Device::NodeLifecycleService lifecycle(
            *nodes, alarmService,
            [&published, &nodes, &runner, second](const EdgePulse::Device::NodeRecord &node) -> AsynGyanis::Core::Task<>
            {
                ++published;
                if (node.id == second.id)
                {
                    co_return;
                }
                // 补 first 的告警时模拟 second 被吊销：second 的复查必须重新看到吊销时间
                static_cast<void>(co_await nodes->revokeCertificate(runner.loop(), second.id));
                co_return;
            },
            90);

    runner.run(lifecycle.sweep(runner.loop(), "9999-12-31 23:59:59"));

    // 只有 first 补发状态与告警；second 已吊销，不能留下无法销案的 critical 告警
    EXPECT_EQ(1, published);
    EXPECT_TRUE(runner.run(alarms->findOpen(runner.loop(), 0, first.id)).has_value());
    EXPECT_FALSE(runner.run(alarms->findOpen(runner.loop(), 0, second.id)).has_value());
    EXPECT_EQ(1, runner.run(alarms->openCount(runner.loop())));
    const auto revoked = runner.run(nodes->findById(runner.loop(), second.id));
    ASSERT_TRUE(revoked.has_value());
    EXPECT_FALSE(revoked->certRevokedAt.empty());
}

// 钉住：全租户扫描能转换别家租户的节点，告警落在节点自己的租户下
TEST(NodeLifecycle, SweepTransitionsNodeFromOtherTenant)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    nodes    = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto                                    allNodes = nodes->scoped({0, true});
    auto                                    alarms   = std::make_shared<EdgePulse::Alarm::AlarmRepository>(context.database);
    EdgePulse::Alarm::AlarmService          alarmService(alarms);
    int                                     published = 0;
    EdgePulse::Device::NodeLifecycleService lifecycle(
            *allNodes, alarmService,
            [&published](const EdgePulse::Device::NodeRecord &) -> AsynGyanis::Core::Task<>
            {
                ++published;
                co_return;
            },
            90);

    const auto tenantNodes = nodes->scoped({2, false});
    const auto created     = runner.run(tenantNodes->create(runner.loop(), "tenant-two-node", "{}"));
    ASSERT_EQ(2, created.tenantId);
    const auto beat = runner.run(tenantNodes->heartbeat(runner.loop(), created.id, "1.0.0"));
    ASSERT_TRUE(beat.has_value());
    ASSERT_TRUE(beat->becameOnline);

    runner.run(lifecycle.sweep(runner.loop(), "9999-12-31 23:59:59"));

    const auto offline = runner.run(allNodes->findById(runner.loop(), created.id));
    ASSERT_TRUE(offline.has_value());
    EXPECT_EQ("offline", offline->status);
    EXPECT_EQ(1, published);
    EXPECT_EQ(1, runner.run(alarms->scoped({2, false})->openCount(runner.loop())));
    EXPECT_EQ(0, runner.run(alarms->openCount(runner.loop())));
    const auto raised = runner.run(alarms->scoped({2, false})->findOpen(runner.loop(), 0, created.id));
    ASSERT_TRUE(raised.has_value());
    EXPECT_EQ(2, raised->tenantId);
}
