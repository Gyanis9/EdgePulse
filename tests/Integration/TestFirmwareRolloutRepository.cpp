#include "Base/Exception/EdgePulseException.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Firmware/FirmwareRepository.h"
#include "Core/Firmware/FirmwareRolloutRepository.h"
#include "Plugins/DevicePlugin/NodeJson.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
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
     * @brief 在仓储所属租户下创建四个节点
     * @return 按创建顺序排列的节点标识
     */
    std::vector<std::int64_t> createNodes(EdgePulse::TestSupport::EventLoopRunner &runner, EdgePulse::Device::NodeRepository &nodes, const std::string &prefix)
    {
        std::vector<std::int64_t> ids;
        for (int index = 0; index < 4; ++index)
        {
            ids.push_back(runner.run(nodes.create(runner.loop(), prefix + "-" + std::to_string(index), "{}")).id);
        }
        return ids;
    }

    /**
     * @brief 构造一份三波次、四目标的灰度聚合
     * @param firmwareId 固件标识
     * @param nodeIds 四个真实节点标识，前两个进入第一波，其余各成一波
     * @return 可直接交给仓储创建的草稿
     */
    EdgePulse::Firmware::FirmwareRolloutDraft makeDraft(const std::int64_t firmwareId, const std::vector<std::int64_t> &nodeIds)
    {
        if (nodeIds.size() != 4)
        {
            throw std::invalid_argument("makeDraft 需要四个节点");
        }
        EdgePulse::Firmware::FirmwareRolloutDraft draft;
        draft.rollout.firmwareId = firmwareId;
        draft.rollout.name       = "灰度发布 A";
        draft.rollout.policyJson = R"({"waves":[{"percent":5},{"percent":25},{"percent":100}]})";
        draft.rollout.status     = "scheduled";
        draft.rollout.createdBy  = 7;

        const std::vector<std::vector<std::int64_t>> waveNodes{{nodeIds[0], nodeIds[1]}, {nodeIds[2]}, {nodeIds[3]}};
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

    /**
     * @brief 断言创建被拒且原因包含指定片段
     */
    void expectCreateRejected(EdgePulse::TestSupport::EventLoopRunner &runner, EdgePulse::Firmware::FirmwareRolloutRepository &rollouts,
                              EdgePulse::Firmware::FirmwareRolloutDraft draft, const std::string &fragment)
    {
        try
        {
            static_cast<void>(runner.run(rollouts.create(runner.loop(), std::move(draft))));
            FAIL() << "非法聚合必须拒绝创建";
        } catch (const EdgePulse::Common::DatabaseException &failure)
        {
            EXPECT_NE(std::string::npos, std::string{failure.what()}.find(fragment)) << failure.what();
        }
    }
} // namespace

