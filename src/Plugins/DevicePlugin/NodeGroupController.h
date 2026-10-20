/**
 * @file NodeGroupController.h
 * @brief 节点分组 HTTP 路由
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
namespace EdgePulse::Auth
{
    class AuthService;
}
namespace EdgePulse::Device
{
    class NodeGroupRepository;
}

namespace EdgePulse::Device
{
    /**
     * @brief 节点分组路由依赖
     */
    struct NodeGroupDependencies
    {
        Auth::AuthService   *auth{nullptr};   ///< 认证
        NodeGroupRepository *groups{nullptr}; ///< 分组仓储
    };

    /**
     * @brief 注册 /api/node-groups
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 依赖
     */
    void registerNodeGroupRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const NodeGroupDependencies &dependencies);
} // namespace EdgePulse::Device
