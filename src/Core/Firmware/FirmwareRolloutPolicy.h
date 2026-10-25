/**
 * @file FirmwareRolloutPolicy.h
 * @brief 固件灰度发布策略的解析、校验与波次物化
 * @author Gyanis
 * @date 2026-10-26
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace EdgePulse::Firmware
{
    /**
     * @brief 一个波次的规模描述
     */
    struct FirmwareRolloutWaveSpec
    {
        bool isPercent{true}; ///< true 表示按目标总数百分比，false 表示固定数量
        int  value{0};        ///< 百分比 1..100，或固定数量且不小于 1
    };

    /**
     * @brief 维护窗口
     * @details 时间按 UTC 当日分钟计；起点大于终点表示窗口跨过午夜。
     */
    struct FirmwareRolloutMaintenanceWindow
    {
        int              startMinute{0}; ///< 窗口起点，UTC 当日分钟 0..1439
        int              endMinute{0};   ///< 窗口终点，UTC 当日分钟 0..1439
        std::vector<int> days;           ///< ISO 星期 1 到 7；空表示每天
    };

    /**
     * @brief 灰度发布策略
     */
    struct FirmwareRolloutPolicy
    {
        std::vector<FirmwareRolloutWaveSpec>            waves;                              ///< 波次计划，不能为空
        int                                             maxConcurrentPerWave{0};            ///< 每波同时下发的上限，0 表示不限
        int                                             settleSeconds{60};                  ///< 派发后等待心跳与遥测的最短秒数
        int                                             observeSeconds{120};                ///< 观察窗口秒数
        int                                             maxWaveSeconds{1800};               ///< 单波最长秒数
        double                                          minWaveSuccessRatio{1.0};           ///< 单波最低成功率 0..1
        double                                          minCumulativeSuccessRatio{1.0};     ///< 累计最低成功率 0..1
        int                                             maxFailures{0};                     ///< 允许的失败目标数，0 表示不设上限
        bool                                            autoRollback{false};                ///< 失败或超时后是否自动回滚
        std::string                                     onTimeout{"pause"};                 ///< 超时动作：pause 或 fail
        int                                             requiredFreshTelemetrySeconds{300}; ///< 遥测最大可接受年龄秒数
        std::vector<std::string>                        exemptAlarmMetrics;                 ///< 不计入门控的告警指标
        std::optional<FirmwareRolloutMaintenanceWindow> maintenanceWindow;                  ///< 可选的维护窗口
        bool                                            requireInstalledVersion{true};      ///< 是否要求节点上报已安装版本；已上报的版本始终要与目标一致

        /**
         * @brief 解析并校验策略 JSON
         * @param json 策略正文
         * @param heartbeatIntervalSeconds 当前部署的 Agent 心跳间隔，settleSeconds 不得小于它的两倍
         * @return std::expected<FirmwareRolloutPolicy, std::string> 策略；不合法时为中文原因
         * @note 未知字段一律拒绝，避免拼写错误的配置被静默忽略
         */
        [[nodiscard]] static std::expected<FirmwareRolloutPolicy, std::string> parse(std::string_view json, int heartbeatIntervalSeconds = 30);

        /**
         * @brief 输出规范化后的策略 JSON
         * @return std::string 含全部字段的紧凑 JSON
         */
        [[nodiscard]] std::string toJson() const;

        /**
         * @brief 判断时刻是否落在维护窗口内
         * @param now 待判定时刻
         * @return bool 没有维护窗口或落在窗口内时为 true
         * @note 窗口按 UTC 计算，只用于派发新波次，不阻断观察与回滚
         */
        [[nodiscard]] bool allowsTime(std::chrono::system_clock::time_point now) const;
    };

    /**
     * @brief 把波次计划物化为每波的目标节点
     * @param policy 已校验的策略
     * @param nodeIds 目标节点，顺序决定波次归属
     * @return std::expected<std::vector<std::vector<std::int64_t>>, std::string> 每波节点；固定数量超出剩余目标时为中文原因
     */
    [[nodiscard]] std::expected<std::vector<std::vector<std::int64_t>>, std::string> planFirmwareRolloutWaves(const FirmwareRolloutPolicy     &policy,
                                                                                                              const std::vector<std::int64_t> &nodeIds);
} // namespace EdgePulse::Firmware
