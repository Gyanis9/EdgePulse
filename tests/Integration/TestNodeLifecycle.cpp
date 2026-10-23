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