// 钉住：一次创建同时写入 rollout、三波次与四目标，读取聚合时按波次与目标还原
TEST(FirmwareRolloutRepository, CreatesAndReadsAggregate)
{
    EdgePulse::TestSupport::AuthTestContext        context;
    EdgePulse::TestSupport::EventLoopRunner        runner;
    EdgePulse::Firmware::FirmwareRepository        firmware(context.database);
    EdgePulse::Device::NodeRepository              nodes(context.database);
    EdgePulse::Firmware::FirmwareRolloutRepository rollouts(context.database);
    const std::int64_t                             firmwareId = createFirmware(runner, firmware, "3.0.0");
    const std::vector<std::int64_t>                nodeIds    = createNodes(runner, nodes, "rollout-aggregate");

    const std::int64_t rolloutId = runner.run(rollouts.create(runner.loop(), makeDraft(firmwareId, nodeIds)));
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
    const EdgePulse::Firmware::FirmwareRolloutTargetRecord *second = findTarget(*aggregate, nodeIds[1]);
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

// 钉住：状态推进按版本比较交换，旧版本与跨租户写入都落空，暂停、恢复与回滚同步维护时间戳与失败原因
TEST(FirmwareRolloutRepository, StatusCompareAndSwap)
{
    EdgePulse::TestSupport::AuthTestContext        context;
    EdgePulse::TestSupport::EventLoopRunner        runner;
    EdgePulse::Firmware::FirmwareRepository        firmware(context.database);
    EdgePulse::Device::NodeRepository              nodes(context.database);
    EdgePulse::Firmware::FirmwareRolloutRepository rollouts(context.database);
    const std::int64_t                             firmwareId = createFirmware(runner, firmware, "3.0.1");
    const std::vector<std::int64_t>                nodeIds    = createNodes(runner, nodes, "rollout-status");
    const std::int64_t                             rolloutId  = runner.run(rollouts.create(runner.loop(), makeDraft(firmwareId, nodeIds)));

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

    // 完成时记录终止时间与失败原因
    EXPECT_TRUE(runner.run(rollouts.updateStatus(runner.loop(), rolloutId, 4, "completed", 3, "", "第三次失败")));
    aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_EQ("completed", aggregate->rollout.status);
    EXPECT_EQ(3, aggregate->rollout.currentWave);
    EXPECT_FALSE(aggregate->rollout.finishedAt.empty());
    EXPECT_EQ("第三次失败", aggregate->rollout.lastError);

    // 回滚属于活动态：终止时间清空，未提供新原因时旧失败原因也不保留
    EXPECT_TRUE(runner.run(rollouts.updateStatus(runner.loop(), rolloutId, 5, "rolling_back", 2)));
    aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_EQ("rolling_back", aggregate->rollout.status);
    EXPECT_TRUE(aggregate->rollout.finishedAt.empty());
    EXPECT_TRUE(aggregate->rollout.lastError.empty());

    // 失败终止记录终止时间与新的原因
    EXPECT_TRUE(runner.run(rollouts.updateStatus(runner.loop(), rolloutId, 6, "failed", 3, "", "回滚失败")));
    aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_FALSE(aggregate->rollout.finishedAt.empty());
    EXPECT_EQ("回滚失败", aggregate->rollout.lastError);

    // 再次恢复运行清掉终止时间与失败原因
    EXPECT_TRUE(runner.run(rollouts.updateStatus(runner.loop(), rolloutId, 7, "running", 0)));
    aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_TRUE(aggregate->rollout.finishedAt.empty());
    EXPECT_TRUE(aggregate->rollout.lastError.empty());
    EXPECT_FALSE(aggregate->rollout.startedAt.empty());

    // 跨租户写入按不存在处理
    const auto otherTenant = rollouts.scoped(EdgePulse::System::TenantScope{2, false});
    EXPECT_FALSE(runner.run(otherTenant->updateStatus(runner.loop(), rolloutId, 8, "aborted")));
}

// 钉住：同一节点在一个发布里只能出现一次；唯一冲突让整笔事务回滚，不留下半份聚合
TEST(FirmwareRolloutRepository, RejectsDuplicateNodeAndRollsBack)
{
    EdgePulse::TestSupport::AuthTestContext        context;
    EdgePulse::TestSupport::EventLoopRunner        runner;
    EdgePulse::Firmware::FirmwareRepository        firmware(context.database);
    EdgePulse::Device::NodeRepository              nodes(context.database);
    EdgePulse::Firmware::FirmwareRolloutRepository rollouts(context.database);
    const std::int64_t                             firmwareId = createFirmware(runner, firmware, "3.0.2");
    const std::vector<std::int64_t>                nodeIds    = createNodes(runner, nodes, "rollout-duplicate");

    EdgePulse::Firmware::FirmwareRolloutDraft draft = makeDraft(firmwareId, nodeIds);
    draft.waves[1].targets[0].nodeId                = draft.waves[0].targets[0].nodeId;
    try
    {
        static_cast<void>(runner.run(rollouts.create(runner.loop(), draft)));
        FAIL() << "同一节点重复出现时创建必须失败";
    } catch (const EdgePulse::Common::DatabaseException &failure)
    {
        EXPECT_NE(std::string::npos, std::string{failure.what()}.find("重复")) << failure.what();
    }

    // 创建先写聚合再写波次与目标，唯一冲突后不能留下 rollout 头
    EXPECT_EQ(0, runner.run(rollouts.list(runner.loop(), firmwareId, EdgePulse::Common::PageParameters{1, 10})).total);
}

// 钉住：目标节点必须存在、未删除且属于发布租户，任何一条不满足都拒绝整笔创建
TEST(FirmwareRolloutRepository, RejectsMissingDeletedAndCrossTenantTargets)
{
    EdgePulse::TestSupport::AuthTestContext        context;
    EdgePulse::TestSupport::EventLoopRunner        runner;
    EdgePulse::Firmware::FirmwareRepository        firmware(context.database);
    EdgePulse::Device::NodeRepository              nodes(context.database);
    EdgePulse::Firmware::FirmwareRolloutRepository rollouts(context.database);
    const std::int64_t                             firmwareId = createFirmware(runner, firmware, "3.0.5");
    const std::vector<std::int64_t>                nodeIds    = createNodes(runner, nodes, "rollout-invalid-target");

    // 不存在的节点
    std::vector<std::int64_t> missing = nodeIds;
    missing[3]                        = 999999999;
    expectCreateRejected(runner, rollouts, makeDraft(firmwareId, missing), "目标节点");

    // 已软删除的节点
    ASSERT_TRUE(runner.run(nodes.remove(runner.loop(), nodeIds[3])));
    expectCreateRejected(runner, rollouts, makeDraft(firmwareId, nodeIds), "目标节点");

    // 另一租户的节点
    const auto                otherTenantNodes = nodes.scoped(EdgePulse::System::TenantScope{2, false});
    const std::int64_t        foreignNodeId    = runner.run(otherTenantNodes->create(runner.loop(), "rollout-foreign-target", "{}")).id;
    std::vector<std::int64_t> mixed            = nodeIds;
    mixed[3]                                   = foreignNodeId;
    expectCreateRejected(runner, rollouts, makeDraft(firmwareId, mixed), "目标节点");

    // 三次拒绝都不能留下发布头
    EXPECT_EQ(0, runner.run(rollouts.list(runner.loop(), firmwareId, EdgePulse::Common::PageParameters{1, 10})).total);
}

// 钉住：没有波次或波次下标重复的聚合在写库前就被拒绝
TEST(FirmwareRolloutRepository, RejectsInvalidWavePlan)
{
    EdgePulse::TestSupport::AuthTestContext        context;
    EdgePulse::TestSupport::EventLoopRunner        runner;
    EdgePulse::Firmware::FirmwareRepository        firmware(context.database);
    EdgePulse::Device::NodeRepository              nodes(context.database);
    EdgePulse::Firmware::FirmwareRolloutRepository rollouts(context.database);
    const std::int64_t                             firmwareId = createFirmware(runner, firmware, "3.0.6");
    const std::vector<std::int64_t>                nodeIds    = createNodes(runner, nodes, "rollout-wave-plan");

    EdgePulse::Firmware::FirmwareRolloutDraft zeroWaves;
    zeroWaves.rollout.firmwareId = firmwareId;
    zeroWaves.rollout.name       = "空波次";
    expectCreateRejected(runner, rollouts, std::move(zeroWaves), "波次");

    EdgePulse::Firmware::FirmwareRolloutDraft duplicateWaves = makeDraft(firmwareId, nodeIds);
    duplicateWaves.waves[1].wave.waveIndex                   = duplicateWaves.waves[0].wave.waveIndex;
    expectCreateRejected(runner, rollouts, std::move(duplicateWaves), "波次下标");

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

// 钉住：目标派发权由发布头版本、波次与目标状态共同约束，重复抢占只会成功一次
TEST(FirmwareRolloutRepository, ClaimTargetFencesConcurrentDispatch)
{
    EdgePulse::TestSupport::AuthTestContext        context;
    EdgePulse::TestSupport::EventLoopRunner        runner;
    EdgePulse::Firmware::FirmwareRepository        firmware(context.database);
    EdgePulse::Device::NodeRepository              nodes(context.database);
    EdgePulse::Firmware::FirmwareRolloutRepository rollouts(context.database);
    const std::int64_t                             firmwareId = createFirmware(runner, firmware, "3.0.8");
    const std::vector<std::int64_t>                nodeIds    = createNodes(runner, nodes, "rollout-claim");
    const std::int64_t                             rolloutId  = runner.run(rollouts.create(runner.loop(), makeDraft(firmwareId, nodeIds)));

    auto aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    const EdgePulse::Firmware::FirmwareRolloutTargetRecord target = aggregate->targets.front();
    const std::string                                       staleBefore = EdgePulse::Common::nowDateTimeUtc();

    EXPECT_EQ(EdgePulse::Firmware::FirmwareRolloutClaimResult::Claimed,
              runner.run(rollouts.claimTarget(runner.loop(), rolloutId, target.id, aggregate->rollout.version, 0, "pending", 0, 0, false, "{}", staleBefore)));
    EXPECT_EQ(EdgePulse::Firmware::FirmwareRolloutClaimResult::TargetNotClaimable,
              runner.run(rollouts.claimTarget(runner.loop(), rolloutId, target.id, aggregate->rollout.version, 0, "pending", 0, 0, false, "{}", staleBefore)));
    EXPECT_EQ(EdgePulse::Firmware::FirmwareRolloutClaimResult::RolloutChanged,
              runner.run(rollouts.claimTarget(runner.loop(), rolloutId, target.id, aggregate->rollout.version + 1, 0, "dispatching", 0, 0, false, "{}", staleBefore)));

    EdgePulse::Firmware::FirmwareRolloutTargetUpdate restore;
    restore.status     = "pending";
    restore.taskId     = 0;
    restore.commandId  = 0;
    restore.healthJson = "{}";
    EXPECT_TRUE(runner.run(rollouts.releaseTarget(runner.loop(), target.id, "dispatching", restore)));
    aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_EQ("pending", aggregate->targets.front().status);
}

// 钉住：同固件同节点的在途任务拒绝重复创建，安装完成后才允许下一次；迟到回报不能复活失败任务
TEST(FirmwareRolloutRepository, TaskCreationAndTerminalStatusAreIdempotent)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::Firmware::FirmwareRepository firmware(context.database);
    EdgePulse::Device::NodeRepository       nodes(context.database);
    const std::int64_t                      firmwareId = createFirmware(runner, firmware, "3.0.9");
    const std::int64_t                      nodeId     = runner.run(nodes.create(runner.loop(), "rollout-task-idempotent", "{}")).id;

    const std::int64_t first = runner.run(firmware.createTask(runner.loop(), firmwareId, nodeId, 0, 0, 0, 0));
    ASSERT_GT(first, 0);
    const auto firstRow = runner.run(firmware.findTask(runner.loop(), first));
    ASSERT_TRUE(firstRow.has_value());
    EXPECT_EQ(0, firstRow->commandId) << "未绑定命令在对外记录里仍按 0 表示";

    EXPECT_EQ(0, runner.run(firmware.createTask(runner.loop(), firmwareId, nodeId, 0, 0, 0, 0))) << "pending 任务已占用节点";
    ASSERT_TRUE(runner.run(firmware.updateTaskById(runner.loop(), first, "installing", 50, "安装中")));
    EXPECT_EQ(0, runner.run(firmware.createTask(runner.loop(), firmwareId, nodeId, 0, 0, 0, 0))) << "installing 任务仍占用节点";

    EXPECT_TRUE(runner.run(firmware.bindTaskCommand(runner.loop(), first, 501)));
    EXPECT_TRUE(runner.run(firmware.bindTaskCommand(runner.loop(), first, 501))) << "同一命令重复绑定应当幂等";
    EXPECT_FALSE(runner.run(firmware.bindTaskCommand(runner.loop(), first, 502))) << "已绑定的任务不能被换绑";
    const auto bound = runner.run(firmware.findTask(runner.loop(), first));
    ASSERT_TRUE(bound.has_value());
    EXPECT_EQ(501, bound->commandId);

    ASSERT_TRUE(runner.run(firmware.updateTaskById(runner.loop(), first, "installed", 100, "完成")));
    const std::int64_t second = runner.run(firmware.createTask(runner.loop(), firmwareId, nodeId, 0, 0, 0, 0));
    ASSERT_GT(second, 0) << "安装完成后允许下一次升级";
    ASSERT_TRUE(runner.run(firmware.bindTaskCommand(runner.loop(), second, 502)));
    ASSERT_TRUE(runner.run(firmware.updateTaskById(runner.loop(), second, "installed", 100, "完成")));
    EXPECT_FALSE(runner.run(firmware.updateTaskById(runner.loop(), second, "failed", 0, "夜间维护失败"))) << "已安装任务不能被迟到的失败改写";

    const std::int64_t third = runner.run(firmware.createTask(runner.loop(), firmwareId, nodeId, 0, 0, 0, 0));
    ASSERT_GT(third, 0);
    ASSERT_TRUE(runner.run(firmware.bindTaskCommand(runner.loop(), third, 503)));
    ASSERT_TRUE(runner.run(firmware.updateTaskById(runner.loop(), third, "failed", 0, "命令已取消")));
    EXPECT_FALSE(runner.run(firmware.updateTask(runner.loop(), firmwareId, nodeId, "installed", 100, "迟到回报", 503)).has_value())
            << "取消后的任务不能被迟到 installed 回报复活";
    const auto terminal = runner.run(firmware.findTask(runner.loop(), third));
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ("failed", terminal->status);
}

// 钉住：心跳写入 Agent 上报的已安装固件版本，节点 JSON 同步透出；省略该参数时保留原值
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
    const AsynGyanis::Base::ConfigValue json = EdgePulse::Device::nodeToJson(beat->node);
    EXPECT_EQ("2.0.0", json["installedFirmwareVersion"].get<std::string>());

    // 旧 Agent 不带版本：只刷新 Agent 版本，已安装固件版本保持不变
    const auto legacy = runner.run(nodes.heartbeat(runner.loop(), nodeId, "1.2.1"));
    ASSERT_TRUE(legacy.has_value());
    EXPECT_EQ("1.2.1", legacy->node.agentVersion);
    EXPECT_EQ("2.0.0", legacy->node.installedFirmwareVersion);
    EXPECT_EQ("2.0.0", EdgePulse::Device::nodeToJson(legacy->node)["installedFirmwareVersion"].get<std::string>());

    const std::int64_t freshNodeId = runner.run(nodes.create(runner.loop(), "rollout-fresh-node", "{}")).id;
    EXPECT_TRUE(runner.run(nodes.findById(runner.loop(), freshNodeId))->installedFirmwareVersion.empty());
    EXPECT_TRUE(EdgePulse::Device::nodeToJson(*runner.run(nodes.findById(runner.loop(), freshNodeId)))["installedFirmwareVersion"].get<std::string>().empty());
}

