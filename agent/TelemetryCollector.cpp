#include "TelemetryCollector.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Base/Runtime/TimeFormat.h"
#include "Platform/FileSystem/FileSystem.h"
#include "Platform/Platform.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <expected>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if !ASYN_PLATFORM_WIN32
#include <sys/statvfs.h>
#endif

namespace EdgePulse::Agent
{
    namespace
    {
        constexpr std::size_t kCpuFieldCount = 8; ///< /proc/stat 汇总行参与统计的字段数

        /**
         * @brief 按空白切分一行文本
         * @param line 原始行
         * @return std::vector<std::string_view> 非空字段视图，指向原行
         */
        [[nodiscard]] std::vector<std::string_view> splitFields(const std::string_view line)
        {
            std::vector<std::string_view> fields;
            std::size_t                   index = 0;
            while (index < line.size())
            {
                while (index < line.size() && (line[index] == ' ' || line[index] == '\t' || line[index] == '\r'))
                {
                    ++index;
                }
                const std::size_t begin = index;
                while (index < line.size() && line[index] != ' ' && line[index] != '\t' && line[index] != '\r')
                {
                    ++index;
                }
                if (index > begin)
                {
                    fields.push_back(line.substr(begin, index - begin));
                }
            }
            return fields;
        }

        /**
         * @brief 把十进制文本解析成无符号整数
         * @param text 待解析文本
         * @param value 成功时写入数值
         * @return true 表示整段文本都是非负十进制整数
         */
        [[nodiscard]] bool parseUnsigned(const std::string_view text, std::uint64_t &value)
        {
            if (text.empty())
            {
                return false;
            }
            const char *begin  = text.data();
            const char *end    = text.data() + text.size();
            const auto  parsed = std::from_chars(begin, end, value);
            return parsed.ec == std::errc{} && parsed.ptr == end;
        }

        /**
         * @brief 读取一行里的第一个无符号整数
         * @param text 待扫描文本
         * @return 找到时返回数值；没有可解析的数字时为空
         */
        [[nodiscard]] std::optional<std::uint64_t> firstUnsigned(const std::string_view text)
        {
            const std::vector<std::string_view> fields = splitFields(text);
            if (fields.empty())
            {
                return std::nullopt;
            }
            std::uint64_t value = 0;
            if (!parseUnsigned(fields.front(), value))
            {
                return std::nullopt;
            }
            return value;
        }

        /**
         * @brief 由已用量与总量计算百分比并对边界收口
         * @param used 已用量
         * @param total 总量
         * @return 总量为零时为空；否则返回 0 到 100 的百分比
         */
        [[nodiscard]] std::optional<double> usagePercent(const std::uint64_t used, const std::uint64_t total)
        {
            if (total == 0)
            {
                return std::nullopt;
            }
            const std::uint64_t boundedUsed = std::min(used, total);
            return 100.0 * static_cast<double>(boundedUsed) / static_cast<double>(total);
        }

#if ASYN_PLATFORM_WIN32
        /**
         * @brief 把 FILETIME 转成 100 纳秒计数
         * @param time Win32 时间结构
         * @return std::uint64_t 累计计数
         */
        [[nodiscard]] std::uint64_t fileTimeTicks(const FILETIME &time)
        {
            ULARGE_INTEGER value{};
            value.LowPart  = time.dwLowDateTime;
            value.HighPart = time.dwHighDateTime;
            return value.QuadPart;
        }

        /**
         * @brief 读取一次 Windows 累计 CPU 时间
         * @return 成功返回快照；系统调用失败时返回中文原因
         */
        [[nodiscard]] std::expected<CpuTimes, std::string> readCpuTimes()
        {
            FILETIME idleTime{};
            FILETIME kernelTime{};
            FILETIME userTime{};
            if (GetSystemTimes(&idleTime, &kernelTime, &userTime) == 0)
            {
                return std::unexpected("GetSystemTimes 调用失败（错误码 " + std::to_string(GetLastError()) + "）");
            }
            const std::uint64_t idle   = fileTimeTicks(idleTime);
            const std::uint64_t kernel = fileTimeTicks(kernelTime);
            const std::uint64_t user   = fileTimeTicks(userTime);
            // kernel 已包含 idle，空闲时间不能重复计入 total
            const std::uint64_t total = kernel + user;
            return CpuTimes{total >= idle ? total - idle : 0, total};
        }

