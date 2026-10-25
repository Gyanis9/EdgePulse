/**
 * @file FirmwareRolloutService.h
 * @brief 固件灰度发布的创建、人工动作与单飞推进
 * @author Gyanis
 * @date 2026-10-26
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Api/Pagination.h"
#include "Core/Firmware/FirmwareRolloutPolicy.h"
#include "Core/Firmware/FirmwareRolloutRepository.h"
#include "Core/System/TenantScope.h"

#include <Core/Coroutine/Task.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}
namespace EdgePulse::Alarm
{
    class AlarmRepository;
}
namespace EdgePulse::Command
{
    class CommandRepository;
    struct CommandRecord;
} // namespace EdgePulse::Command
namespace EdgePulse::Device
{
    class NodeRepository;
    struct NodeRecord;
} // namespace EdgePulse::Device
namespace EdgePulse::Firmware
{
    class FirmwareRepository;
    struct FirmwareRecord;
    struct FirmwareSigningPolicy;
} // namespace EdgePulse::Firmware
namespace EdgePulse::Telemetry
{
    class TelemetryRepository;
}

namespace EdgePulse::Firmware
{
    /**
     * @brief 业务拒绝的类别
     */
    enum class FirmwareRolloutErrorKind
    {
        InvalidInput, ///< 参数或策略不合法
        NotFound,     ///< 发布、固件或节点不存在
        Conflict,     ///< 当前状态不允许该动作
        Internal,     ///< 内部状态不一致
    };

    /**
     * @brief 业务拒绝
     */
    struct FirmwareRolloutError
    {
        FirmwareRolloutErrorKind kind{FirmwareRolloutErrorKind::InvalidInput}; ///< 类别
        std::string              message;                                      ///< 中文原因
    };

    /**
     * @brief 服务返回类型
     */
    template<typename Value>
    using FirmwareRolloutResult = std::expected<Value, FirmwareRolloutError>;

    /**
     * @brief 把新建命令送达 Agent 的发布口
     * @note 生产装配走实时总线；测试注入记录器即可断言派发内容
     */
    using FirmwareCommandPublisher = std::function<AsynGyanis::Core::Task<>(const Command::CommandRecord &)>;

    /**
     * @brief 灰度服务依赖
     * @note 仓储都以全租户视图保存，每个入口再按调用方租户边界派生视图
     */
    struct FirmwareRolloutDependencies
    {
        std::shared_ptr<FirmwareRepository>             firmware;                     ///< 固件与任务
        std::shared_ptr<FirmwareRolloutRepository>      rollouts;                     ///< 灰度聚合
        std::shared_ptr<Command::CommandRepository>     commands;                     ///< 命令
        std::shared_ptr<Device::NodeRepository>         nodes;                        ///< 节点
        std::shared_ptr<Telemetry::TelemetryRepository> telemetry;                    ///< 遥测
        std::shared_ptr<Alarm::AlarmRepository>         alarms;                       ///< 告警
        FirmwareCommandPublisher                        publisher;                    ///< 命令发布
        std::shared_ptr<FirmwareSigningPolicy>          signingPolicy;                ///< 签名门禁，空表示不强制
        int                                             heartbeatIntervalSeconds{30}; ///< Agent 心跳间隔，用于校验 settle
        std::size_t                                     dueLimit{20};                 ///< 每 tick 最多推进多少个发布
        int                                             commandTimeoutSeconds{600};   ///< 固件命令超时秒数
    };

    /**
     * @brief 灰度协调服务
     * @details 所有写操作都以发布版本做比较交换；推进只由 leader 周期调用，服务自身不判定领导权
     */
    class FirmwareRolloutService
    {
    public:
        /**
         * @brief 绑定依赖
         * @param dependencies 仓储、签名策略与命令发布口
         * @throws std::invalid_argument 必需仓储或发布口为空
         */
        explicit FirmwareRolloutService(FirmwareRolloutDependencies dependencies);

        /**
         * @brief 创建灰度发布并物化波次与目标
         * @param loop 恢复协程用的事件循环
         * @param scope 调用方租户边界
         * @param actorId 创建人
         * @param firmwareId 目标固件
         * @param name 发布名称
         * @param policyJson 策略 JSON
         * @param nodeIds 目标节点，1 到 1000 个且不能重复
         * @return FirmwareRolloutResult<std::int64_t> 发布标识；拒绝时带中文原因
         */
        AsynGyanis::Core::Task<FirmwareRolloutResult<std::int64_t>> create(AsynGyanis::Core::EventLoop &loop, System::TenantScope scope, std::int64_t actorId,
                                                                           std::int64_t firmwareId, std::string name, std::string policyJson, std::vector<std::int64_t> nodeIds);

        /**
         * @brief 读取完整聚合
         * @param loop 恢复协程用的事件循环
         * @param scope 租户边界
         * @param rolloutId 发布标识
         * @return std::optional<FirmwareRolloutAggregate> 发布、波次与目标
         */
        AsynGyanis::Core::Task<std::optional<FirmwareRolloutAggregate>> find(AsynGyanis::Core::EventLoop &loop, System::TenantScope scope, std::int64_t rolloutId);

        /**
         * @brief 分页查询发布
         * @param loop 恢复协程用的事件循环
         * @param scope 租户边界
         * @param firmwareId 固件过滤，0 表示全部
         * @param page 分页
         * @return Common::PageRows<FirmwareRolloutRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<FirmwareRolloutRecord>> list(AsynGyanis::Core::EventLoop &loop, System::TenantScope scope, std::int64_t firmwareId,
                                                                             const Common::PageParameters &page);

        /// 暂停；只接受 scheduled、preparing 与 running
        AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> pause(AsynGyanis::Core::EventLoop &loop, System::TenantScope scope, std::int64_t rolloutId, std::string reason);

        /// 恢复；只接受 paused，并重新开启当前波次的观察窗口
        AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> resume(AsynGyanis::Core::EventLoop &loop, System::TenantScope scope, std::int64_t rolloutId,
                                                                   std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

        /// 人工提升；跳过当前波次直接进入下一波，最后一波则完成
        AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> promote(AsynGyanis::Core::EventLoop &loop, System::TenantScope scope, std::int64_t rolloutId,
                                                                    std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

        /// 强制提升；必须给出非空原因，原因写入当前波次的门控快照
        AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> forcePromote(AsynGyanis::Core::EventLoop &loop, System::TenantScope scope, std::int64_t rolloutId, std::string reason,
                                                                         std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

        /// 中止；任何非终止状态都可转入 aborted
        AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> abort(AsynGyanis::Core::EventLoop &loop, System::TenantScope scope, std::int64_t rolloutId,
                                                                  std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

        /// 人工回滚；允许 running、paused 与 failed，实际命令由下一次 tick 下发
        AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> rollback(AsynGyanis::Core::EventLoop &loop, System::TenantScope scope, std::int64_t rolloutId);

        /**
         * @brief 推进所有到期发布
         * @param loop 恢复协程用的事件循环
         * @param now 服务端当前时刻；测试注入即可完全确定门控时间
         * @return AsynGyanis::Core::Task<> 单个发布失败只记日志，不阻断其余发布
         * @note 只应由 leader 的 LeaderTick 调用，本方法不做领导权判定
         */
        AsynGyanis::Core::Task<> tick(AsynGyanis::Core::EventLoop &loop, std::chrono::system_clock::time_point now);

    private:
        using TimePoint = std::chrono::system_clock::time_point;

        /**
         * @brief 单个目标在某一拍的门控结论
         */
        struct TargetGateResult
        {
            std::int64_t targetId{0};          ///< 目标标识
            std::int64_t nodeId{0};            ///< 节点标识
            std::string  status;               ///< 建议写入的目标状态
            std::string  healthJson;           ///< 健康快照
            bool         healthy{false};       ///< 通过
            bool         failed{false};        ///< 明确失败
            bool         indeterminate{false}; ///< 尚未有结论
            std::string  reason;               ///< 中文原因
        };

        AsynGyanis::Core::Task<> tickOne(AsynGyanis::Core::EventLoop &loop, std::int64_t rolloutId, TimePoint now);
        AsynGyanis::Core::Task<> dispatchWave(AsynGyanis::Core::EventLoop &loop, FirmwareRolloutAggregate aggregate, std::size_t waveIndex, const FirmwareRolloutPolicy &policy,
                                              TimePoint now);
        AsynGyanis::Core::Task<> gateWave(AsynGyanis::Core::EventLoop &loop, FirmwareRolloutAggregate aggregate, std::size_t waveIndex, const FirmwareRolloutPolicy &policy,
                                          TimePoint now);
        AsynGyanis::Core::Task<> dispatchRollback(AsynGyanis::Core::EventLoop &loop, FirmwareRolloutAggregate aggregate);
        AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> transition(AsynGyanis::Core::EventLoop &loop, System::TenantScope scope, std::int64_t rolloutId,
                                                                       const std::vector<std::string> &allowed, std::string status, std::string pauseReason = {},
                                                                       std::string lastError = {});
        AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> promoteInternal(AsynGyanis::Core::EventLoop &loop, System::TenantScope scope, std::int64_t rolloutId,
                                                                            std::string reason, bool forced, TimePoint now);

        AsynGyanis::Core::Task<TargetGateResult> evaluateTarget(AsynGyanis::Core::EventLoop &loop, const FirmwareRolloutRecord &rollout, const FirmwareRolloutTargetRecord &target,
                                                                const FirmwareRecord &targetFirmware, const FirmwareRolloutPolicy &policy, TimePoint gateStart, TimePoint settleEnd,
                                                                TimePoint observeEnd, TimePoint now);
        AsynGyanis::Core::Task<>                 persistGate(AsynGyanis::Core::EventLoop &loop, const FirmwareRolloutTargetRecord &target, const TargetGateResult &result);

        AsynGyanis::Core::Task<std::int64_t> resolvePreviousFirmware(AsynGyanis::Core::EventLoop &loop, const FirmwareRecord &firmware, const Device::NodeRecord &node);
        AsynGyanis::Core::Task<bool>         failTarget(AsynGyanis::Core::EventLoop &loop, const FirmwareRolloutTargetRecord &target, std::string reason);
        AsynGyanis::Core::Task<std::int64_t> existingTask(AsynGyanis::Core::EventLoop &loop, std::int64_t firmwareId, std::int64_t rolloutId, std::int64_t waveId,
                                                          std::int64_t nodeId);

    private:
        std::shared_ptr<FirmwareRepository>             m_firmware;             ///< 租户视图入口，实际调用先 scoped
        std::shared_ptr<FirmwareRolloutRepository>      m_rollouts;             ///< 租户视图入口
        std::shared_ptr<Command::CommandRepository>     m_commands;             ///< 命令
        std::shared_ptr<Device::NodeRepository>         m_nodes;                ///< 节点
        std::shared_ptr<Telemetry::TelemetryRepository> m_telemetry;            ///< 遥测
        std::shared_ptr<Alarm::AlarmRepository>         m_alarms;               ///< 告警
        std::shared_ptr<FirmwareRolloutRepository>      m_allRollouts;          ///< 全租户灰度视图
        std::shared_ptr<FirmwareRepository>             m_allFirmware;          ///< 全租户固件视图
        std::shared_ptr<Command::CommandRepository>     m_allCommands;          ///< 全租户命令视图
        std::shared_ptr<Device::NodeRepository>         m_allNodes;             ///< 全租户节点视图
        std::shared_ptr<Telemetry::TelemetryRepository> m_allTelemetry;         ///< 全租户遥测视图
        std::shared_ptr<Alarm::AlarmRepository>         m_allAlarms;            ///< 全租户告警视图
        FirmwareCommandPublisher                        m_publisher;            ///< 命令发布
        std::shared_ptr<FirmwareSigningPolicy>          m_signingPolicy;        ///< 签名门禁
        int                                             m_heartbeatSeconds{30}; ///< 心跳间隔
        std::size_t                                     m_dueLimit{20};         ///< 每 tick 上限
        int                                             m_commandTimeout{600};  ///< 命令超时
    };
} // namespace EdgePulse::Firmware
