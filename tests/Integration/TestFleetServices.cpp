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

    const auto now = EdgePulse::Common::nowDateTimeUtc();
    ASSERT_TRUE(runner.run(repository.insertBatch(runner.loop(), 1, {sample("cpu.usage", 20), sample("memory.usage", 40)})));
    ASSERT_TRUE(runner.run(repository.insertBatch(runner.loop(), 1, {sample("cpu.usage", 35)})));

    const auto latest = runner.run(repository.latest(runner.loop(), 1));
    ASSERT_EQ(2U, latest.size());
    const auto cpu = std::find_if(latest.begin(), latest.end(), [](const auto &point) { return point.metric == "cpu.usage"; });
    ASSERT_NE(latest.end(), cpu);
    EXPECT_DOUBLE_EQ(35.0, cpu->value);

    const auto series = runner.run(repository.series(runner.loop(), 1, "cpu.usage", now, EdgePulse::Common::nowDateTimeUtc(), 100));
    EXPECT_EQ(2U, series.size());
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
