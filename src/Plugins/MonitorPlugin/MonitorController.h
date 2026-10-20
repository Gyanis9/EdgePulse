/**
 * @file MonitorController.h
 * @brief 监控日志与统计 HTTP 路由
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
namespace EdgePulse::System
{
    class LogRepository;
}

namespace EdgePulse::Monitor
{
    /**
     * @brief 监控路由依赖
     */
    struct MonitorDependencies
    {
        Auth::AuthService     *auth{nullptr}; ///< 认证服务
        System::LogRepository *logs{nullptr}; ///< 日志仓储
    };

    /**
     * @brief 注册 /api/monitor 下的日志路由
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 服务集合
     */
    void registerMonitorRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const MonitorDependencies &dependencies);
} // namespace EdgePulse::Monitor
