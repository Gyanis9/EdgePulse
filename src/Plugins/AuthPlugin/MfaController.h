/**
 * @file MfaController.h
 * @brief 两步验证设置、启用、关闭、挑战校验与恢复码路由
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
    class AuthService;
    class MfaService;

    /**
     * @brief 注册 /api/auth/mfa 下的全部路由
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param authService 认证服务，用于挑战校验与会话确认
     * @param mfaService 两步验证服务
     * @note 设置、启用与恢复码响应含一次性敏感材料，统一带 Cache-Control: no-store
     */
    void registerMfaRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, AuthService &authService, MfaService &mfaService);
} // namespace EdgePulse::Auth
