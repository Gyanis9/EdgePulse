/**
 * @file MiddlewareSetup.h
 * @brief 全局中间件装配
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <memory>

namespace AsynGyanis::Net
{
    class Router;
    class TokenBucket;
} // namespace AsynGyanis::Net

namespace EdgePulse::Web
{
    /**
     * @brief 给路由器挂上统一的安全响应头与请求级异常兜底
     * @param router 目标路由器，必须在 start() 之前调用
     * @note 异常只回统一 envelope，不把 what() 原文交给调用方；内部原因写日志
     */
    void applySecurityHeaders(AsynGyanis::Net::Router &router);

    /**
     * @brief 给路由器挂上共享令牌桶限流
     * @param router 目标路由器，必须在 start() 之前调用
     * @param bucket 所有公开监听器共享的令牌桶；空指针表示限流关闭，不注册中间件
     * @note 同一实例必须交给全部公开路由器，否则实际上限会随监听器数量成倍放大
     */
    void applyRateLimit(AsynGyanis::Net::Router &router, const std::shared_ptr<AsynGyanis::Net::TokenBucket> &bucket);
} // namespace EdgePulse::Web
