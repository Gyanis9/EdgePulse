/**
 * @file EnrollmentController.h
 * @brief 管理端一次性注册/改绑令牌路由
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
}
namespace EdgePulse::System
{
    class TenantRepository;
}
namespace EdgePulse::Device
{
    class EnrollmentService;
}

namespace EdgePulse::Device
{
    /**
     * @brief 注册令牌路由依赖
     */
    struct EnrollmentDependencies
    {
        Auth::AuthService        *auth{nullptr};        ///< 认证
        EnrollmentService        *enrollments{nullptr}; ///< 令牌服务
        System::TenantRepository *tenants{nullptr};     ///< 租户，用于平台管理员指定归属
    };

    /**
     * @brief 注册 /api/enrollment-tokens
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 依赖
     */
    void registerEnrollmentRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const EnrollmentDependencies &dependencies);
} // namespace EdgePulse::Device
