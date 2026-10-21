#include "Base/Runtime/TimeFormat.h"
#include "Core/Alarm/AlarmRepository.h"
#include "Core/Alarm/AlarmService.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/NodeGroupRepository.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Firmware/FirmwareRepository.h"
#include "Core/Telemetry/TelemetryRepository.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace
{
    EdgePulse::Telemetry::TelemetrySample sample(const std::string &metric, const double value)
    {
        EdgePulse::Telemetry::TelemetrySample result;
        result.metric = metric;
        result.value  = value;
        result.unit   = "%";
        return result;
    }
} // namespace

// 钉住：遥测批量写入、最新值与时间序列
TEST(FleetServices, TelemetryInsertLatestAndSeries)
{
    EdgePulse::TestSupport::AuthTestContext   context;
    EdgePulse::TestSupport::EventLoopRunner   runner;
    EdgePulse::Telemetry::TelemetryRepository repository(context.database);
    EdgePulse::Device::NodeRepository         nodes(context.database);
    const std::int64_t                        nodeId = runner.run(nodes.create(runner.loop(), "telemetry-node", "{}")).id;

    const auto now = EdgePulse::Common::nowDateTimeUtc();
    ASSERT_TRUE(runner.run(repository.insertBatch(runner.loop(), nodeId, {sample("cpu.usage", 20), sample("memory.usage", 40)})));
    ASSERT_TRUE(runner.run(repository.insertBatch(runner.loop(), nodeId, {sample("cpu.usage", 35)})));

    const auto latest = runner.run(repository.latest(runner.loop(), nodeId));
    ASSERT_EQ(2U, latest.size());
    const auto cpu = std::find_if(latest.begin(), latest.end(), [](const auto &point) { return point.metric == "cpu.usage"; });
    ASSERT_NE(latest.end(), cpu);
    EXPECT_DOUBLE_EQ(35.0, cpu->value);

    const auto series = runner.run(repository.series(runner.loop(), nodeId, "cpu.usage", now, EdgePulse::Common::nowDateTimeUtc(), 100));
    EXPECT_EQ(2U, series.size());
}

// 钉住：保留天数只删窗口外的遥测，非正数天数不删任何数据
TEST(FleetServices, TelemetryPruneRespectsRetentionWindow)
{
    EdgePulse::TestSupport::AuthTestContext   context;
    EdgePulse::TestSupport::EventLoopRunner   runner;
    EdgePulse::Telemetry::TelemetryRepository repository(context.database);
    EdgePulse::Device::NodeRepository         nodes(context.database);
    const std::int64_t                        nodeId = runner.run(nodes.create(runner.loop(), "retention-node", "{}")).id;

    auto stale        = sample("cpu.usage", 10);
    stale.collectedAt = "2020-01-01 00:00:00";
    ASSERT_TRUE(runner.run(repository.insertBatch(runner.loop(), nodeId, {stale, sample("cpu.usage", 20)})));

    const auto allTenants = repository.scoped({0, true});
    EXPECT_FALSE(runner.run(allTenants->prune(runner.loop(), 0)));
    const auto window = runner.run(repository.series(runner.loop(), nodeId, "cpu.usage", "2019-01-01 00:00:00", EdgePulse::Common::nowDateTimeUtc(), 100));
    EXPECT_EQ(2U, window.size());

    ASSERT_TRUE(runner.run(allTenants->prune(runner.loop(), 30)));
    const auto remaining = runner.run(repository.series(runner.loop(), nodeId, "cpu.usage", "2019-01-01 00:00:00", EdgePulse::Common::nowDateTimeUtc(), 100));
    ASSERT_EQ(1U, remaining.size());
    EXPECT_DOUBLE_EQ(20.0, remaining.front().value);
}

