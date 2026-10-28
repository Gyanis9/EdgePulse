/**
 * @file TenantController.h
 * @brief 租户管理 HTTP 路由
 * @author Gyanis
 * @date 2026-10-21
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
namespace EdgePulse::System
{
    class TenantRepository;

    /**
     * @brief 租户路由依赖
     */
    struct TenantDependencies
    {
        Auth::AuthService *auth{nullptr};    ///< 认证服务
        TenantRepository  *tenants{nullptr}; ///< 租户仓储
    };

    /**
     * @brief 注册租户管理路由（仅平台管理员）
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 服务集合
     */
    void registerTenantRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const TenantDependencies &dependencies);
} // namespace EdgePulse::System
