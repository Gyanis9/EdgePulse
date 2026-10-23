/**
 * @file AgentController.h
 * @brief Agent mTLS 路由
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

namespace EdgePulse::Device
{
    class AgentRegistry;
    class NodeRepository;
} // namespace EdgePulse::Device

namespace EdgePulse::System
{
    class TenantRepository;
}

namespace EdgePulse::Device
{
    /**
     * @brief 注册 Agent 的注册与心跳路由
     * @param router 目标路由器；调用方必须只在 mTLS 监听器上挂载
     * @param loop 服务器所属事件循环
     * @param registry Agent 注册表
     * @param nodes 节点仓储，用于校验心跳请求的证书身份
     * @param tenants 租户仓储，用于解析注册请求的租户编码
     * @param heartbeatIntervalSeconds 期望的心跳间隔，随心跳响应下发
     */
    void registerRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, AgentRegistry &registry, NodeRepository &nodes, System::TenantRepository &tenants,
                        int heartbeatIntervalSeconds);
} // namespace EdgePulse::Device
