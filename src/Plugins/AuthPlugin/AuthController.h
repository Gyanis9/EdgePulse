/**
 * @file AuthController.h
 * @brief 认证相关 HTTP 路由
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
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
    class CaptchaService;

    /**
     * @brief 注册验证码、登录、登出、me、routes 与改密路由
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param authService 认证服务
     * @param captchaService 验证码服务
     * @param exposeCaptchaCode 开发档是否在响应里带验证码答案
     */
    void registerRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, AuthService &authService, CaptchaService &captchaService, bool exposeCaptchaCode);
} // namespace EdgePulse::Auth
