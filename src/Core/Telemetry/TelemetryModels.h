/**
 * @file TelemetryModels.h
 * @brief 遥测与告警共用的行模型
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>
#include <string>

namespace EdgePulse::Telemetry
{
    /**
     * @brief 一条待写入的遥测样本
     */
    struct TelemetrySample
    {
        std::string metric;      ///< 指标名
        double      value{0.0};  ///< 数值
        std::string unit;        ///< 单位
        std::string labelsJson;  ///< 标签 JSON，可空
        std::string collectedAt; ///< SQL DATETIME；空表示由服务端补当前时间
    };

    /**
     * @brief 一条遥测读数
     */
    struct TelemetryPoint
    {
        std::int64_t id{0};       ///< 主键
        std::int64_t nodeId{0};   ///< 节点
        std::string  metric;      ///< 指标
        double       value{0.0};  ///< 数值
        std::string  unit;        ///< 单位
        std::string  collectedAt; ///< 采集时间
    };
} // namespace EdgePulse::Telemetry

namespace EdgePulse::Alarm
{
    /**
     * @brief 告警规则
     */
    struct AlarmRuleRecord
    {
        std::int64_t id{0};                ///< 主键
        std::string  name;                 ///< 规则名
        std::int64_t nodeId{0};            ///< 节点；0 表示全部
        std::string  metric;               ///< 指标
        std::string  comparison;           ///< > >= < <= ==
        double       threshold{0.0};       ///< 阈值
        std::string  level{"warning"};     ///< info/warning/critical
        bool         enabled{true};        ///< 是否启用
        int          cooldownSeconds{300}; ///< 恢复后的冷却秒数
        std::string  createTime;           ///< 创建时间
    };

    /**
     * @brief 一条告警
     */
    struct AlarmRecord
    {
        std::int64_t id{0};          ///< 主键
        std::int64_t tenantId{1};    ///< 归属租户
        std::int64_t ruleId{0};      ///< 规则
        std::int64_t nodeId{0};      ///< 节点
        std::string  nodeName;       ///< 节点名
        std::string  level;          ///< 级别
        std::string  metric;         ///< 指标
        double       value{0.0};     ///< 触发值
        double       threshold{0.0}; ///< 阈值
        std::string  status{"open"}; ///< open/acknowledged/resolved
        std::string  message;        ///< 说明
        std::string  triggeredAt;    ///< 触发时间
        std::string  acknowledgedAt; ///< 确认时间
        std::string  resolvedAt;     ///< 恢复时间
    };
} // namespace EdgePulse::Alarm
