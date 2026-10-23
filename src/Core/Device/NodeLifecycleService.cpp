#include "Core/Device/NodeLifecycleService.h"

#include "Base/Log/LogMacros.h"
#include "Core/Alarm/AlarmService.h"
#include "Core/Metrics/BusinessMetrics.h"

#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Device
{
    NodeLifecycleService::NodeLifecycleService(NodeRepository &nodes, Alarm::AlarmService &alarms, StatusPublisher publisher, const int heartbeatTimeoutSeconds) :
        m_nodes(nodes), m_alarms(alarms), m_publisher(std::move(publisher)), m_heartbeatTimeoutSeconds(heartbeatTimeoutSeconds)
    {
    }

    AsynGyanis::Core::Task<> NodeLifecycleService::onHeartbeat(AsynGyanis::Core::EventLoop &loop, const NodeRecord &node, const bool becameOnline)
    {
        if (!becameOnline)
        {
            co_return;
        }
        LOG_INFO_FMT("节点恢复在线：node={} name={}", node.id, node.name);
        co_await publish(node);
        co_await m_alarms.resolveNodeOffline(loop, node.id);
        co_return;
    }

    AsynGyanis::Core::Task<> NodeLifecycleService::onNodeRemoved(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        co_await m_alarms.resolveNodeOffline(loop, nodeId);
        co_return;
    }

    AsynGyanis::Core::Task<> NodeLifecycleService::onCertificateRevoked(AsynGyanis::Core::EventLoop &loop, const NodeRecord &node)
    {
        co_await publish(node);
        co_await m_alarms.resolveNodeOffline(loop, node.id);
        co_return;
    }

    AsynGyanis::Core::Task<> NodeLifecycleService::sweep(AsynGyanis::Core::EventLoop &loop, std::string cutoff)
    {
        const std::vector<NodeRecord> changed = co_await m_nodes.markStaleOffline(loop, std::move(cutoff), 200);
        for (const NodeRecord &node: changed)
        {
            LOG_INFO_FMT("节点离线：node={} name={}，超过 {} 秒未收到心跳", node.id, node.name, m_heartbeatTimeoutSeconds);
            Metrics::BusinessMetrics::instance().incrementOfflineTransitions();
            co_await publish(node);
            co_await m_alarms.reportNodeOffline(loop, node, m_heartbeatTimeoutSeconds);
        }
        co_return;
    }

    AsynGyanis::Core::Task<> NodeLifecycleService::publish(const NodeRecord &node)
    {
        if (!m_publisher)
        {
            co_return;
        }
        try
        {
            co_await m_publisher(node);
        } catch (const std::exception &failure)
        {
            // 状态已经落库；推送失败只影响实时界面，不能因此漏掉后续的告警
            LOG_ERROR_FMT("节点状态发布失败：{}", failure.what());
        }
        co_return;
    }
} // namespace EdgePulse::Device
