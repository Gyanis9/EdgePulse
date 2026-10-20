/**
 * @file AlarmEventStream.h
 * @brief 告警 SSE 端点
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
namespace EdgePulse::Web
{
    class SseBroadcaster;
}

namespace EdgePulse::Web
{
    /**
     * @brief 注册 /api/events/alarms
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param auth 认证服务
     * @param broadcaster 告警广播器
     */
    void registerAlarmEventStream(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, Auth::AuthService &auth, SseBroadcaster &broadcaster);
} // namespace EdgePulse::Web
