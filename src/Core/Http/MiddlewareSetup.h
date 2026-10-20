/**
 * @file MiddlewareSetup.h
 * @brief 全局中间件装配
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

namespace AsynGyanis::Net
{
    class Router;
}

namespace EdgePulse::Web
{
    /**
     * @brief 给路由器挂上统一的安全响应头与请求级异常兜底
     * @param router 目标路由器，必须在 start() 之前调用
     * @note 异常只回统一 envelope，不把 what() 原文交给调用方；内部原因写日志
     */
    void applySecurityHeaders(AsynGyanis::Net::Router &router);
} // namespace EdgePulse::Web
