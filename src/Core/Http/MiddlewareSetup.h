/**
 * @file MiddlewareSetup.h
 * @brief 全局中间件装配
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Http/Routing/Middleware.h"

#include <memory>

namespace AsynGyanis::Core
{
    class EventLoop;
}
namespace AsynGyanis::Net
{
    class Router;
    class TokenBucket;
} // namespace AsynGyanis::Net

namespace EdgePulse::Web
{
    class IdempotencyRepository;

    /**
     * @brief 给路由器挂上统一的安全响应头与请求级异常兜底
     * @param router 目标路由器，必须在 start() 之前调用
     * @note 异常只回统一 envelope，不把 what() 原文交给调用方；内部原因写日志
     */
    void applySecurityHeaders(AsynGyanis::Net::Router &router);

    /**
     * @brief 创建放行存活与就绪探针的令牌桶限流中间件
     * @param bucket 所有公开监听器共享的令牌桶
     * @return 中间件函数；探针路径不取令牌，其余路径走共享令牌桶
     * @note 探针被 429 会让编排器把进程误判为不健康，因此 /healthz、/readyz、/api/health 与 /api/probe/ 一律放行
     */
    [[nodiscard]] AsynGyanis::Net::MiddlewareFunc rateLimitExceptProbes(std::shared_ptr<AsynGyanis::Net::TokenBucket> bucket);

    /**
     * @brief 给路由器挂上写请求幂等键处理
     * @param router 目标路由器，必须在 start() 之前调用
     * @param loop 服务器所属事件循环
     * @param repository 幂等记录仓储；空指针表示不注册中间件
     * @note 必须排在 registerAuditMiddleware() 之后，让审计仍能记录被重放的请求
     */
    void applyIdempotency(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, std::shared_ptr<IdempotencyRepository> repository);

    /**
     * @brief 给路由器挂上共享令牌桶限流
     * @param router 目标路由器，必须在 start() 之前调用
     * @param bucket 所有公开监听器共享的令牌桶；空指针表示限流关闭，不注册中间件
     * @note 同一实例必须交给全部公开路由器，否则实际上限会随监听器数量成倍放大
     * @note 存活与就绪探针路径不参与限流，见 rateLimitExceptProbes()
     */
    void applyRateLimit(AsynGyanis::Net::Router &router, const std::shared_ptr<AsynGyanis::Net::TokenBucket> &bucket);
} // namespace EdgePulse::Web
