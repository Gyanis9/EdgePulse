/**
 * @file IdempotencyMiddleware.h
 * @brief 写请求的幂等键中间件
 * @author Gyanis
 * @date 2026-10-25
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

namespace EdgePulse::Web
{
    class IdempotencyRepository;

    /**
     * @brief 为携带 Idempotency-Key 的写请求做占位、重放与冲突判定
     * @param loop 服务器所属事件循环，仓储读写完成后在这里恢复
     * @param repository 幂等记录仓储；调用方必须保证其活到路由器销毁之后
     * @return AsynGyanis::Net::MiddlewareFunc 可直接挂到路由器上的中间件
     * @note 只处理 POST/PUT/PATCH/DELETE；认证、Agent、实时通道、固件流式上传与一次性令牌接口一律放行
     */
    [[nodiscard]] AsynGyanis::Net::MiddlewareFunc idempotencyMiddleware(AsynGyanis::Core::EventLoop &loop, std::shared_ptr<IdempotencyRepository> repository);
} // namespace EdgePulse::Web
