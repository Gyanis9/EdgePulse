#include "Core/Alarm/AlarmService.h"

#include "Base/Log/LogMacros.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Alarm
{
    namespace
    {
        [[nodiscard]] bool conditionHolds(const std::string &comparison, const double value, const double threshold)
        {
            if (comparison == ">")
            {
                return value > threshold;
            }
            if (comparison == ">=")
            {
                return value >= threshold;
            }
            if (comparison == "<")
            {
                return value < threshold;
            }
            if (comparison == "<=")
            {
                return value <= threshold;
            }
            if (comparison == "==")
            {
                return value == threshold;
            }
            return false;
        }

        [[nodiscard]] std::string messageOf(const AlarmRuleRecord &rule, const double value)
        {
            return rule.name + "：" + rule.metric + " = " + std::to_string(value) + "（阈值 " + rule.comparison + " " + std::to_string(rule.threshold) + "）";
        }
    } // namespace

    AlarmService::AlarmService(std::shared_ptr<AlarmRepository> repository, AlarmNotifier notifier) : m_repository(std::move(repository)), m_notifier(std::move(notifier))
    {
    }

    AsynGyanis::Core::Task<std::vector<AlarmRecord>> AlarmService::evaluate(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId,
                                                                            const std::vector<Telemetry::TelemetrySample> &samples)
    {
        std::map<std::string, double> latestByMetric;
        for (const Telemetry::TelemetrySample &sample: samples)
        {
            latestByMetric[sample.metric] = sample.value;
        }
        std::vector<AlarmRecord>           changed;
        const std::vector<AlarmRuleRecord> rules = co_await m_repository->enabledRules(loop);
        for (const AlarmRuleRecord &rule: rules)
        {
            if (rule.nodeId != 0 && rule.nodeId != nodeId)
            {
                continue;
            }
            const auto sample = latestByMetric.find(rule.metric);
            if (sample == latestByMetric.end())
            {
                continue;
            }
            const bool                 triggered = conditionHolds(rule.comparison, sample->second, rule.threshold);
            std::optional<AlarmRecord> open      = co_await m_repository->findOpen(loop, rule.id, nodeId);
            if (triggered && !open.has_value())
            {
                // 冷却窗口避免「恢复后立刻再报」把告警流刷成噪声
                if (co_await m_repository->recentlyResolved(loop, rule.id, nodeId, rule.cooldownSeconds))
                {
                    continue;
                }
                const auto inserted = co_await m_repository->insertAlarm(loop, rule, nodeId, sample->second, messageOf(rule, sample->second));
                if (inserted.has_value())
                {
                    changed.push_back(*inserted);
                    if (m_notifier)
                    {
                        try
                        {
                            co_await m_notifier(*inserted);
                        } catch (const std::exception &failure)
                        {
                            LOG_ERROR_FMT("告警通知广播失败：{}", failure.what());
                        }
                    }
                }
            } else if (!triggered && open.has_value())
            {
                const auto resolved = co_await m_repository->resolve(loop, open->id, 0);
                if (resolved.has_value())
                {
                    changed.push_back(*resolved);
                    if (m_notifier)
                    {
                        try
                        {
                            co_await m_notifier(*resolved);
                        } catch (const std::exception &failure)
                        {
                            LOG_ERROR_FMT("告警恢复广播失败：{}", failure.what());
                        }
                    }
                }
            }
        }
        co_return changed;
    }

    AsynGyanis::Core::Task<> AlarmService::notify(const AlarmRecord &alarm)
    {
        if (!m_notifier)
        {
            co_return;
        }
        try
        {
            co_await m_notifier(alarm);
        } catch (const std::exception &failure)
        {
            LOG_ERROR_FMT("告警通知广播失败：{}", failure.what());
        }
        co_return;
    }

    AlarmRepository &AlarmService::repository() noexcept
    {
        return *m_repository;
    }
} // namespace EdgePulse::Alarm
