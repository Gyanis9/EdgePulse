/**
 * @file AlarmController.h
 * @brief 告警与规则 HTTP 路由
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
namespace EdgePulse::Alarm
{
    class AlarmService;
}

namespace EdgePulse::Alarm
{
    /**
     * @brief 告警路由依赖
     */
    struct AlarmDependencies
    {
        Auth::AuthService *auth{nullptr};   ///< 认证
        AlarmService      *alarms{nullptr}; ///< 告警服务
    };

    /**
     * @brief 注册 /api/alarms 与 /api/alarm-rules
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 依赖
     */
    void registerAlarmRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const AlarmDependencies &dependencies);
} // namespace EdgePulse::Alarm
