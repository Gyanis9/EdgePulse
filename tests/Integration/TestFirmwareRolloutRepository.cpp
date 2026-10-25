#include "Base/Exception/EdgePulseException.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Firmware/FirmwareRepository.h"
#include "Core/Firmware/FirmwareRolloutRepository.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
    /**
     * @brief 写入一份可被灰度引用的固件元数据
     * @return 固件标识
     */
    std::int64_t createFirmware(EdgePulse::TestSupport::EventLoopRunner &runner, EdgePulse::Firmware::FirmwareRepository &firmware, const std::string &version)
    {
        EdgePulse::Firmware::FirmwareRecord record;
        record.name        = "EdgePulse OS " + version;
        record.version     = version;
        record.fileName    = "os-" + version + ".bin";
        record.sizeBytes   = 4096;
        record.sha256      = std::string(64, 'a');
        record.storagePath = "unused-" + version;
        return runner.run(firmware.create(runner.loop(), record));
    }

    /**
     * @brief 构造一份三波次、四目标的灰度聚合
     * @param firmwareId 固件标识
     * @return 可直接交给仓储创建的草稿
     */
    EdgePulse::Firmware::FirmwareRolloutDraft makeDraft(const std::int64_t firmwareId)
    {
        EdgePulse::Firmware::FirmwareRolloutDraft draft;
        draft.rollout.firmwareId = firmwareId;
        draft.rollout.name       = "灰度发布 A";
        draft.rollout.policyJson = R"({"waves":[{"percent":5},{"percent":25},{"percent":100}]})";
        draft.rollout.status     = "scheduled";
        draft.rollout.createdBy  = 7;

        const std::vector<std::vector<std::int64_t>> waveNodes{{101, 102}, {103}, {104}};
        for (std::size_t index = 0; index < waveNodes.size(); ++index)
        {
            EdgePulse::Firmware::FirmwareRolloutWaveDraft wave;
            wave.wave.waveIndex = static_cast<int>(index);
            wave.wave.status    = index == 0 ? "observing" : "pending";
            for (const std::int64_t nodeId: waveNodes[index])
            {
                EdgePulse::Firmware::FirmwareRolloutTargetRecord target;
                target.nodeId             = nodeId;
                target.previousFirmwareId = 0;
                target.status             = "pending";
                wave.targets.push_back(target);
            }
            draft.waves.push_back(std::move(wave));
        }
        return draft;
    }

    /**
     * @brief 在聚合里按节点查目标
     */
    const EdgePulse::Firmware::FirmwareRolloutTargetRecord *findTarget(const EdgePulse::Firmware::FirmwareRolloutAggregate &aggregate, const std::int64_t nodeId)
    {
        const auto target = std::find_if(aggregate.targets.begin(), aggregate.targets.end(),
                                         [nodeId](const EdgePulse::Firmware::FirmwareRolloutTargetRecord &row) { return row.nodeId == nodeId; });
        return target == aggregate.targets.end() ? nullptr : &*target;
    }
} // namespace

// 钉住：一次创建同时写入 rollout、三波次与四目标，读取聚合时按波次与目标还原
TEST(FirmwareRolloutRepository, CreatesAndReadsAggregate)
{
    EdgePulse::TestSupport::AuthTestContext        context;
    EdgePulse::TestSupport::EventLoopRunner        runner;
    EdgePulse::Firmware::FirmwareRepository        firmware(context.database);
    EdgePulse::Firmware::FirmwareRolloutRepository rollouts(context.database);
    const std::int64_t                             firmwareId = createFirmware(runner, firmware, "3.0.0");

    const std::int64_t rolloutId = runner.run(rollouts.create(runner.loop(), makeDraft(firmwareId)));
    ASSERT_GT(rolloutId, 0);

    const std::optional<EdgePulse::Firmware::FirmwareRolloutAggregate> aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_EQ(rolloutId, aggregate->rollout.id);
    EXPECT_EQ(1, aggregate->rollout.tenantId);
    EXPECT_EQ(firmwareId, aggregate->rollout.firmwareId);
    EXPECT_EQ("灰度发布 A", aggregate->rollout.name);
    EXPECT_EQ("scheduled", aggregate->rollout.status);
    EXPECT_EQ(0, aggregate->rollout.currentWave);
    EXPECT_EQ(3, aggregate->rollout.totalWaves);
    EXPECT_EQ(7, aggregate->rollout.createdBy);
    EXPECT_EQ(1, aggregate->rollout.version);
    EXPECT_FALSE(aggregate->rollout.createdAt.empty());

    ASSERT_EQ(3U, aggregate->waves.size());
    for (std::size_t index = 0; index < aggregate->waves.size(); ++index)
    {
        EXPECT_EQ(static_cast<int>(index), aggregate->waves[index].waveIndex);
        EXPECT_EQ(rolloutId, aggregate->waves[index].rolloutId);
        EXPECT_GT(aggregate->waves[index].id, 0);
    }
    EXPECT_EQ(2, aggregate->waves[0].targetCount);
    EXPECT_EQ(1, aggregate->waves[1].targetCount);
    EXPECT_EQ(1, aggregate->waves[2].targetCount);

    ASSERT_EQ(4U, aggregate->targets.size());
    const EdgePulse::Firmware::FirmwareRolloutTargetRecord *second = findTarget(*aggregate, 102);
    ASSERT_NE(nullptr, second);
    EXPECT_EQ(1, second->tenantId);
    EXPECT_EQ(aggregate->waves[0].id, second->waveId);
    EXPECT_EQ("pending", second->status);
    EXPECT_EQ(rolloutId, second->rolloutId);

    const auto page = runner.run(rollouts.list(runner.loop(), firmwareId, EdgePulse::Common::PageParameters{1, 10}));
    EXPECT_EQ(1, page.total);
    ASSERT_EQ(1U, page.rows.size());
    EXPECT_EQ(rolloutId, page.rows[0].id);

    // 租户边界：另一个租户既查不到聚合，也列不出发布
    const auto otherTenant = rollouts.scoped(EdgePulse::System::TenantScope{2, false});
    EXPECT_FALSE(runner.run(otherTenant->find(runner.loop(), rolloutId)).has_value());
    EXPECT_EQ(0, runner.run(otherTenant->list(runner.loop(), firmwareId, EdgePulse::Common::PageParameters{1, 10})).total);
}