// 钉住：波次时间戳、目标健康快照与门控快照可分别更新；目标更新受租户、所属发布波次约束
TEST(FirmwareRolloutRepository, UpdatesWaveTargetGateAndListsDue)
{
    EdgePulse::TestSupport::AuthTestContext        context;
    EdgePulse::TestSupport::EventLoopRunner        runner;
    EdgePulse::Firmware::FirmwareRepository        firmware(context.database);
    EdgePulse::Device::NodeRepository              nodes(context.database);
    EdgePulse::Firmware::FirmwareRolloutRepository rollouts(context.database);
    const std::int64_t                             firmwareId = createFirmware(runner, firmware, "3.0.4");
    const std::vector<std::int64_t>                nodeIds    = createNodes(runner, nodes, "rollout-wave-target");

    EdgePulse::Firmware::FirmwareRolloutDraft draft     = makeDraft(firmwareId, nodeIds);
    const std::int64_t                        rolloutId = runner.run(rollouts.create(runner.loop(), draft));
    auto                                      aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    const std::int64_t firstWaveId = aggregate->waves[0].id;
    const auto        *firstTarget = findTarget(*aggregate, nodeIds[0]);
    ASSERT_NE(nullptr, firstTarget);
    const std::int64_t firstTargetId = firstTarget->id;

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

    EdgePulse::Firmware::FirmwareRolloutTargetUpdate targetUpdate;
    targetUpdate.status     = "healthy";
    targetUpdate.healthJson = R"({"command":"succeeded"})";
    targetUpdate.taskId     = 501;
    targetUpdate.commandId  = 601;
    EXPECT_TRUE(runner.run(rollouts.updateTarget(runner.loop(), firstTargetId, targetUpdate)));
    aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    firstTarget = findTarget(*aggregate, nodeIds[0]);
    ASSERT_NE(nullptr, firstTarget);
    EXPECT_EQ("healthy", firstTarget->status);
    EXPECT_EQ(R"({"command":"succeeded"})", firstTarget->healthJson);
    EXPECT_EQ(501, firstTarget->taskId);
    EXPECT_EQ(601, firstTarget->commandId);

    // 只更新健康快照时状态与关联字段保持原值
    EdgePulse::Firmware::FirmwareRolloutTargetUpdate healthOnly;
    healthOnly.healthJson = R"({"gate":"ok"})";
    EXPECT_TRUE(runner.run(rollouts.updateTarget(runner.loop(), firstTargetId, healthOnly)));
    aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    firstTarget = findTarget(*aggregate, nodeIds[0]);
    ASSERT_NE(nullptr, firstTarget);
    EXPECT_EQ("healthy", firstTarget->status);
    EXPECT_EQ(R"({"gate":"ok"})", firstTarget->healthJson);
    EXPECT_EQ(501, firstTarget->taskId);
    EXPECT_EQ(601, firstTarget->commandId);

    // 目标只能移到同一发布的波次：另一发布的波次下标命中不了
    EdgePulse::Firmware::FirmwareRolloutDraft secondDraft = makeDraft(firmwareId, nodeIds);
    secondDraft.rollout.name                              = "灰度发布 B";
    secondDraft.rollout.status                            = "draft";
    const std::int64_t secondRolloutId                    = runner.run(rollouts.create(runner.loop(), secondDraft));
    const auto         secondAggregate                    = runner.run(rollouts.find(runner.loop(), secondRolloutId));
    ASSERT_TRUE(secondAggregate.has_value());
    EdgePulse::Firmware::FirmwareRolloutTargetUpdate crossRollout;
    crossRollout.waveId = secondAggregate->waves[1].id;
    EXPECT_FALSE(runner.run(rollouts.updateTarget(runner.loop(), firstTargetId, crossRollout)));
    aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    firstTarget = findTarget(*aggregate, nodeIds[0]);
    ASSERT_NE(nullptr, firstTarget);
    EXPECT_EQ(firstWaveId, firstTarget->waveId);

    // 同一发布的波次可以正常指定
    EdgePulse::Firmware::FirmwareRolloutTargetUpdate ownWave;
    ownWave.waveId = firstWaveId;
    EXPECT_TRUE(runner.run(rollouts.updateTarget(runner.loop(), firstTargetId, ownWave)));

    const auto otherTenant = rollouts.scoped(EdgePulse::System::TenantScope{2, false});
    EXPECT_FALSE(runner.run(otherTenant->setWave(runner.loop(), firstWaveId, "failed")));
    EXPECT_FALSE(runner.run(otherTenant->appendGate(runner.loop(), firstWaveId, R"({"passed":0})")));
    EXPECT_FALSE(runner.run(otherTenant->updateTarget(runner.loop(), firstTargetId, healthOnly)));
    aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    firstTarget = findTarget(*aggregate, nodeIds[0]);
    ASSERT_NE(nullptr, firstTarget);
    EXPECT_EQ(R"({"gate":"ok"})", firstTarget->healthJson);

    // 草稿不进入待推进集合
    EdgePulse::Firmware::FirmwareRolloutDraft draftOnly = makeDraft(firmwareId, nodeIds);
    draftOnly.rollout.status                            = "draft";
    ASSERT_GT(runner.run(rollouts.create(runner.loop(), draftOnly)), 0);
    EXPECT_EQ(1U, runner.run(rollouts.listDueRollouts(runner.loop(), 10)).size());
}

