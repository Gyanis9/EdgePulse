/**
 * @file DeviceController.h
 * @brief 浏览器侧节点管理路由
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}
namespace AsynGyanis::Net
{
    class HttpRequest;
    class Router;
} // namespace AsynGyanis::Net

namespace EdgePulse::Device
{
    class NodeService;

    /**
     * @brief 认证层交给节点控制器的权限快照
     */
    struct AccessContext
    {
        std::vector<std::string> permissions; ///< 当前会话的权限码
    };

    /// 从请求解析身份；未认证返回空
    using AccessResolver = std::function<std::optional<AccessContext>(const AsynGyanis::Net::HttpRequest &)>;

    /**
     * @brief 注册 /api/nodes 的读写路由
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param nodeService 节点服务
     * @param accessResolver 身份解析回调，由 App 用认证服务注入
     */
    void registerRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, NodeService &nodeService, AccessResolver accessResolver);
} // namespace EdgePulse::Device