// 钉住：状态推进按版本比较交换，旧版本与跨租户写入都落空，暂停与恢复同步维护时间戳
TEST(FirmwareRolloutRepository, StatusCompareAndSwap)
{
    EdgePulse::TestSupport::AuthTestContext        context;
    EdgePulse::TestSupport::EventLoopRunner        runner;
    EdgePulse::Firmware::FirmwareRepository        firmware(context.database);
    EdgePulse::Firmware::FirmwareRolloutRepository rollouts(context.database);
    const std::int64_t                             firmwareId = createFirmware(runner, firmware, "3.0.1");
    const std::int64_t                             rolloutId  = runner.run(rollouts.create(runner.loop(), makeDraft(firmwareId)));

    ASSERT_TRUE(runner.run(rollouts.updateStatus(runner.loop(), rolloutId, 1, "running", 1)));
    auto aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_EQ("running", aggregate->rollout.status);
    EXPECT_EQ(1, aggregate->rollout.currentWave);
    EXPECT_EQ(2, aggregate->rollout.version);
    EXPECT_FALSE(aggregate->rollout.startedAt.empty());

    // 旧版本再提交一次必须落空，不能把状态写回
    EXPECT_FALSE(runner.run(rollouts.updateStatus(runner.loop(), rolloutId, 1, "paused", 0, "人工暂停")));
    EXPECT_TRUE(runner.run(rollouts.updateStatus(runner.loop(), rolloutId, 2, "paused", 1, "人工暂停")));
    aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_EQ("paused", aggregate->rollout.status);
    EXPECT_EQ("人工暂停", aggregate->rollout.pauseReason);
    EXPECT_FALSE(aggregate->rollout.pausedAt.empty());
    EXPECT_EQ(3, aggregate->rollout.version);

    // 恢复运行清掉暂停现场，开始时间保持首次进入运行时的值
    EXPECT_TRUE(runner.run(rollouts.updateStatus(runner.loop(), rolloutId, 3, "running", 1)));
    aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_TRUE(aggregate->rollout.pausedAt.empty());
    EXPECT_TRUE(aggregate->rollout.pauseReason.empty());

    EXPECT_TRUE(runner.run(rollouts.updateStatus(runner.loop(), rolloutId, 4, "completed", 3)));
    aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_EQ("completed", aggregate->rollout.status);
    EXPECT_EQ(3, aggregate->rollout.currentWave);
    EXPECT_FALSE(aggregate->rollout.finishedAt.empty());

    // 跨租户写入按不存在处理
    const auto otherTenant = rollouts.scoped(EdgePulse::System::TenantScope{2, false});
    EXPECT_FALSE(runner.run(otherTenant->updateStatus(runner.loop(), rolloutId, 5, "aborted")));
}