// 钉住：阈值触发、自动恢复与冷却窗口
TEST(FleetServices, AlarmEvaluationTriggersAndResolves)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    alarmRepository = std::make_shared<EdgePulse::Alarm::AlarmRepository>(context.database);
    int                                     notifications   = 0;
    EdgePulse::Alarm::AlarmService          service(alarmRepository,
                                                    [&notifications](const EdgePulse::Alarm::AlarmRecord &) -> AsynGyanis::Core::Task<>
                                                    {
                                               ++notifications;
                                               co_return;
                                                    });

    const auto triggered = runner.run(service.evaluate(runner.loop(), 1, {sample("cpu.usage", 95)}));
    ASSERT_EQ(1U, triggered.size());
    EXPECT_EQ("open", triggered[0].status);
    EXPECT_EQ(1, notifications);
    EXPECT_EQ(1, runner.run(alarmRepository->openCount(runner.loop())));

    // 冷却窗口内再次越过阈值不重复报
    const auto again = runner.run(service.evaluate(runner.loop(), 1, {sample("cpu.usage", 97)}));
    EXPECT_TRUE(again.empty());

    const auto resolved = runner.run(service.evaluate(runner.loop(), 1, {sample("cpu.usage", 10)}));
    ASSERT_EQ(1U, resolved.size());
    EXPECT_EQ("resolved", resolved[0].status);
    EXPECT_EQ(2, notifications);
    EXPECT_EQ(0, runner.run(alarmRepository->openCount(runner.loop())));
}

// 钉住：命令队列的轮询、结果与取消
TEST(FleetServices, CommandQueueLifecycle)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::Command::CommandRepository   repository(context.database);
    EdgePulse::Device::NodeRepository       nodes(context.database);
    const auto                              node   = runner.run(nodes.create(runner.loop(), "command-node", "{}"));
    const std::int64_t                      nodeId = node.id;

    const auto created = runner.run(repository.create(runner.loop(), nodeId, "reboot", "{}", 1, 60));
    ASSERT_TRUE(created.has_value());
    const auto next = runner.run(repository.takeNext(runner.loop(), nodeId));
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ("running", next->status);
    EXPECT_FALSE(runner.run(repository.takeNext(runner.loop(), nodeId)).has_value());

    const auto finished = runner.run(repository.finish(runner.loop(), created->id, "succeeded", R"({"ok":true})", "done"));
    ASSERT_TRUE(finished.has_value());
    EXPECT_EQ("succeeded", finished->status);

    const auto second = runner.run(repository.create(runner.loop(), nodeId, "diagnostics.collect", "{}", 1, 60));
    ASSERT_TRUE(second.has_value());
    const auto canceled = runner.run(repository.cancel(runner.loop(), second->id));
    ASSERT_TRUE(canceled.has_value());
    EXPECT_EQ("canceled", canceled->status);
}

// 钉住：固件元数据、任务状态与分组占用检查
TEST(FleetServices, FirmwareTasksAndNodeGroups)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::Firmware::FirmwareRepository firmware(context.database);
    EdgePulse::Device::NodeGroupRepository  groups(context.database);

    EdgePulse::Firmware::FirmwareRecord record;
    record.name                   = "EdgePulse OS";
    record.version                = "1.0.0";
    record.fileName               = "os.bin";
    record.sizeBytes              = 1024;
    record.sha256                 = std::string(64, 'a');
    record.storagePath            = "var/firmware/test.bin";
    const std::int64_t firmwareId = runner.run(firmware.create(runner.loop(), record));
    EXPECT_GT(firmwareId, 0);

    const std::int64_t taskId = runner.run(firmware.createTask(runner.loop(), firmwareId, 1));
    EXPECT_GT(taskId, 0);
    const auto task = runner.run(firmware.updateTask(runner.loop(), firmwareId, 1, "installed", 100, "完成"));
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ("installed", task->status);

    const std::int64_t parent = runner.run(groups.create(runner.loop(), 0, "华东", 10, "", 1));
    EXPECT_TRUE(runner.run(groups.exists(runner.loop(), parent)));
    const auto tree = runner.run(groups.tree(runner.loop()));
    EXPECT_GE(tree.size(), 2U);
}

