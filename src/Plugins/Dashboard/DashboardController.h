/**
 * @file DashboardController.h
 * @brief 工作台汇总 HTTP 路由
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
    class DataScopeService;
} // namespace EdgePulse::Auth
namespace EdgePulse::System
{
    class LogRepository;
    class RoleRepository;
} // namespace EdgePulse::System

namespace EdgePulse::Dashboard
{
    /**
     * @brief 工作台依赖
     */
    struct DashboardDependencies
    {
        Auth::AuthService      *auth{nullptr};       ///< 认证服务
        Auth::DataScopeService *dataScopes{nullptr}; ///< 数据范围
        System::LogRepository  *logs{nullptr};       ///< 日志与统计
        System::RoleRepository *roles{nullptr};      ///< 角色分布
    };

    /**
     * @brief 注册 /api/dashboard/summary
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 服务集合
     */
    void registerDashboardRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const DashboardDependencies &dependencies);
} // namespace EdgePulse::Dashboard
