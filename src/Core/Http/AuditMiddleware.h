/**
 * @file AuditMiddleware.h
 * @brief 写操作审计中间件
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

namespace EdgePulse::Web
{
    /**
     * @brief 把写操作审计挂到路由器上
     * @param router 目标路由器，必须尚未开始服务
     * @param loop 服务器所属事件循环
     * @param auth 认证服务，用于记下操作人
     * @param logs 日志仓储
     * @note 认证、验证码与登出路径不记审计；请求体里的口令与令牌字段在落库前被剔除
     */
    void registerAuditMiddleware(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, Auth::AuthService &auth, System::LogRepository &logs);
} // namespace EdgePulse::Web
