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

namespace EdgePulse::System
{
    class LogRepository;
}

namespace EdgePulse::Auth
{
    class AuthService;
    class MfaService;

    /**
     * @brief 注册 /api/auth/mfa 下的全部路由
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param authService 认证服务，用于挑战校验、口令复核与会话确认
     * @param mfaService 两步验证服务
     * @param logs 操作日志仓储，记录设置、启用、关闭与换发恢复码
     * @note 设置、启用与恢复码响应含一次性敏感材料，统一带 Cache-Control: no-store
     * @note 四类管理动作各自写一条操作审计，只记用户、IP 与动作，不落验证码、口令或秘密
     */
    void registerMfaRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, AuthService &authService, MfaService &mfaService, System::LogRepository &logs);
} // namespace EdgePulse::Auth
