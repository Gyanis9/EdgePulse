/**
 * @file TelemetryIngest.h
 * @brief 遥测样本落库与告警评估的共用入口
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Telemetry/TelemetryModels.h"

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Device
{
    class NodeRepository;
}

namespace EdgePulse::Alarm
{
    class AlarmService;
} // namespace EdgePulse::Alarm

namespace EdgePulse::Telemetry
{
    class TelemetryRepository;

    /**
     * @brief 一次遥测写入的结果
     */
    struct TelemetryIngestOutcome
    {
        bool                            nodeExists{false}; ///< 节点是否存在且未删除
        bool                            stored{false};     ///< 样本是否落库
        std::vector<Alarm::AlarmRecord> alarms;            ///< 本次触发的告警
    };

    /**
     * @brief 校验节点、写入样本并评估告警
     * @param loop 恢复协程用的事件循环
     * @param nodes 节点仓储
     * @param telemetry 遥测仓储
     * @param alarms 告警服务
     * @param nodeId 节点
     * @param samples 已解析的样本
     * @return TelemetryIngestOutcome 结果
     */
    AsynGyanis::Core::Task<TelemetryIngestOutcome> ingestTelemetry(AsynGyanis::Core::EventLoop &loop, Device::NodeRepository &nodes, TelemetryRepository &telemetry,
                                                                   Alarm::AlarmService &alarms, std::int64_t nodeId, std::vector<TelemetrySample> samples);
} // namespace EdgePulse::Telemetry
