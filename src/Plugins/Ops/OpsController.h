/**
 * @file OpsController.h
 * @brief 运维总览 HTTP 路由
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>

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
namespace EdgePulse::Ops
{
    class OpsRepository;
}

namespace EdgePulse::Ops
{
    /**
     * @brief 运维路由依赖
     */
    struct OpsDependencies
    {
        Auth::AuthService *auth{nullptr};       ///< 认证
        OpsRepository     *repository{nullptr}; ///< 只读仓储
        std::uint16_t      httpPort{0};         ///< HTTP 端口
        std::uint16_t      httpsPort{0};        ///< HTTPS/h3 端口
        std::uint16_t      agentPort{0};        ///< Agent mTLS 端口
        std::uint16_t      opsPort{0};          ///< 运维端口
    };

    /**
     * @brief 注册 /api/ops/summary 与 /api/ops/protocols
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 依赖
     */
    void registerOpsRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const OpsDependencies &dependencies);
} // namespace EdgePulse::Ops
