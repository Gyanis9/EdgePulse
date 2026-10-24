/**
 * @file ApiTokenController.h
 * @brief API 令牌管理 HTTP 路由
 * @author Gyanis
 * @date 2026-10-25
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
    class ApiTokenService;
    class AuthService;
} // namespace EdgePulse::Auth

namespace EdgePulse::System
{
    /**
     * @brief 令牌路由依赖
     */
    struct ApiTokenDependencies
    {
        Auth::AuthService     *auth{nullptr};   ///< 认证服务
        Auth::ApiTokenService *tokens{nullptr}; ///< 令牌服务
    };

    /**
     * @brief 注册 API 令牌管理路由
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 服务集合
     * @note 租户一律取自会话，请求体不参与租户选择
     */
    void registerApiTokenRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const ApiTokenDependencies &dependencies);
} // namespace EdgePulse::System
