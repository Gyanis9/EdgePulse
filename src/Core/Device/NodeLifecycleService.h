/**
 * @file NodeLifecycleService.h
 * @brief 节点在线转换、离线扫描与系统离线告警
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Device/NodeRepository.h"

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <functional>
#include <string>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Alarm
{
    class AlarmService;
}

namespace EdgePulse::Device
{
    /**
     * @brief 节点生命周期：在线转换时发布状态、离线时生成告警
     */
    class NodeLifecycleService
    {
    public:
        /// 节点状态发布回调；在服务器循环线程上 co_await
        using StatusPublisher = std::function<AsynGyanis::Core::Task<>(const NodeRecord &)>;

        /**
         * @brief 构造
         * @param nodes 节点仓储
         * @param alarms 告警服务
         * @param publisher 状态发布回调，可空
         * @param heartbeatTimeoutSeconds 判定离线的心跳超时秒数
         */
        NodeLifecycleService(NodeRepository &nodes, Alarm::AlarmService &alarms, StatusPublisher publisher, int heartbeatTimeoutSeconds);

        /**
         * @brief 心跳落下后处理在线转换：发布状态并销案离线告警
         * @param loop 恢复协程用的事件循环
         * @param node 心跳后的节点
         * @param becameOnline 本次心跳是否把节点从离线拉回在线
         */
        AsynGyanis::Core::Task<> onHeartbeat(AsynGyanis::Core::EventLoop &loop, const NodeRecord &node, bool becameOnline);

        /**
         * @brief 节点删除后清掉它的系统离线告警
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点标识
         */
        AsynGyanis::Core::Task<> onNodeRemoved(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId);

        /**
         * @brief 证书吊销后发布节点状态并清掉它的系统离线告警
         * @param loop 恢复协程用的事件循环
         * @param node 被吊销证书的节点
         */
        AsynGyanis::Core::Task<> onCertificateRevoked(AsynGyanis::Core::EventLoop &loop, const NodeRecord &node);

        /**
         * @brief 把超时未心跳的节点转离线：逐个发布状态并生成系统告警
         * @param loop 恢复协程用的事件循环
         * @param cutoff 最近心跳早于该时刻即视为超时
         */
        AsynGyanis::Core::Task<> sweep(AsynGyanis::Core::EventLoop &loop, std::string cutoff);

    private:
        /**
         * @brief 发布一次节点状态；推送失败只记日志，不推翻已经落库的状态
         */
        AsynGyanis::Core::Task<> publish(const NodeRecord &node);

        NodeRepository      &m_nodes;                   ///< 节点仓储
        Alarm::AlarmService &m_alarms;                  ///< 告警服务
        StatusPublisher      m_publisher;               ///< 状态发布回调
        int                  m_heartbeatTimeoutSeconds; ///< 心跳超时秒数
    };
} // namespace EdgePulse::Device
