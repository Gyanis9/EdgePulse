/**
 * @file AgentController.h
 * @brief Agent mTLS 路由
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

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

namespace EdgePulse::Device
{
    class AgentCertificateAuthority;
    class AgentRegistry;
    class NodeConfigRepository;
    class NodeRepository;
} // namespace EdgePulse::Device

namespace EdgePulse::System
{
    class TenantRepository;
}

namespace EdgePulse::Device
{
    /**
     * @brief 注册 Agent 的注册、证书续期与心跳路由
     * @param router 目标路由器；调用方必须只在 mTLS 监听器上挂载
     * @param loop 服务器所属事件循环
     * @param registry Agent 注册表
     * @param nodes 节点仓储，用于校验心跳与续期请求的证书身份
     * @param tenants 租户仓储，用于解析注册请求的租户编码并检查续期时的租户状态
     * @param heartbeatIntervalSeconds 期望的心跳间隔，随心跳响应下发
     * @param nodeConfig 配置影子仓储，心跳响应据此带回期望配置版本；可空
     * @param certificateAuthority 内部 CA；空或未启用时续期返回 503
     * @param beforeCertificateRotation 每次写入轮换前的测试钩子，可空
     */
    void registerRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, AgentRegistry &registry, NodeRepository &nodes, System::TenantRepository &tenants,
                        int heartbeatIntervalSeconds, NodeConfigRepository *nodeConfig = nullptr, AgentCertificateAuthority *certificateAuthority = nullptr,
                        std::function<void(std::int64_t)> beforeCertificateRotation = {});
} // namespace EdgePulse::Device