        /**
         * @brief 读取一次 Windows 内存占用率
         * @return 成功返回百分比；系统调用失败时返回中文原因
         */
        [[nodiscard]] std::expected<double, std::string> readMemoryPercent()
        {
            MEMORYSTATUSEX status{};
            status.dwLength = sizeof(status);
            if (GlobalMemoryStatusEx(&status) == 0)
            {
                return std::unexpected("GlobalMemoryStatusEx 调用失败（错误码 " + std::to_string(GetLastError()) + "）");
            }
            const auto percent = usagePercent(status.ullTotalPhys - std::min(status.ullTotalPhys, status.ullAvailPhys), status.ullTotalPhys);
            if (!percent.has_value())
            {
                return std::unexpected("物理内存总量为 0，无法计算占用率");
            }
            return *percent;
        }
#else
        /**
         * @brief 读取一次 Linux 累计 CPU 时间
         * @return 成功返回快照；文件不可读或格式不符时返回中文原因
         */
        [[nodiscard]] std::expected<CpuTimes, std::string> readCpuTimes()
        {
            std::ifstream stream("/proc/stat");
            if (!stream.is_open())
            {
                return std::unexpected("无法打开 /proc/stat");
            }
            std::string line;
            if (!std::getline(stream, line))
            {
                return std::unexpected("读取 /proc/stat 失败：文件为空");
            }
            return parseLinuxCpuStat(line);
        }

        /**
         * @brief 读取一次 Linux 内存占用率
         * @return 成功返回百分比；文件不可读或格式不符时返回中文原因
         */
        [[nodiscard]] std::expected<double, std::string> readMemoryPercent()
        {
            std::ifstream stream("/proc/meminfo");
            if (!stream.is_open())
            {
                return std::unexpected("无法打开 /proc/meminfo");
            }
            const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
            return parseLinuxMemInfo(text);
        }
#endif

        /**
         * @brief 读取磁盘占用率
         * @param path 磁盘挂载点或卷路径
         * @return 成功返回百分比；路径不可用或总量为零时返回中文原因
         */
        [[nodiscard]] std::expected<double, std::string> readDiskPercent(const std::filesystem::path &path)
        {
#if ASYN_PLATFORM_WIN32
            ULARGE_INTEGER availableBytes{};
            ULARGE_INTEGER totalBytes{};
            ULARGE_INTEGER freeBytes{};
            if (GetDiskFreeSpaceExW(path.c_str(), &availableBytes, &totalBytes, &freeBytes) == 0)
            {
                return std::unexpected("GetDiskFreeSpaceExW 调用失败（错误码 " + std::to_string(GetLastError()) + "）：" + AsynGyanis::Platform::FileSystem::utf8FromPath(path));
            }
            const auto percent = usagePercent(totalBytes.QuadPart - std::min(totalBytes.QuadPart, freeBytes.QuadPart), totalBytes.QuadPart);
            if (!percent.has_value())
            {
                return std::unexpected("磁盘总空间为 0，无法计算占用率：" + AsynGyanis::Platform::FileSystem::utf8FromPath(path));
            }
            return *percent;
#else
            struct statvfs info{};
            if (statvfs(path.c_str(), &info) != 0)
            {
                return std::unexpected("statvfs 调用失败：" + std::error_code(errno, std::generic_category()).message() + "：" +
                                       AsynGyanis::Platform::FileSystem::utf8FromPath(path));
            }
            const std::uint64_t fragment = info.f_frsize != 0 ? static_cast<std::uint64_t>(info.f_frsize) : static_cast<std::uint64_t>(info.f_bsize);
            if (fragment == 0)
            {
                return std::unexpected("statvfs 返回的块大小为零，无法计算磁盘占用率：" + AsynGyanis::Platform::FileSystem::utf8FromPath(path));
            }
            const auto blocks = static_cast<std::uint64_t>(info.f_blocks);
            const auto free   = static_cast<std::uint64_t>(info.f_bfree);
            if (blocks > std::numeric_limits<std::uint64_t>::max() / fragment || free > std::numeric_limits<std::uint64_t>::max() / fragment)
            {
                return std::unexpected("statvfs 返回的块数超出可计算范围：" + AsynGyanis::Platform::FileSystem::utf8FromPath(path));
            }
            const std::uint64_t total   = blocks * fragment;
            const std::uint64_t used    = total - std::min(total, free * fragment);
            const auto          percent = usagePercent(used, total);
            if (!percent.has_value())
            {
                return std::unexpected("磁盘总空间为 0，无法计算占用率：" + AsynGyanis::Platform::FileSystem::utf8FromPath(path));
            }
            return *percent;
#endif
        }
    } // namespace

