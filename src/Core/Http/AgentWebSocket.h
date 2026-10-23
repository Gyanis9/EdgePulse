/**
 * @file AgentWebSocket.h
 * @brief Agent 长连接端点
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

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
     * @param lifecycle 节点生命周期服务；hello 把节点拉回在线时用它发布状态与销案告警，可空
     */
    void registerAgentWebSocket(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, Device::NodeRepository &nodes, Command::CommandRepository &commands,
                                Command::AgentChannelRegistry &channels, Device::NodeLifecycleService *lifecycle = nullptr);
} // namespace EdgePulse::Web
