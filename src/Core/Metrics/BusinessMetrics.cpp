#include "Core/Metrics/BusinessMetrics.h"

#include <cstdint>
#include <utility>

namespace EdgePulse::Metrics
{
    namespace
    {
        using AsynGyanis::Core::ProcessMetricKind;
        using AsynGyanis::Core::ProcessMetricMerge;
        using AsynGyanis::Core::ProcessMetricsRegistry;

        [[nodiscard]] std::uint64_t clampCount(const std::int64_t value) noexcept
        {
            return value > 0 ? static_cast<std::uint64_t>(value) : 0;
        }
    } // namespace

    BusinessMetrics &BusinessMetrics::instance()
    {
        static BusinessMetrics metrics;
        return metrics;
    }

    void BusinessMetrics::registerMetrics()
    {
        std::call_once(m_registration,
                       [this]
                       {
                           const auto gauge = [](std::string name, std::string help, const std::atomic<std::uint64_t> &value)
                           {
                               return ProcessMetricsRegistry::registerMetric(std::move(name), std::move(help), ProcessMetricKind::Gauge, ProcessMetricMerge::Sum,
                                                                             [&value] { return value.load(std::memory_order_relaxed); });
                           };
                           const auto counter = [](std::string name, std::string help, const std::atomic<std::uint64_t> &value)
                           {
                               return ProcessMetricsRegistry::registerMetric(std::move(name), std::move(help), ProcessMetricKind::Counter, ProcessMetricMerge::Sum,
                                                                             [&value] { return value.load(std::memory_order_relaxed); });
                           };

                           m_handles.reserve(9);
                           m_handles.push_back(gauge("edgepulse_nodes_online", "当前在线节点数", m_online));
                           m_handles.push_back(gauge("edgepulse_nodes_offline", "当前离线节点数", m_offline));
                           m_handles.push_back(gauge("edgepulse_alarms_open", "当前未恢复告警数", m_alarmsOpen));
                           m_handles.push_back(gauge("edgepulse_commands_pending", "尚未完成的命令数", m_commandsPending));
                           m_handles.push_back(gauge("edgepulse_agent_certs_expiring_7d", "七天内到期的 Agent 证书数", m_certificatesExpiring));
                           m_handles.push_back(gauge("edgepulse_firmware_unsigned", "未签名的固件数", m_unsignedFirmware));
                           m_handles.push_back(counter("edgepulse_node_offline_transitions_total", "节点由在线转为离线的累计次数", m_offlineTransitions));
                           m_handles.push_back(counter("edgepulse_agent_identity_rejections_total", "Agent 身份校验拒绝的累计次数", m_identityRejections));
                           m_handles.push_back(counter("edgepulse_webhook_ssrf_rejections_total", "Webhook 地址安全拒绝的累计次数", m_webhookSsrfRejections));
                       });
    }

    void BusinessMetrics::setNodeCounts(const std::int64_t online, const std::int64_t offline)
    {
        m_online.store(clampCount(online), std::memory_order_relaxed);
        m_offline.store(clampCount(offline), std::memory_order_relaxed);
    }

    void BusinessMetrics::setAlarmsOpen(const std::int64_t open)
    {
        m_alarmsOpen.store(clampCount(open), std::memory_order_relaxed);
    }

    void BusinessMetrics::setCommandsPending(const std::int64_t pending)
    {
        m_commandsPending.store(clampCount(pending), std::memory_order_relaxed);
    }

    void BusinessMetrics::setAgentCertificatesExpiring(const std::int64_t count)
    {
        m_certificatesExpiring.store(clampCount(count), std::memory_order_relaxed);
    }

    void BusinessMetrics::setUnsignedFirmware(const std::int64_t count)
    {
        m_unsignedFirmware.store(clampCount(count), std::memory_order_relaxed);
    }

    void BusinessMetrics::incrementOfflineTransitions()
    {
        m_offlineTransitions.fetch_add(1, std::memory_order_relaxed);
    }

    void BusinessMetrics::incrementIdentityRejections()
    {
        m_identityRejections.fetch_add(1, std::memory_order_relaxed);
    }

    void BusinessMetrics::incrementWebhookSsrfRejections()
    {
        m_webhookSsrfRejections.fetch_add(1, std::memory_order_relaxed);
    }
} // namespace EdgePulse::Metrics
