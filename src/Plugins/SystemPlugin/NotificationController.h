/**
 * @file NotificationController.h
 * @brief 告警通知渠道 HTTP 路由
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <Core/Coroutine/Task.h>

#include <memory>

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
namespace EdgePulse::Notification
{
    class NotificationRepository;
}
namespace EdgePulse::Common
{
    class OutboundUrlGuard;
}

namespace EdgePulse::Notification
{
    /**
     * @brief 通知路由依赖
     */
    struct NotificationDependencies
    {
        Auth::AuthService                        *auth{nullptr};     ///< 认证
        NotificationRepository                   *channels{nullptr}; ///< 渠道仓储
        System::TenantRepository                 *tenants{nullptr};  ///< 租户，用于平台管理员指定归属
        std::shared_ptr<Common::OutboundUrlGuard> guard;             ///< Webhook 出站地址安全校验器
    };

    /**
     * @brief 注册通知渠道路由
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 依赖
     */
    void registerNotificationRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const NotificationDependencies &dependencies);
} // namespace EdgePulse::Notification
