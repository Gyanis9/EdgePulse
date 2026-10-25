/**
 * @file FirmwareRolloutRepository.h
 * @brief 固件灰度发布聚合仓储
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Api/Pagination.h"
#include "Base/Database/Database.h"
#include "Core/System/TenantScope.h"

#include <Core/Coroutine/Task.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Firmware
{
    /**
     * @brief 一次灰度发布的头部
     */
    struct FirmwareRolloutRecord
    {
        std::int64_t id{0};          ///< 主键
        std::int64_t tenantId{1};    ///< 归属租户
        std::int64_t firmwareId{0};  ///< 目标固件
        std::string  name;           ///< 发布名称
        std::string  policyJson;     ///< 波次与门控策略 JSON
        std::string  status;         ///< draft/scheduled/preparing/running/paused/rolling_back/completed/rolled_back/rollback_unavailable/aborted/failed
        int          currentWave{0}; ///< 当前波次下标，0 表示尚未推进
        int          totalWaves{0};  ///< 波次总数
        std::int64_t createdBy{0};   ///< 创建人
        std::string  createdAt;      ///< 创建时间
        std::string  startedAt;      ///< 首次进入运行的时间，可空
        std::string  finishedAt;     ///< 终止时间，可空
        std::string  pausedAt;       ///< 最近一次暂停时间，可空
        std::string  pauseReason;    ///< 暂停原因，可空
        std::string  lastError;      ///< 最近一次失败原因，可空
        std::int64_t version{1};     ///< 乐观锁版本
    };

    /**
     * @brief 灰度发布的一个波次
     */
    struct FirmwareRolloutWaveRecord
    {
        std::int64_t id{0};          ///< 主键
        std::int64_t rolloutId{0};   ///< 所属发布
        int          waveIndex{0};   ///< 波次下标，从 0 开始
        std::string  status;         ///< pending/dispatching/observing/passed/failed/skipped/rolling_back/rolled_back
        int          targetCount{0}; ///< 本波目标数
        std::string  dispatchedAt;   ///< 派发时间，可空
        std::string  deadlineAt;     ///< 观察截止时间，可空
        std::string  settledAt;      ///< 门控判定时间，可空
        std::string  gateJson;       ///< 门控快照，每行一个 JSON
        std::string  createdAt;      ///< 创建时间
        std::string  updatedAt;      ///< 更新时间
    };

    /**
     * @brief 灰度发布中的一个节点目标
     */
    struct FirmwareRolloutTargetRecord
    {
        std::int64_t id{0};                 ///< 主键
        std::int64_t rolloutId{0};          ///< 所属发布
        std::int64_t waveId{0};             ///< 所属波次
        std::int64_t tenantId{1};           ///< 归属租户
        std::int64_t nodeId{0};             ///< 目标节点
        std::int64_t previousFirmwareId{0}; ///< 下发前已安装固件，0 表示未知
        std::int64_t taskId{0};             ///< 关联的固件任务
        std::int64_t commandId{0};          ///< 关联的固件命令
        std::string  status;                ///< pending/dispatching/installing/healthy/failed/rolling_back/rolled_back
        std::string  healthJson;            ///< 最近一次健康快照 JSON
        std::string  updatedAt;             ///< 更新时间
    };

    /**
     * @brief 创建用的波次草稿
     * @details 仓储生成波次主键后回填到本波目标，调用方传入的主键会被覆盖。
     */
    struct FirmwareRolloutWaveDraft
    {
        FirmwareRolloutWaveRecord                wave;    ///< 波次字段；id 为 0 时由仓储生成
        std::vector<FirmwareRolloutTargetRecord> targets; ///< 本波目标；waveId 与 tenantId 由仓储回填
    };

    /**
     * @brief 创建灰度发布的输入
     */
    struct FirmwareRolloutDraft
    {
        FirmwareRolloutRecord                 rollout; ///< 发布头部；租户、主键与总波次数由仓储决定
        std::vector<FirmwareRolloutWaveDraft> waves;   ///< 波次与目标；至少一项
    };

    /**
     * @brief 读取聚合时的完整发布
     */
    struct FirmwareRolloutAggregate
    {
        FirmwareRolloutRecord                    rollout; ///< 发布头部
        std::vector<FirmwareRolloutWaveRecord>   waves;   ///< 按 wave_index 升序
        std::vector<FirmwareRolloutTargetRecord> targets; ///< 按 wave_id、id 升序
    };

    /**
     * @brief 目标节点的数据范围过滤
     * @details 与节点列表使用同一套判据：全部、仅本人创建，或指定部门集合。
     */
    struct FirmwareRolloutTargetScope
    {
        bool                      all{true};      ///< 可见全部目标
        bool                      selfOnly{false}; ///< 仅本人创建的节点
        std::int64_t              userId{0};       ///< 当前账号
        std::vector<std::int64_t> deptIds;         ///< 可见部门；all 为假且非 selfOnly 时生效
    };

    /**
     * @brief 抢占一个待派发目标的结果
     */
    enum class FirmwareRolloutClaimResult
    {
        Claimed,          ///< 抢到派发权
        RolloutChanged,   ///< 发布状态、版本或当前波次已变化
        TargetNotClaimable, ///< 目标已被其他派发或已进入终局
    };

    /**
     * @brief 目标更新的可选字段
     * @details 字符串为空表示保持原值；整数字段为负表示保持原值。
     */
    struct FirmwareRolloutTargetUpdate
    {
        std::string  status;                 ///< 新状态，空表示不改
        std::string  healthJson;             ///< 健康快照，空表示不改
        std::int64_t taskId{-1};             ///< 固件任务，负表示不改
        std::int64_t commandId{-1};          ///< 固件命令，负表示不改
        std::int64_t previousFirmwareId{-1}; ///< 下发前固件，负表示不改
        std::int64_t waveId{-1};             ///< 调整到的波次，负表示不改；非负值必须属于同一发布
    };

    /**
     * @brief 固件灰度发布聚合仓储
     * @details 只持久化聚合与状态，不创建命令、不做健康判定；所有读写受构造时的租户边界约束。
     */
    class FirmwareRolloutRepository
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         * @param tenant 租户边界
         */
        explicit FirmwareRolloutRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant = {});

        /// 按租户边界派生仓储视图
        [[nodiscard]] std::shared_ptr<FirmwareRolloutRepository> scoped(System::TenantScope tenant) const;

        /**
         * @brief 在一个事务里写入发布、波次与目标
         * @param loop 恢复协程用的事件循环
         * @param draft 聚合草稿；租户取自仓储视图，主键一律由仓储生成
         * @return std::int64_t 发布标识
         * @throws Common::DatabaseException 固件不存在、波次为空或下标重复、目标节点不存在或不属于发布租户、同一节点重复出现，或数据库写入失败
         * @note 同一发布内 node_id 必须唯一；固件、目标节点与发布必须同租户
         * @note 至少需要一个波次；草稿也必须带完整波次计划，当前不提供追加波次接口
         */
        AsynGyanis::Core::Task<std::int64_t> create(AsynGyanis::Core::EventLoop &loop, FirmwareRolloutDraft draft);

        /**
         * @brief 读取完整聚合
         * @param loop 恢复协程用的事件循环
         * @param rolloutId 发布标识
         * @param scope 目标节点的数据范围；默认不过滤
         * @return std::optional<FirmwareRolloutAggregate> 发布、波次与目标；不在租户边界内时为空
         */
        AsynGyanis::Core::Task<std::optional<FirmwareRolloutAggregate>> find(AsynGyanis::Core::EventLoop &loop, std::int64_t rolloutId,
                                                                             const FirmwareRolloutTargetScope &scope = {});

        /**
         * @brief 分页查询发布
         * @param loop 恢复协程用的事件循环
         * @param firmwareId 固件过滤，0 表示全部
         * @param page 分页
         * @param scope 目标节点的数据范围；只返回至少有一个可见目标的发布
         * @return Common::PageRows<FirmwareRolloutRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<FirmwareRolloutRecord>> list(AsynGyanis::Core::EventLoop &loop, std::int64_t firmwareId, const Common::PageParameters &page,
                                                                             const FirmwareRolloutTargetScope &scope = {});

        /**
         * @brief 以版本比较交换推进发布状态
         * @param loop 恢复协程用的事件循环
         * @param rolloutId 发布标识
         * @param expectedVersion 调用方读到的版本
         * @param status 新状态
         * @param currentWave 当前波次；负表示不改
         * @param pauseReason 暂停原因；进入 running 时清空
         * @param lastError 失败原因；空表示保留原值，但进入 running 或 rolling_back 时旧值会被清空
         * @return true 状态已推进且版本加一
         * @return false 发布不存在、不在租户边界内或版本已变
         * @note 终止状态记录 finished_at；活动状态与回滚清空 finished_at，避免复用上一次终止时间
         * @note 暂停原因与失败原因分别按 240 与 480 字节截断，不切断 UTF-8 字符
         */
        AsynGyanis::Core::Task<bool> updateStatus(AsynGyanis::Core::EventLoop &loop, std::int64_t rolloutId, std::int64_t expectedVersion, std::string status, int currentWave = -1,
                                                  std::string pauseReason = {}, std::string lastError = {});

        /**
         * @brief 更新波次状态与时间戳
         * @param loop 恢复协程用的事件循环
         * @param waveId 波次标识
         * @param status 新状态
         * @param dispatchedAt 派发时间，空表示保留原值
         * @param deadlineAt 观察截止时间，空表示保留原值
         * @param settledAt 判定时间，空表示保留原值
         * @return true 命中本租户的波次
         * @return false 波次不存在或不在租户边界内
         */
        AsynGyanis::Core::Task<bool> setWave(AsynGyanis::Core::EventLoop &loop, std::int64_t waveId, std::string status, std::string dispatchedAt = {}, std::string deadlineAt = {},
                                             std::string settledAt = {});

        /**
         * @brief 追加一条门控快照
         * @param loop 恢复协程用的事件循环
         * @param waveId 波次标识
         * @param gateJson 一行 JSON 快照
         * @return true 已追加
         * @return false 波次不存在或不在租户边界内
         * @note 同一波次的多条快照以换行分隔，按写入顺序保留
         * @note 只应由控制面 leader 单飞调用；事务只保证单次读改写，跨实例追加仍靠 leader 串行化
         */
        AsynGyanis::Core::Task<bool> appendGate(AsynGyanis::Core::EventLoop &loop, std::int64_t waveId, std::string gateJson);

        /**
         * @brief 更新目标的关联与健康字段
         * @param loop 恢复协程用的事件循环
         * @param targetId 目标标识
         * @param update 要写入的字段；空字符串与负整数表示保持原值
         * @param expectedStatus 非空时只在目标仍处于该状态时写入
         * @return true 命中本租户的目标
         * @return false 目标不存在、不在租户边界内或状态已变化
         */
        AsynGyanis::Core::Task<bool> updateTarget(AsynGyanis::Core::EventLoop &loop, std::int64_t targetId, FirmwareRolloutTargetUpdate update,
                                                  std::string expectedStatus = {});

        /**
         * @brief 在发布头版本与目标状态同时满足时抢占一个目标的派发权
         * @param loop 恢复协程用的事件循环
         * @param rolloutId 发布标识
         * @param targetId 目标标识
         * @param expectedVersion 调用方读到的发布版本
         * @param waveIndex 当前波次
         * @param expectedStatus 调用方读到的目标状态
         * @param expectedTaskId 调用方读到的任务标识
         * @param expectedCommandId 调用方读到的命令标识
         * @param rollbackPhase true 表示回滚派发，只接受 rolling_back 的发布头
         * @param claimedHealth 抢占期间的占位健康快照
         * @param staleBefore 早于该时刻的 dispatching 目标允许被重新抢占
         * @return FirmwareRolloutClaimResult 抢占结论
         * @note 发布头版本、状态、当前波次与目标状态在同一事务里校验；失败时不留下任何写入
         */
        AsynGyanis::Core::Task<FirmwareRolloutClaimResult> claimTarget(AsynGyanis::Core::EventLoop &loop, std::int64_t rolloutId, std::int64_t targetId,
                                                                       std::int64_t expectedVersion, int waveIndex, std::string expectedStatus,
                                                                       std::int64_t expectedTaskId, std::int64_t expectedCommandId, bool rollbackPhase,
                                                                       std::string claimedHealth, std::string staleBefore);

        /**
         * @brief 释放抢占并恢复目标原有状态与绑定
         * @param loop 恢复协程用的事件循环
         * @param targetId 目标标识
         * @param expectedStatus 当前状态，必须为 dispatching
         * @param restore 恢复后的字段
         * @return true 恢复成功
         * @return false 目标已被其他动作改写
         */
        AsynGyanis::Core::Task<bool> releaseTarget(AsynGyanis::Core::EventLoop &loop, std::int64_t targetId, std::string expectedStatus,
                                                   FirmwareRolloutTargetUpdate restore);

        /**
         * @brief 取需要协调器推进的发布
         * @param loop 恢复协程用的事件循环
         * @param limit 本次最多返回多少条
         * @return std::vector<FirmwareRolloutRecord> scheduled、preparing、running 与 rolling_back 的发布，按创建时间升序
         * @note 平台协调器应使用 allTenants 视图；暂停与草稿需要人工动作，不进入本列表
         */
        AsynGyanis::Core::Task<std::vector<FirmwareRolloutRecord>> listDueRollouts(AsynGyanis::Core::EventLoop &loop, std::size_t limit);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
        System::TenantScope               m_tenant;   ///< 租户边界
    };
} // namespace EdgePulse::Firmware
