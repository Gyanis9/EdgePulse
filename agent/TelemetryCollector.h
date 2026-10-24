/**
 * @file TelemetryCollector.h
 * @brief 采集 CPU、内存与磁盘占用率，并供诊断命令取用指标
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace EdgePulse::Agent
{
    /// 单次遥测请求允许携带的样本上限，与服务端校验一致
    inline constexpr std::size_t kTelemetryBatchLimit = 500;

    /// 服务端允许的指标名长度上限
    inline constexpr std::size_t kMaximumMetricNameLength = 64;

    /**
     * @brief 一条待上报的遥测样本
     */
    struct TelemetrySample
    {
        std::string metric;      ///< 指标名，如 cpu.usage
        double      value{0.0};  ///< 指标值，有限数值
        std::string unit;        ///< 单位，占用率固定为 %
        std::string collectedAt; ///< 采集时刻，UTC YYYY-MM-DD HH:MM:SS
    };

    /**
     * @brief 一次 CPU 累计时间快照
     */
    struct CpuTimes
    {
        std::uint64_t busy{0};  ///< 非空闲累计时间
        std::uint64_t total{0}; ///< 全部累计时间
    };

    /**
     * @brief 解析一行 /proc/stat 的 cpu 汇总行
     * @param text /proc/stat 正文，只读取第一行以 cpu 开头的汇总行
     * @return 成功返回累计时间；没有汇总行或字段不足时返回中文原因
     * @note 只统计 user 到 steal 八个字段；guest 与 guest_nice 已计入 user 与 nice，不重复累加
     */
    [[nodiscard]] std::expected<CpuTimes, std::string> parseLinuxCpuStat(std::string_view text);

    /**
     * @brief 由两次 CPU 快照计算占用率
     * @param previous 上一次快照
     * @param current 本次快照
     * @return 成功返回 0 到 100 的百分比；总时间差为零或累计时间回退时返回中文原因
     */
    [[nodiscard]] std::expected<double, std::string> computeCpuPercent(const CpuTimes &previous, const CpuTimes &current);

    /**
     * @brief 解析 /proc/meminfo 并计算内存占用率
     * @param text /proc/meminfo 正文
     * @return 成功返回 0 到 100 的百分比；缺少 MemTotal 或内存可用量字段时返回中文原因
     * @note 优先使用 MemAvailable；缺失时退化为 MemFree 加 Buffers 加 Cached
     */
    [[nodiscard]] std::expected<double, std::string> parseLinuxMemInfo(std::string_view text);

    /**
     * @brief 由已用与总量计算磁盘占用率
     * @param usedBytes 已用字节数
     * @param totalBytes 总字节数
     * @return 总量为零时为空；否则返回 0 到 100 的百分比，已用量超过总量时按 100 处理
     */
    [[nodiscard]] std::optional<double> diskUsagePercent(std::uint64_t usedBytes, std::uint64_t totalBytes);

    /**
     * @brief 把一条样本序列化成队列与请求共用的 JSON 对象
     * @param sample 待序列化的样本
     * @return 成功返回紧凑 JSON 文本；指标名为空或数值不是有限数时为空
     */
    [[nodiscard]] std::optional<std::string> serializeTelemetrySample(const TelemetrySample &sample);

    /**
     * @brief 跨平台采集 CPU、内存与磁盘占用率
     * @details Linux 读取 /proc/stat、/proc/meminfo 与 statvfs；Windows 使用 GetSystemTimes、
     *          GlobalMemoryStatusEx 与 GetDiskFreeSpaceExW。任何一项系统调用失败只记 WARN 并跳过该项，
     *          不向调用方抛出。CPU 需要两次快照，构造时先取一次基准，内部互斥量保护基准的读写，
     *          因此遥测循环与诊断工作线程可以并发调用。
     */
    class TelemetryCollector
    {
    public:
        /**
         * @brief 记录磁盘路径并取一次 CPU 基准快照
         * @param diskPath 磁盘遥测的挂载点或卷路径
         * @note Windows 上路径为 / 时改用当前盘符根目录并记 WARN
         */
        explicit TelemetryCollector(std::filesystem::path diskPath);

        /**
         * @brief 采集一轮指标
         * @return 采集成功的样本，共用同一个 collectedAt；单项失败时缺省该项
         * @note 阻塞式系统读取，调用方应放在工作线程上执行
         */
        [[nodiscard]] std::vector<TelemetrySample> collect();

        /**
         * @brief 采集一轮指标并序列化成诊断用的 JSON 数组
         * @return 形如 [{"metric":"cpu.usage","value":12.5,"unit":"%"}] 的文本；序列化失败时为空数组
         * @note 可在任意线程调用；返回值不含 collectedAt
         */
        [[nodiscard]] std::string metricsJson();

    private:
        std::filesystem::path   m_diskPath;    ///< 磁盘遥测的挂载点或卷路径
        std::mutex              m_cpuMutex;    ///< 保护上一次 CPU 快照
        std::optional<CpuTimes> m_previousCpu; ///< 上一次 CPU 快照；首轮读取失败时为空
    };
} // namespace EdgePulse::Agent