    std::expected<CpuTimes, std::string> parseLinuxCpuStat(const std::string_view text)
    {
        std::size_t position = 0;
        while (position <= text.size())
        {
            const std::size_t      lineEnd = text.find('\n', position);
            const std::string_view line    = text.substr(position, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - position);
            const auto             fields  = splitFields(line);
            if (!fields.empty() && fields.front() == "cpu")
            {
                if (fields.size() < 5)
                {
                    return std::unexpected("解析 /proc/stat 失败：cpu 汇总行字段不足（至少需要 user、nice、system、idle）");
                }
                const std::size_t fieldCount = std::min(fields.size() - 1, kCpuFieldCount);
                std::uint64_t     values[kCpuFieldCount]{};
                for (std::size_t index = 0; index < fieldCount; ++index)
                {
                    const std::string_view token = fields[index + 1];
                    if (!parseUnsigned(token, values[index]))
                    {
                        return std::unexpected("解析 /proc/stat 失败：cpu 汇总行第 " + std::to_string(index + 1) + " 个字段不是非负整数");
                    }
                }
                std::uint64_t total = 0;
                for (std::size_t index = 0; index < fieldCount; ++index)
                {
                    total += values[index];
                }
                const std::uint64_t idle      = values[3];
                const std::uint64_t iowait    = fieldCount > 4 ? values[4] : 0;
                const std::uint64_t idleTotal = idle + iowait <= total ? idle + iowait : total;
                return CpuTimes{total - idleTotal, total};
            }
            if (lineEnd == std::string_view::npos)
            {
                break;
            }
            position = lineEnd + 1;
        }
        return std::unexpected("解析 /proc/stat 失败：没有找到以 cpu 开头的汇总行");
    }

    std::expected<double, std::string> computeCpuPercent(const CpuTimes &previous, const CpuTimes &current)
    {
        if (current.total < previous.total)
        {
            return std::unexpected("CPU 累计时间在上次采样后回退，无法计算占用率");
        }
        const std::uint64_t totalDelta = current.total - previous.total;
        if (totalDelta == 0)
        {
            return std::unexpected("CPU 总时间差为 0，无法计算占用率");
        }
        const std::uint64_t busyDelta = current.busy > previous.busy ? current.busy - previous.busy : 0;
        return std::clamp(100.0 * static_cast<double>(busyDelta) / static_cast<double>(totalDelta), 0.0, 100.0);
    }

    std::expected<double, std::string> parseLinuxMemInfo(const std::string_view text)
    {
        std::map<std::string, std::uint64_t> values;
        std::size_t                          position = 0;
        while (position <= text.size())
        {
            const std::size_t      lineEnd = text.find('\n', position);
            const std::string_view line    = text.substr(position, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - position);
            const std::size_t      colon   = line.find(':');
            if (colon != std::string_view::npos)
            {
                const auto key    = splitFields(line.substr(0, colon));
                const auto number = firstUnsigned(line.substr(colon + 1));
                if (!key.empty() && number.has_value())
                {
                    values[std::string{key.front()}] = *number;
                }
            }
            if (lineEnd == std::string_view::npos)
            {
                break;
            }
            position = lineEnd + 1;
        }

        const auto total = values.find("MemTotal");
        if (total == values.end() || total->second == 0)
        {
            return std::unexpected("解析 /proc/meminfo 失败：缺少有效的 MemTotal");
        }
        std::optional<std::uint64_t> available;
        if (const auto direct = values.find("MemAvailable"); direct != values.end())
        {
            available = direct->second;
        } else
        {
            const auto free    = values.find("MemFree");
            const auto buffers = values.find("Buffers");
            const auto cached  = values.find("Cached");
            if (free == values.end() || buffers == values.end() || cached == values.end())
            {
                return std::unexpected("解析 /proc/meminfo 失败：MemAvailable 缺失，且 MemFree、Buffers 或 Cached 不完整");
            }
            available = free->second + buffers->second + cached->second;
        }
        const std::uint64_t boundedAvailable = std::min(*available, total->second);
        return 100.0 * static_cast<double>(total->second - boundedAvailable) / static_cast<double>(total->second);
    }

