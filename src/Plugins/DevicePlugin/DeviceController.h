/**
 * @file DeviceController.h
 * @brief 浏览器侧节点管理路由
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <Core/Coroutine/Task.h>

#include <cstdint>
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

namespace EdgePulse::Auth
{
    class DataScopeService;
}

namespace EdgePulse::System
{
    class TenantRepository;
}

namespace EdgePulse::Device
{
    class NodeLifecycleService;
    class NodeService;

    /**
     * @brief 认证层交给节点控制器的权限快照
     */
    struct AccessContext
    {
        std::int64_t             userId{0};            ///< 用户标识
        std::int64_t             tenantId{1};          ///< 租户
        bool                     platformAdmin{false}; ///< 平台管理员
        std::string              username;             ///< 登录名
        std::int64_t             departmentId{0};      ///< 部门标识
        std::vector<std::string> roles;                ///< 角色码
        std::vector<std::string> permissions;          ///< 当前会话的权限码
    };

    /// 从请求异步解析身份；未认证返回空
    using AccessResolver = std::function<AsynGyanis::Core::Task<std::optional<AccessContext>>(AsynGyanis::Core::EventLoop &, AsynGyanis::Net::HttpRequest &)>;

    /**
     * @brief 注册 /api/nodes 的读写路由
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param nodeService 节点服务
     * @param accessResolver 身份解析回调，由 App 用认证服务注入
     * @param dataScopes 数据范围解析；空表示不限制
     * @param tenants 租户仓储，平台管理员按 tenantId 指定节点归属
     * @param lifecycle 节点生命周期服务，删除节点后用它销案系统离线告警；可空
     */
    void registerRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, NodeService &nodeService, AccessResolver accessResolver,
                        Auth::DataScopeService *dataScopes = nullptr, System::TenantRepository *tenants = nullptr, NodeLifecycleService *lifecycle = nullptr);
} // namespace EdgePulse::Device