// 钉住：计划命令到点前不参与轮询，到点后才进入待执行队列
TEST(FleetServices, ScheduledCommandsWaitForDueTime)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::Command::CommandRepository   repository(context.database);
    EdgePulse::Device::NodeRepository       nodes(context.database);
    const auto                              node   = runner.run(nodes.create(runner.loop(), "scheduled-node", "{}"));
    const std::int64_t                      nodeId = node.id;
    const auto                              now    = std::chrono::system_clock::now();
    const std::string                       future = EdgePulse::Common::formatDateTimeUtc(now + std::chrono::hours{1});
    const std::string                       past   = EdgePulse::Common::formatDateTimeUtc(now - std::chrono::minutes{1});

    const auto waiting = runner.run(repository.create(runner.loop(), nodeId, "reboot", "{}", 1, 60, future));
    ASSERT_TRUE(waiting.has_value());
    EXPECT_FALSE(runner.run(repository.takeNext(runner.loop(), nodeId)).has_value());
    EXPECT_TRUE(runner.run(repository.listDue(runner.loop())).empty());

    const auto due = runner.run(repository.create(runner.loop(), nodeId, "diagnostics.collect", "{}", 1, 60, past));
    ASSERT_TRUE(due.has_value());
    const auto dueList = runner.run(repository.listDue(runner.loop()));
    ASSERT_EQ(1U, dueList.size());
    EXPECT_EQ(due->id, dueList[0].id);
    const auto next = runner.run(repository.takeNext(runner.loop(), nodeId));
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(due->id, next->id);
}

// 钉住：任务记录下发前版本，回滚目标与节点上一版本都能取回
TEST(FleetServices, FirmwareRollbackTracksPreviousVersion)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::Firmware::FirmwareRepository firmware(context.database);
    EdgePulse::Device::NodeRepository       nodes(context.database);
    const auto                              node = runner.run(nodes.create(runner.loop(), "rollback-node", "{}"));

    EdgePulse::Firmware::FirmwareRecord first;
    first.name                 = "Firmware A";
    first.version              = "1.0.0";
    first.fileName             = "a.bin";
    first.contentType          = "application/octet-stream";
    first.sha256               = std::string(64, 'a');
    first.storagePath          = "unused-a";
    const std::int64_t firstId = runner.run(firmware.create(runner.loop(), first));

    EdgePulse::Firmware::FirmwareRecord second = first;
    second.name                                = "Firmware B";
    second.version                             = "2.0.0";
    second.sha256                              = std::string(64, 'b');
    second.storagePath                         = "unused-b";
    const std::int64_t secondId                = runner.run(firmware.create(runner.loop(), second));

    const std::int64_t firstTask = runner.run(firmware.createTask(runner.loop(), firstId, node.id));
    ASSERT_GT(firstTask, 0);
    ASSERT_TRUE(runner.run(firmware.updateTask(runner.loop(), firstId, node.id, "installed", 100, "ok")).has_value());

    const std::int64_t taskId = runner.run(firmware.createTask(runner.loop(), secondId, node.id, firstId));
    ASSERT_GT(taskId, 0);
    ASSERT_TRUE(runner.run(firmware.updateTask(runner.loop(), secondId, node.id, "installed", 100, "ok")).has_value());
    EXPECT_EQ(firstId, runner.run(firmware.latestOtherFirmware(runner.loop(), node.id, secondId)));
    EXPECT_EQ(firstId, runner.run(firmware.rollbackSource(runner.loop(), secondId, node.id)));

    // 回滚任务把当前版本记为“上一版”，之后还能再切回来
    const std::int64_t rollbackTask = runner.run(firmware.createTask(runner.loop(), firstId, node.id, secondId));
    ASSERT_GT(rollbackTask, 0);
    EXPECT_EQ(secondId, runner.run(firmware.rollbackSource(runner.loop(), firstId, node.id)));
}