    std::optional<double> diskUsagePercent(const std::uint64_t usedBytes, const std::uint64_t totalBytes)
    {
        return usagePercent(usedBytes, totalBytes);
    }

    std::optional<std::string> serializeTelemetrySample(const TelemetrySample &sample)
    {
        if (sample.metric.empty() || sample.metric.size() > kMaximumMetricNameLength || !std::isfinite(sample.value))
        {
            return std::nullopt;
        }
        const AsynGyanis::Base::ConfigValue object = AsynGyanis::Base::ConfigValue::object({
                {"metric", sample.metric},
                {"value", sample.value},
                {"unit", sample.unit},
                {"collectedAt", sample.collectedAt},
        });
        return AsynGyanis::Base::serializeConfigValue(object);
    }

    TelemetryCollector::TelemetryCollector(std::filesystem::path diskPath) : m_diskPath(std::move(diskPath))
    {
#if ASYN_PLATFORM_WIN32
        if (m_diskPath.generic_string() == "/")
        {
            std::error_code error;
            const auto      root = std::filesystem::current_path(error).root_path();
            if (!error && !root.empty())
            {
                LOG_WARN_FMT("Windows 上 disk_path 为 /，已改用当前盘符根目录 {}", AsynGyanis::Platform::FileSystem::utf8FromPath(root));
                m_diskPath = root;
            } else
            {
                LOG_WARN_FMT("Windows 上 disk_path 为 /，且无法定位当前盘符根目录：{}", error.message());
            }
        }
#endif
        const auto baseline = readCpuTimes();
        if (baseline.has_value())
        {
            m_previousCpu = *baseline;
        } else
        {
            LOG_WARN_FMT("初始 CPU 快照失败，首轮将跳过 cpu.usage：{}", baseline.error());
        }
    }

    std::vector<TelemetrySample> TelemetryCollector::collect()
    {
        std::vector<TelemetrySample> samples;
        const std::string            collectedAt = Common::nowDateTimeUtc();

        {
            // 采样与计算都在锁内完成：诊断线程与遥测循环并发调用时，不会把较旧的快照写回基准
            std::lock_guard lock(m_cpuMutex);
            const auto      currentCpu = readCpuTimes();
            if (!currentCpu.has_value())
            {
                LOG_WARN_FMT("CPU 采集失败，本轮跳过 cpu.usage：{}", currentCpu.error());
            } else if (!m_previousCpu.has_value())
            {
                m_previousCpu = *currentCpu;
                LOG_WARN("缺少上一次 CPU 快照，本轮跳过 cpu.usage");
            } else
            {
                const auto percent = computeCpuPercent(*m_previousCpu, *currentCpu);
                m_previousCpu      = *currentCpu;
                if (percent.has_value())
                {
                    samples.push_back(TelemetrySample{"cpu.usage", *percent, "%", collectedAt});
                } else
                {
                    LOG_WARN_FMT("CPU 占用率计算失败，本轮跳过 cpu.usage：{}", percent.error());
                }
            }
        }

        const auto memoryPercent = readMemoryPercent();
        if (memoryPercent.has_value())
        {
            samples.push_back(TelemetrySample{"memory.usage", *memoryPercent, "%", collectedAt});
        } else
        {
            LOG_WARN_FMT("内存采集失败，本轮跳过 memory.usage：{}", memoryPercent.error());
        }

        const auto diskPercent = readDiskPercent(m_diskPath);
        if (diskPercent.has_value())
        {
            samples.push_back(TelemetrySample{"disk.usage", *diskPercent, "%", collectedAt});
        } else
        {
            LOG_WARN_FMT("磁盘采集失败，本轮跳过 disk.usage：{}", diskPercent.error());
        }
        return samples;
    }

    std::string TelemetryCollector::metricsJson()
    {
        const std::vector<TelemetrySample> samples = collect();
        AsynGyanis::Base::ConfigValue      metrics = AsynGyanis::Base::ConfigValue::array();
        for (const TelemetrySample &sample: samples)
        {
            metrics.push_back(AsynGyanis::Base::ConfigValue::object({
                    {"metric", sample.metric},
                    {"value", sample.value},
                    {"unit", sample.unit},
            }));
        }
        const auto serialized = AsynGyanis::Base::serializeConfigValue(metrics);
        if (!serialized.has_value())
        {
            LOG_ERROR("诊断指标无法序列化为 JSON，metrics 返回空数组");
            return "[]";
        }
        return *serialized;
    }
} // namespace EdgePulse::Agent
