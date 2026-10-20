/**
 * @file AlarmService.h
 * @brief 遥测触发的告警评估与生命周期
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Alarm/AlarmRepository.h"
#include "Core/Telemetry/TelemetryModels.h"

#include <Core/Coroutine/Task.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Alarm
{
    /**
     * @brief 告警变更通知
     * @details 由 App 注入浏览器 WebSocket 与 SSE 广播；失败只记日志，不回滚告警落库
     */
    using AlarmNotifier = std::function<AsynGyanis::Core::Task<>(const AlarmRecord &)>;

    /**
     * @brief 告警评估服务
     */
    class AlarmService
    {
    public:
        /**
         * @brief 构造
         * @param repository 告警仓储
         * @param notifier 变更通知；可为空
         */
        AlarmService(std::shared_ptr<AlarmRepository> repository, AlarmNotifier notifier = {});

        /**
         * @brief 按一批遥测评估规则
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点
         * @param samples 样本；同一指标只取最后一条
         * @return std::vector<AlarmRecord> 本次新增或恢复的告警
         */
        AsynGyanis::Core::Task<std::vector<AlarmRecord>> evaluate(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId, const std::vector<Telemetry::TelemetrySample> &samples);

        /**
         * @brief 广播一条告警变更
         * @param alarm 告警
         * @return AsynGyanis::Core::Task<> 完成即返回；通知失败只记日志
         */
        AsynGyanis::Core::Task<> notify(const AlarmRecord &alarm);

        /**
         * @brief 告警仓储
         * @return AlarmRepository& 仓储
         */
        [[nodiscard]] AlarmRepository &repository() noexcept;

    private:
        std::shared_ptr<AlarmRepository> m_repository; ///< 告警仓储
        AlarmNotifier                    m_notifier;   ///< 变更通知
    };
} // namespace EdgePulse::Alarm
