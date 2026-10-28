/**
 * @file NodeConfigController.h
 * @brief 设备影子与管理端/Agent 配置路由
 * @author Gyanis
 * @date 2026-10-24
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
    class DataScopeService;
} // namespace EdgePulse::Auth

namespace EdgePulse::Device
{
    class NodeConfigRepository;
    class NodeConfigService;
    class NodeRepository;

    /**
     * @brief 配置路由依赖
     */
    struct NodeConfigDependencies
    {
        Auth::AuthService      *auth{nullptr};       ///< 浏览器会话认证
        NodeConfigRepository   *configs{nullptr};    ///< 配置仓储
        NodeConfigService      *service{nullptr};    ///< 配置业务服务
        NodeRepository         *nodes{nullptr};      ///< 节点仓储，用于校验节点归属与 Agent 身份
        Auth::DataScopeService *dataScopes{nullptr}; ///< 数据范围解析；空表示不限制
    };

    /**
     * @brief 注册管理端配置路由
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 依赖
     */
    void registerNodeConfigRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const NodeConfigDependencies &dependencies);

    /**
     * @brief 注册 Agent 配置拉取与上报路由
     * @param router 目标路由器；调用方必须只在 mTLS 监听器上挂载
     * @param loop 服务器所属事件循环
     * @param dependencies 依赖
     * @note 两条路由都先按客户端证书绑定节点，再以节点自身租户读写配置
     */
    void registerNodeConfigAgentRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const NodeConfigDependencies &dependencies);
} // namespace EdgePulse::Device