// 钉住：同一节点在一个发布里只能出现一次；唯一冲突让整笔事务回滚，不留下半份聚合
TEST(FirmwareRolloutRepository, RejectsDuplicateNodeAndRollsBack)
{
    EdgePulse::TestSupport::AuthTestContext        context;
    EdgePulse::TestSupport::EventLoopRunner        runner;
    EdgePulse::Firmware::FirmwareRepository        firmware(context.database);
    EdgePulse::Firmware::FirmwareRolloutRepository rollouts(context.database);
    const std::int64_t                             firmwareId = createFirmware(runner, firmware, "3.0.2");

    EdgePulse::Firmware::FirmwareRolloutDraft draft = makeDraft(firmwareId);
    draft.waves[1].targets[0].nodeId                = draft.waves[0].targets[0].nodeId;
    try
    {
        static_cast<void>(runner.run(rollouts.create(runner.loop(), draft)));
        FAIL() << "同一节点重复出现时创建必须失败";
    } catch (const EdgePulse::Common::DatabaseException &failure)
    {
        EXPECT_NE(std::string::npos, std::string{failure.what()}.find("重复"));
    }

    // 创建先写聚合再写波次与目标，唯一冲突后不能留下 rollout 头
    EXPECT_EQ(0, runner.run(rollouts.list(runner.loop(), firmwareId, EdgePulse::Common::PageParameters{1, 10})).total);
}

// 钉住：带 commandId 只更新指定任务，空 commandId 保持旧的按固件与节点更新
TEST(FirmwareRolloutRepository, TaskUpdatesScopeByCommandId)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::Firmware::FirmwareRepository firmware(context.database);
    EdgePulse::Device::NodeRepository       nodes(context.database);
    const std::int64_t                      firmwareId = createFirmware(runner, firmware, "3.0.3");
    const std::int64_t                      nodeId     = runner.run(nodes.create(runner.loop(), "rollout-task-node", "{}")).id;

    const std::int64_t firstTask = runner.run(firmware.createTask(runner.loop(), firmwareId, nodeId, 0, 0, 0, 11));
    ASSERT_GT(firstTask, 0);
    const auto first = runner.run(firmware.updateTask(runner.loop(), firmwareId, nodeId, "failed", 40, "第一条失败", 11));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(11, first->commandId);
    EXPECT_EQ("failed", first->status);

    const std::int64_t secondTask = runner.run(firmware.createTask(runner.loop(), firmwareId, nodeId, 0, 0, 0, 22));
    ASSERT_GT(secondTask, 0);

    // 不存在的命令号不能误伤其他任务
    EXPECT_FALSE(runner.run(firmware.updateTask(runner.loop(), firmwareId, nodeId, "installed", 100, "错配", 33)).has_value());
    const auto second = runner.run(firmware.updateTask(runner.loop(), firmwareId, nodeId, "installed", 100, "第二条完成", 22));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(22, second->commandId);

    const std::vector<EdgePulse::Firmware::FirmwareTaskRecord> scopedRows = runner.run(firmware.tasks(runner.loop(), firmwareId));
    ASSERT_EQ(2U, scopedRows.size());
    const auto firstRow = std::find_if(scopedRows.begin(), scopedRows.end(), [](const auto &row) { return row.commandId == 11; });
    ASSERT_NE(scopedRows.end(), firstRow);
    EXPECT_EQ("failed", firstRow->status);
    const auto secondRow = std::find_if(scopedRows.begin(), scopedRows.end(), [](const auto &row) { return row.commandId == 22; });
    ASSERT_NE(scopedRows.end(), secondRow);
    EXPECT_EQ("installed", secondRow->status);
    EXPECT_EQ("第二条完成", secondRow->message);

    // 旧调用不带 commandId：仍按固件与节点更新全部任务，并返回最新一行
    const auto legacy = runner.run(firmware.updateTask(runner.loop(), firmwareId, nodeId, "installed", 100, "旧调用"));
    ASSERT_TRUE(legacy.has_value());
    EXPECT_EQ(22, legacy->commandId);
    for (const EdgePulse::Firmware::FirmwareTaskRecord &row: runner.run(firmware.tasks(runner.loop(), firmwareId)))
    {
        EXPECT_EQ("installed", row.status);
        EXPECT_EQ("旧调用", row.message);
    }
}

