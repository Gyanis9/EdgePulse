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
        if (becameOnline)
        {
            LOG_INFO_FMT("节点恢复在线：node={} name={}", node.id, node.name);
            co_await publish(node);
        }
        // 每次心跳都尝试销案：扫描可能在这次心跳之后插入了过期离线告警
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
            try
            {
                // 批次与心跳并发：发布前重读，节点已恢复在线时不能再用旧快照覆盖它
                const std::optional<NodeRecord> current = co_await m_nodes.findById(loop, node.id);
                if (!current.has_value() || current->status != "offline")
                {
                    continue;
                }
                LOG_INFO_FMT("节点离线：node={} name={}，超过 {} 秒未收到心跳", current->id, current->name, m_heartbeatTimeoutSeconds);
                Metrics::BusinessMetrics::instance().incrementOfflineTransitions();
                co_await publish(*current);
                co_await m_alarms.reportNodeOffline(loop, *current, m_heartbeatTimeoutSeconds);
            } catch (const std::exception &failure)
            {
                // 单个节点失败不能拖住批次里的其它节点
                LOG_ERROR_FMT("节点离线处理失败：node={} reason={}", node.id, failure.what());
            }
        }

        // 上一轮若在转离线与写告警之间中断，节点会停在“离线且没有系统告警”的状态；按条件补一次
        const std::vector<NodeRecord> missing = co_await m_nodes.listOfflineWithoutSystemAlarm(loop, 200);
        for (const NodeRecord &node: missing)
        {
            try
            {
                const std::optional<NodeRecord> current = co_await m_nodes.findById(loop, node.id);
                if (!current.has_value() || current->status != "offline")
                {
                    continue;
                }
                LOG_INFO_FMT("补离线状态与系统告警：node={} name={}", current->id, current->name);
                // 上一轮可能停在发布之前，这里补发一次状态；发布幂等，不影响实时界面
                co_await publish(*current);
                co_await m_alarms.reportNodeOffline(loop, *current, m_heartbeatTimeoutSeconds);
            } catch (const std::exception &failure)
            {
                LOG_ERROR_FMT("补系统离线告警失败：node={} reason={}", node.id, failure.what());
            }
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
