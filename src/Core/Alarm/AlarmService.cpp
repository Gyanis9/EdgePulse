#include "Core/Alarm/AlarmService.h"

#include "Base/Log/LogMacros.h"
#include "Core/Device/NodeRepository.h"

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

    std::shared_ptr<AlarmService> AlarmService::scoped(const System::TenantScope tenant) const
    {
        return std::make_shared<AlarmService>(m_repository->scoped(tenant), m_notifier);
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

    AsynGyanis::Core::Task<std::optional<AlarmRecord>> AlarmService::reportNodeOffline(AsynGyanis::Core::EventLoop &loop, const Device::NodeRecord &node, const int timeoutSeconds)
    {
        // 先抓住 scoped 视图：协程恢复期间它必须一直活着
        const std::shared_ptr<AlarmRepository> scopedRepository = m_repository->scoped({node.tenantId, false});
        const auto                             current          = co_await scopedRepository->findOpen(loop, 0, node.id);
        if (current.has_value())
        {
            // 同一个离线过程只保留一条：扫描重入或并发心跳都不能把告警刷成多条
            co_return *current;
        }

        AlarmRuleRecord systemRule;
        systemRule.id        = 0;
        systemRule.level     = "critical";
        systemRule.metric    = "node.online";
        systemRule.threshold = 1;

        const std::string message  = "节点离线：超过 " + std::to_string(timeoutSeconds) + " 秒未收到心跳";
        auto              inserted = co_await scopedRepository->insertAlarm(loop, systemRule, node.id, 0, message);
        if (inserted.has_value())
        {
            LOG_INFO_FMT("系统离线告警已生成：alarm={} node={} name={}", inserted->id, node.id, node.name);
            co_await notify(*inserted);
        }
        co_return inserted;
    }

    AsynGyanis::Core::Task<std::optional<AlarmRecord>> AlarmService::resolveNodeOffline(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        // 节点标识全局唯一，而恢复路径拿不到节点租户；按全租户视图找那条系统告警
        const std::shared_ptr<AlarmRepository> scopedRepository = m_repository->scoped({0, true});
        const auto                             open             = co_await scopedRepository->findOpen(loop, 0, nodeId);
        if (!open.has_value())
        {
            co_return std::nullopt;
        }
        const auto resolved = co_await scopedRepository->resolve(loop, open->id, 0);
        if (resolved.has_value())
        {
            LOG_INFO_FMT("系统离线告警已销案：alarm={} node={}", resolved->id, nodeId);
            co_await notify(*resolved);
        }
        co_return resolved;
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