// 钉住：心跳写入 Agent 上报的已安装固件版本；省略该参数时保留原值
TEST(FirmwareRolloutRepository, HeartbeatPersistsInstalledFirmwareVersion)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::Device::NodeRepository       nodes(context.database);
    const std::int64_t                      nodeId = runner.run(nodes.create(runner.loop(), "rollout-heartbeat-node", "{}")).id;

    const auto beat = runner.run(nodes.heartbeat(runner.loop(), nodeId, "1.2.0", "2.0.0"));
    ASSERT_TRUE(beat.has_value());
    EXPECT_EQ("2.0.0", beat->node.installedFirmwareVersion);
    EXPECT_EQ("2.0.0", runner.run(nodes.findById(runner.loop(), nodeId))->installedFirmwareVersion);

    // 旧 Agent 不带版本：只刷新 Agent 版本，已安装固件版本保持不变
    const auto legacy = runner.run(nodes.heartbeat(runner.loop(), nodeId, "1.2.1"));
    ASSERT_TRUE(legacy.has_value());
    EXPECT_EQ("1.2.1", legacy->node.agentVersion);
    EXPECT_EQ("2.0.0", legacy->node.installedFirmwareVersion);

    const std::int64_t freshNodeId = runner.run(nodes.create(runner.loop(), "rollout-fresh-node", "{}")).id;
    EXPECT_TRUE(runner.run(nodes.findById(runner.loop(), freshNodeId))->installedFirmwareVersion.empty());
}

// 钉住：波次时间戳、目标健康快照与门控快照可分别更新，待推进列表只收运行态发布
TEST(FirmwareRolloutRepository, UpdatesWaveTargetGateAndListsDue)
{
    EdgePulse::TestSupport::AuthTestContext        context;
    EdgePulse::TestSupport::EventLoopRunner        runner;
    EdgePulse::Firmware::FirmwareRepository        firmware(context.database);
    EdgePulse::Firmware::FirmwareRolloutRepository rollouts(context.database);
    const std::int64_t                             firmwareId = createFirmware(runner, firmware, "3.0.4");

    EdgePulse::Firmware::FirmwareRolloutDraft draft     = makeDraft(firmwareId);
    const std::int64_t                        rolloutId = runner.run(rollouts.create(runner.loop(), draft));
    auto                                      aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    const std::int64_t firstWaveId = aggregate->waves[0].id;

    const std::vector<EdgePulse::Firmware::FirmwareRolloutRecord> due = runner.run(rollouts.listDueRollouts(runner.loop(), 10));
    ASSERT_EQ(1U, due.size());
    EXPECT_EQ(rolloutId, due[0].id);

    EXPECT_TRUE(runner.run(rollouts.setWave(runner.loop(), firstWaveId, "observing", "2026-10-25 10:00:00", "2026-10-25 10:05:00")));
    aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_EQ("observing", aggregate->waves[0].status);
    EXPECT_EQ("2026-10-25 10:00:00", aggregate->waves[0].dispatchedAt);
    EXPECT_EQ("2026-10-25 10:05:00", aggregate->waves[0].deadlineAt);

    EXPECT_TRUE(runner.run(rollouts.appendGate(runner.loop(), firstWaveId, R"({"passed":1,"failed":0})")));
    EXPECT_TRUE(runner.run(rollouts.appendGate(runner.loop(), firstWaveId, R"({"passed":2,"failed":0})")));
    aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_NE(std::string::npos, aggregate->waves[0].gateJson.find(R"({"passed":1,"failed":0})"));
    EXPECT_NE(std::string::npos, aggregate->waves[0].gateJson.find(R"({"passed":2,"failed":0})"));
    EXPECT_NE(std::string::npos, aggregate->waves[0].gateJson.find('\n'));

    const auto *firstTarget = findTarget(*aggregate, 101);
    ASSERT_NE(nullptr, firstTarget);
    EdgePulse::Firmware::FirmwareRolloutTargetUpdate targetUpdate;
    targetUpdate.status     = "healthy";
    targetUpdate.healthJson = R"({"command":"succeeded"})";
    targetUpdate.taskId     = 501;
    targetUpdate.commandId  = 601;
    EXPECT_TRUE(runner.run(rollouts.updateTarget(runner.loop(), firstTarget->id, targetUpdate)));
    aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    firstTarget = findTarget(*aggregate, 101);
    ASSERT_NE(nullptr, firstTarget);
    EXPECT_EQ("healthy", firstTarget->status);
    EXPECT_EQ(R"({"command":"succeeded"})", firstTarget->healthJson);
    EXPECT_EQ(501, firstTarget->taskId);
    EXPECT_EQ(601, firstTarget->commandId);

    const auto otherTenant = rollouts.scoped(EdgePulse::System::TenantScope{2, false});
    EXPECT_FALSE(runner.run(otherTenant->setWave(runner.loop(), firstWaveId, "failed")));
    EXPECT_FALSE(runner.run(otherTenant->appendGate(runner.loop(), firstWaveId, R"({"passed":0})")));

    // 草稿不进入待推进集合
    EdgePulse::Firmware::FirmwareRolloutDraft draftOnly = makeDraft(firmwareId);
    draftOnly.rollout.status                            = "draft";
    ASSERT_GT(runner.run(rollouts.create(runner.loop(), draftOnly)), 0);
    EXPECT_EQ(1U, runner.run(rollouts.listDueRollouts(runner.loop(), 10)).size());
}
