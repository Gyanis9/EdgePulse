/**
 * @file TelemetryController.h
 * @brief 遥测查询与 Agent 上报路由
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
namespace EdgePulse::Device
{
    class NodeRepository;
}
namespace EdgePulse::Telemetry
{
    class TelemetryRepository;
}

namespace EdgePulse::Telemetry
{
    /**
     * @brief 遥测路由依赖
     */
    struct TelemetryDependencies
    {
        Auth::AuthService      *auth{nullptr};      ///< 认证
        Device::NodeRepository *nodes{nullptr};     ///< 节点
        TelemetryRepository    *telemetry{nullptr}; ///< 遥测
        Alarm::AlarmService    *alarms{nullptr};    ///< 告警评估
    };

    /**
     * @brief 注册浏览器遥测查询路由
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 依赖
     */
    void registerTelemetryBrowserRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const TelemetryDependencies &dependencies);

    /**
     * @brief 注册 Agent 遥测上报路由（只挂 mTLS 监听器）
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 依赖
     */
    void registerTelemetryAgentRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const TelemetryDependencies &dependencies);
} // namespace EdgePulse::Telemetry