// 钉住：原因文本按列上限截断，且不会切断 UTF-8 多字节字符
TEST(FirmwareRolloutRepository, TruncatesReasonsAtColumnLimits)
{
    EdgePulse::TestSupport::AuthTestContext        context;
    EdgePulse::TestSupport::EventLoopRunner        runner;
    EdgePulse::Firmware::FirmwareRepository        firmware(context.database);
    EdgePulse::Device::NodeRepository              nodes(context.database);
    EdgePulse::Firmware::FirmwareRolloutRepository rollouts(context.database);
    const std::int64_t                             firmwareId = createFirmware(runner, firmware, "3.0.7");
    const std::vector<std::int64_t>                nodeIds    = createNodes(runner, nodes, "rollout-reason-truncate");
    const std::int64_t                             rolloutId  = runner.run(rollouts.create(runner.loop(), makeDraft(firmwareId, nodeIds)));

    const std::string unit = "中";
    std::string       pauseReason;
    for (int index = 0; index < 200; ++index)
    {
        pauseReason += unit;
    }
    std::string lastError = "a";
    for (int index = 0; index < 200; ++index)
    {
        lastError += unit;
    }
    ASSERT_TRUE(runner.run(rollouts.updateStatus(runner.loop(), rolloutId, 1, "paused", 0, pauseReason, lastError)));
    const auto aggregate = runner.run(rollouts.find(runner.loop(), rolloutId));
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_EQ(240U, aggregate->rollout.pauseReason.size());
    EXPECT_EQ(80U * unit.size(), aggregate->rollout.pauseReason.size());
    EXPECT_EQ(unit, aggregate->rollout.pauseReason.substr(237));
    EXPECT_EQ(478U, aggregate->rollout.lastError.size());
    EXPECT_EQ(1U + 159U * unit.size(), aggregate->rollout.lastError.size());
    EXPECT_EQ('a', aggregate->rollout.lastError.front());
    EXPECT_EQ(unit, aggregate->rollout.lastError.substr(aggregate->rollout.lastError.size() - unit.size()));
}
