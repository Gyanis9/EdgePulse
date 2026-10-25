#include "Core/Firmware/FirmwareRolloutPolicy.h"

#include "Base/Config/ConfigValue.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::Firmware
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;
        using AsynGyanis::Base::configValueAs;

        constexpr int         kMaximumWaves             = 50;
        constexpr int         kMaximumConcurrentPerWave = 1000;
        constexpr int         kMaximumSeconds           = 604800;
        constexpr std::size_t kMaximumExemptCount       = 100;
        constexpr std::size_t kMaximumMetricLength      = 64;

        [[nodiscard]] bool containsAny(const ConfigValue &object, const std::initializer_list<std::string_view> keys, std::string &unknown)
        {
            for (auto iterator = object.begin(); iterator != object.end(); ++iterator)
            {
                const std::string &key   = iterator.key();
                const bool         known = std::any_of(keys.begin(), keys.end(), [&key](const std::string_view candidate) { return candidate == key; });
                if (!known)
                {
                    unknown = key;
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool readInteger(const ConfigValue &object, const std::string_view key, const int minimum, const int maximum, const int fallback, int &value,
                                       std::string &error)
        {
            if (!object.contains(std::string{key}))
            {
                value = fallback;
                return true;
            }
            const std::optional<int> parsed = configValueAs<int>(object.at(std::string{key}));
            if (!parsed.has_value())
            {
                error = "策略字段 " + std::string{key} + " 必须是整数";
                return false;
            }
            if (*parsed < minimum || *parsed > maximum)
            {
                error = "策略字段 " + std::string{key} + " 必须在 " + std::to_string(minimum) + " 到 " + std::to_string(maximum) + " 之间";
                return false;
            }
            value = *parsed;
            return true;
        }

        [[nodiscard]] bool readBoolean(const ConfigValue &object, const std::string_view key, const bool fallback, bool &value, std::string &error)
        {
            if (!object.contains(std::string{key}))
            {
                value = fallback;
                return true;
            }
            const std::optional<bool> parsed = configValueAs<bool>(object.at(std::string{key}));
            if (!parsed.has_value())
            {
                error = "策略字段 " + std::string{key} + " 必须是布尔值";
                return false;
            }
            value = *parsed;
            return true;
        }

        [[nodiscard]] std::optional<double> ratioOf(const ConfigValue &value)
        {
            if (value.is_number_float())
            {
                return value.get<double>();
            }
            if (value.is_number_integer())
            {
                return static_cast<double>(value.get<std::int64_t>());
            }
            if (value.is_number_unsigned())
            {
                return static_cast<double>(value.get<std::uint64_t>());
            }
            return std::nullopt;
        }

        [[nodiscard]] bool readRatio(const ConfigValue &object, const std::string_view key, const double fallback, double &value, std::string &error)
        {
            if (!object.contains(std::string{key}))
            {
                value = fallback;
                return true;
            }
            const std::optional<double> parsed = ratioOf(object.at(std::string{key}));
            if (!parsed.has_value())
            {
                error = "策略字段 " + std::string{key} + " 必须是数值";
                return false;
            }
            if (!std::isfinite(*parsed) || *parsed < 0.0 || *parsed > 1.0)
            {
                error = "策略字段 " + std::string{key} + " 必须在 0 到 1 之间";
                return false;
            }
            value = *parsed;
            return true;
        }

        /**
         * @brief 解析 HH:MM 为 UTC 当日分钟
         */
        [[nodiscard]] std::optional<int> parseClock(const std::string &text)
        {
            if (text.size() != 5 || text[2] != ':')
            {
                return std::nullopt;
            }
            const auto digit = [](const char character) -> std::optional<int>
            {
                if (character < '0' || character > '9')
                {
                    return std::nullopt;
                }
                return character - '0';
            };
            const std::optional<int> hourTens   = digit(text[0]);
            const std::optional<int> hourOnes   = digit(text[1]);
            const std::optional<int> minuteTens = digit(text[3]);
            const std::optional<int> minuteOnes = digit(text[4]);
            if (!hourTens.has_value() || !hourOnes.has_value() || !minuteTens.has_value() || !minuteOnes.has_value())
            {
                return std::nullopt;
            }
            const int hour   = *hourTens * 10 + *hourOnes;
            const int minute = *minuteTens * 10 + *minuteOnes;
            if (hour > 23 || minute > 59)
            {
                return std::nullopt;
            }
            return hour * 60 + minute;
        }

        [[nodiscard]] std::tm utcTime(std::chrono::system_clock::time_point point)
        {
            const std::time_t seconds = std::chrono::system_clock::to_time_t(point);
            std::tm           value{};
#ifdef _WIN32
            gmtime_s(&value, &seconds);
#else
            gmtime_r(&seconds, &value);
#endif
            return value;
        }

        [[nodiscard]] bool parseWaves(const ConfigValue &value, std::vector<FirmwareRolloutWaveSpec> &waves, std::string &error)
        {
            if (!value.is_array())
            {
                error = "策略字段 waves 必须是数组";
                return false;
            }
            if (value.empty())
            {
                error = "策略字段 waves 不能为空";
                return false;
            }
            if (value.size() > static_cast<std::size_t>(kMaximumWaves))
            {
                error = "波次数量不能超过 " + std::to_string(kMaximumWaves);
                return false;
            }
            for (std::size_t index = 0; index < value.size(); ++index)
            {
                const ConfigValue &entry = value[index];
                const std::string  label = "第 " + std::to_string(index + 1) + " 个波次";
                if (!entry.is_object())
                {
                    error = label + "必须是对象";
                    return false;
                }
                std::string unknown;
                if (containsAny(entry, {"percent", "size"}, unknown))
                {
                    error = label + "含有未知字段 " + unknown;
                    return false;
                }
                const bool hasPercent = entry.contains("percent");
                const bool hasSize    = entry.contains("size");
                if (hasPercent == hasSize)
                {
                    error = label + "必须且只能提供 percent 或 size";
                    return false;
                }
                FirmwareRolloutWaveSpec spec;
                if (hasPercent)
                {
                    const std::optional<int> percent = configValueAs<int>(entry.at("percent"));
                    if (!percent.has_value() || *percent < 1 || *percent > 100)
                    {
                        error = label + "的 percent 必须是 1 到 100 的整数";
                        return false;
                    }
                    spec.isPercent = true;
                    spec.value     = *percent;
                } else
                {
                    const std::optional<int> size = configValueAs<int>(entry.at("size"));
                    if (!size.has_value() || *size < 1)
                    {
                        error = label + "的 size 必须是正整数";
                        return false;
                    }
                    spec.isPercent = false;
                    spec.value     = *size;
                }
                waves.push_back(spec);
            }
            return true;
        }

        [[nodiscard]] bool parseMaintenanceWindow(const ConfigValue &value, std::optional<FirmwareRolloutMaintenanceWindow> &window, std::string &error)
        {
            if (!value.is_object())
            {
                error = "策略字段 maintenanceWindow 必须是对象";
                return false;
            }
            std::string unknown;
            if (containsAny(value, {"start", "end", "days"}, unknown))
            {
                error = "maintenanceWindow 含有未知字段 " + unknown;
                return false;
            }
            const std::optional<std::string> start = value.contains("start") ? configValueAs<std::string>(value.at("start")) : std::nullopt;
            const std::optional<std::string> end   = value.contains("end") ? configValueAs<std::string>(value.at("end")) : std::nullopt;
            if (!start.has_value() || !end.has_value())
            {
                error = "maintenanceWindow 必须提供 HH:MM 形式的 start 与 end";
                return false;
            }
            const std::optional<int> startMinute = parseClock(*start);
            const std::optional<int> endMinute   = parseClock(*end);
            if (!startMinute.has_value() || !endMinute.has_value())
            {
                error = "maintenanceWindow 的 start 与 end 必须是 HH:MM";
                return false;
            }
            if (*startMinute == *endMinute)
            {
                error = "maintenanceWindow 的 start 与 end 不能相同";
                return false;
            }
            FirmwareRolloutMaintenanceWindow parsed;
            parsed.startMinute = *startMinute;
            parsed.endMinute   = *endMinute;
            if (value.contains("days"))
            {
                const ConfigValue &days = value.at("days");
                if (!days.is_array())
                {
                    error = "maintenanceWindow 的 days 必须是数组";
                    return false;
                }
                for (const ConfigValue &day: days)
                {
                    const std::optional<int> weekday = configValueAs<int>(day);
                    if (!weekday.has_value() || *weekday < 1 || *weekday > 7)
                    {
                        error = "maintenanceWindow 的 days 必须是 1 到 7 的整数";
                        return false;
                    }
                    if (std::find(parsed.days.begin(), parsed.days.end(), *weekday) == parsed.days.end())
                    {
                        parsed.days.push_back(*weekday);
                    }
                }
            }
            window = std::move(parsed);
            return true;
        }
    } // namespace

    std::expected<FirmwareRolloutPolicy, std::string> FirmwareRolloutPolicy::parse(const std::string_view json, const int heartbeatIntervalSeconds)
    {
        const std::optional<ConfigValue> root = AsynGyanis::Base::parseConfigValue(json);
        if (!root.has_value() || !root->is_object())
        {
            return std::unexpected("灰度策略必须是 JSON 对象");
        }
        std::string unknown;
        if (containsAny(*root,
                        {"waves", "maxConcurrentPerWave", "settleSeconds", "observeSeconds", "maxWaveSeconds", "minWaveSuccessRatio", "minCumulativeSuccessRatio", "maxFailures",
                         "autoRollback", "onTimeout", "requiredFreshTelemetrySeconds", "exemptAlarmMetrics", "maintenanceWindow", "requireInstalledVersion"},
                        unknown))
        {
            return std::unexpected("灰度策略含有未知字段 " + unknown);
        }

        FirmwareRolloutPolicy policy;
        std::string           error;
        if (!root->contains("waves"))
        {
            return std::unexpected("灰度策略缺少 waves");
        }
        if (!parseWaves(root->at("waves"), policy.waves, error))
        {
            return std::unexpected(error);
        }
        if (!readInteger(*root, "maxConcurrentPerWave", 0, kMaximumConcurrentPerWave, 0, policy.maxConcurrentPerWave, error) ||
            !readInteger(*root, "settleSeconds", 1, kMaximumSeconds, 60, policy.settleSeconds, error) ||
            !readInteger(*root, "observeSeconds", 0, kMaximumSeconds, 120, policy.observeSeconds, error) ||
            !readInteger(*root, "maxWaveSeconds", 1, kMaximumSeconds, 1800, policy.maxWaveSeconds, error) ||
            !readInteger(*root, "maxFailures", 0, 1000000, 0, policy.maxFailures, error) ||
            !readInteger(*root, "requiredFreshTelemetrySeconds", 1, kMaximumSeconds, 300, policy.requiredFreshTelemetrySeconds, error) ||
            !readRatio(*root, "minWaveSuccessRatio", 1.0, policy.minWaveSuccessRatio, error) ||
            !readRatio(*root, "minCumulativeSuccessRatio", 1.0, policy.minCumulativeSuccessRatio, error) ||
            !readBoolean(*root, "autoRollback", false, policy.autoRollback, error) || !readBoolean(*root, "requireInstalledVersion", false, policy.requireInstalledVersion, error))
        {
            return std::unexpected(error);
        }

        // settle 至少要覆盖两个心跳周期，否则窗口还没等到心跳就可能给出结论
        const int heartbeat = std::max(1, heartbeatIntervalSeconds);
        if (policy.settleSeconds < heartbeat * 2)
        {
            return std::unexpected("settleSeconds 不能小于两个心跳周期（" + std::to_string(heartbeat * 2) + " 秒）");
        }
        if (policy.maxWaveSeconds < std::max(policy.settleSeconds, policy.observeSeconds))
        {
            return std::unexpected("maxWaveSeconds 不能小于 settleSeconds 与 observeSeconds 中的较大值");
        }

        if (root->contains("onTimeout"))
        {
            const std::optional<std::string> value = configValueAs<std::string>(root->at("onTimeout"));
            if (!value.has_value() || (*value != "pause" && *value != "fail"))
            {
                return std::unexpected("onTimeout 必须是 pause 或 fail");
            }
            policy.onTimeout = *value;
        }

        if (root->contains("exemptAlarmMetrics"))
        {
            const ConfigValue &metrics = root->at("exemptAlarmMetrics");
            if (!metrics.is_array())
            {
                return std::unexpected("exemptAlarmMetrics 必须是数组");
            }
            if (metrics.size() > kMaximumExemptCount)
            {
                return std::unexpected("exemptAlarmMetrics 最多 " + std::to_string(kMaximumExemptCount) + " 项");
            }
            for (const ConfigValue &metric: metrics)
            {
                const std::optional<std::string> name = configValueAs<std::string>(metric);
                if (!name.has_value() || name->empty() || name->size() > kMaximumMetricLength)
                {
                    return std::unexpected("exemptAlarmMetrics 的每一项必须是非空且不超过 64 字节的字符串");
                }
                if (std::find(policy.exemptAlarmMetrics.begin(), policy.exemptAlarmMetrics.end(), *name) != policy.exemptAlarmMetrics.end())
                {
                    return std::unexpected("exemptAlarmMetrics 不能重复：" + *name);
                }
                policy.exemptAlarmMetrics.push_back(*name);
            }
        }

        if (root->contains("maintenanceWindow"))
        {
            if (!parseMaintenanceWindow(root->at("maintenanceWindow"), policy.maintenanceWindow, error))
            {
                return std::unexpected(error);
            }
        }
        return policy;
    }

    std::string FirmwareRolloutPolicy::toJson() const
    {
        ConfigValue waveArray = ConfigValue::array();
        for (const FirmwareRolloutWaveSpec &spec: this->waves)
        {
            ConfigValue entry = ConfigValue::object();
            if (spec.isPercent)
            {
                entry["percent"] = spec.value;
            } else
            {
                entry["size"] = spec.value;
            }
            waveArray.push_back(std::move(entry));
        }
        ConfigValue root = ConfigValue::object({{"waves", std::move(waveArray)},
                                                {"maxConcurrentPerWave", maxConcurrentPerWave},
                                                {"settleSeconds", settleSeconds},
                                                {"observeSeconds", observeSeconds},
                                                {"maxWaveSeconds", maxWaveSeconds},
                                                {"minWaveSuccessRatio", minWaveSuccessRatio},
                                                {"minCumulativeSuccessRatio", minCumulativeSuccessRatio},
                                                {"maxFailures", maxFailures},
                                                {"autoRollback", autoRollback},
                                                {"onTimeout", onTimeout},
                                                {"requiredFreshTelemetrySeconds", requiredFreshTelemetrySeconds},
                                                {"exemptAlarmMetrics", exemptAlarmMetrics},
                                                {"requireInstalledVersion", requireInstalledVersion}});
        if (maintenanceWindow.has_value())
        {
            const auto minutesToClock = [](const int minute) -> std::string
            {
                const int   hour = minute / 60;
                const int   rest = minute % 60;
                std::string text;
                text.push_back(static_cast<char>('0' + hour / 10));
                text.push_back(static_cast<char>('0' + hour % 10));
                text.push_back(':');
                text.push_back(static_cast<char>('0' + rest / 10));
                text.push_back(static_cast<char>('0' + rest % 10));
                return text;
            };
            ConfigValue window = ConfigValue::object({{"start", minutesToClock(maintenanceWindow->startMinute)}, {"end", minutesToClock(maintenanceWindow->endMinute)}});
            if (!maintenanceWindow->days.empty())
            {
                window["days"] = maintenanceWindow->days;
            }
            root["maintenanceWindow"] = std::move(window);
        }
        return root.dump();
    }

    bool FirmwareRolloutPolicy::allowsTime(const std::chrono::system_clock::time_point now) const
    {
        if (!maintenanceWindow.has_value())
        {
            return true;
        }
        const std::tm utc     = utcTime(now);
        const int     minute  = utc.tm_hour * 60 + utc.tm_min;
        const int     weekday = utc.tm_wday == 0 ? 7 : utc.tm_wday;
        if (!maintenanceWindow->days.empty() && std::find(maintenanceWindow->days.begin(), maintenanceWindow->days.end(), weekday) == maintenanceWindow->days.end())
        {
            return false;
        }
        if (maintenanceWindow->startMinute < maintenanceWindow->endMinute)
        {
            return minute >= maintenanceWindow->startMinute && minute < maintenanceWindow->endMinute;
        }
        return minute >= maintenanceWindow->startMinute || minute < maintenanceWindow->endMinute;
    }

    std::expected<std::vector<std::vector<std::int64_t>>, std::string> planFirmwareRolloutWaves(const FirmwareRolloutPolicy &policy, const std::vector<std::int64_t> &nodeIds)
    {
        if (policy.waves.empty())
        {
            return std::unexpected("灰度策略至少需要一个波次");
        }
        if (nodeIds.empty())
        {
            return std::unexpected("灰度发布至少需要一个目标节点");
        }
        const std::size_t                      total    = nodeIds.size();
        std::size_t                            assigned = 0;
        std::vector<std::vector<std::int64_t>> plan;
        plan.reserve(policy.waves.size());
        for (std::size_t index = 0; index < policy.waves.size(); ++index)
        {
            const FirmwareRolloutWaveSpec &spec              = policy.waves[index];
            std::size_t                    desiredCumulative = 0;
            if (spec.isPercent)
            {
                // 百分比按累计比例解释：5、25、100 表示先到 5%，再到 25%，最后铺满
                desiredCumulative = static_cast<std::size_t>(std::ceil(static_cast<double>(total) * static_cast<double>(spec.value) / 100.0));
                desiredCumulative = std::min(desiredCumulative, total);
                if (desiredCumulative < assigned)
                {
                    return std::unexpected("第 " + std::to_string(index + 1) + " 波的累计百分比小于前一波，波次百分比必须递增");
                }
            } else
            {
                if (static_cast<std::size_t>(spec.value) > total - assigned)
                {
                    return std::unexpected("第 " + std::to_string(index + 1) + " 波的固定数量超过剩余目标节点");
                }
                desiredCumulative = assigned + static_cast<std::size_t>(spec.value);
            }
            std::vector<std::int64_t> waveNodes;
            for (std::size_t position = assigned; position < desiredCumulative; ++position)
            {
                waveNodes.push_back(nodeIds[position]);
            }
            assigned = desiredCumulative;
            plan.push_back(std::move(waveNodes));
        }
        return plan;
    }
} // namespace EdgePulse::Firmware
