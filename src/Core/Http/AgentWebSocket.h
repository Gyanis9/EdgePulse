/**
 * @file AgentWebSocket.h
 * @brief Agent 长连接端点
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <Core/Coroutine/Task.h>

#include <chrono>
#include <cstdint>
#include <functional>

namespace AsynGyanis::Core
{
    class EventLoop;
}
namespace AsynGyanis::Net
{
    class Router;
}
namespace EdgePulse::Command
{
    class AgentChannelRegistry;
    class CommandRepository;
} // namespace EdgePulse::Command
namespace EdgePulse::Device
{
    class NodeLifecycleService;
    class NodeRepository;
} // namespace EdgePulse::Device

namespace EdgePulse::Web
{
    /**
     * @brief 注册 /api/agent/ws
     * @param router mTLS 监听器上的路由器
     * @param loop 服务器所属事件循环
     * @param nodes 节点仓储
     * @param commands 命令仓储
     * @param channels 在线通道表
     * @param lifecycle 节点生命周期服务；每次 hello 的心跳都用它发布状态与销案告警，可空
     * @param beforeHeartbeat hello 的身份与证书绑定检查通过后、心跳落库前调用的钩子，参数为节点标识；生产传空
     * @param revocationCheckInterval 建立通道后复核节点吊销/删除状态的周期；非正值按 30 秒处理
     * @note 钩子在 hello 协程内执行，因此运行于服务器事件循环线程
     * @note 周期复核与读超时共用同一个时限：Agent 空闲时 receiveFor() 到点就查库；持续有消息时
     *       至少每个周期也查一次。复核发现节点已删除或 cert_revoked_at 非空时摘表并以 1008 关闭，
     *       因此跨实例关闭消息丢失不会再让被吊销的连接一直保留
     */
    void registerAgentWebSocket(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, Device::NodeRepository &nodes, Command::CommandRepository &commands,
                                Command::AgentChannelRegistry &channels, Device::NodeLifecycleService *lifecycle = nullptr,
                                std::function<AsynGyanis::Core::Task<>(std::int64_t)> beforeHeartbeat         = {},
                                std::chrono::milliseconds                             revocationCheckInterval = std::chrono::seconds{30});
} // namespace EdgePulse::Web
