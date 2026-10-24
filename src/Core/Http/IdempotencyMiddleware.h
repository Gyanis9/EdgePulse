/**
 * @file IdempotencyMiddleware.h
 * @brief 写请求的幂等键中间件
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Http/Routing/Middleware.h"

#include <memory>
#include <string>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Auth
{
    class AuthService;
}

namespace EdgePulse::Web
{
    class IdempotencyRepository;

    /**
     * @brief 为携带 Idempotency-Key 的写请求做占位、重放与冲突判定
     * @param loop 服务器所属事件循环，仓储读写与鉴权完成后在这里恢复
     * @param repository 幂等记录仓储；调用方必须保证其活到路由器销毁之后
     * @param auth 认证服务，必须先于占位与重放验明令牌；必须活到路由器销毁之后
     * @param excludedPrefix 反向代理前缀，非空时该前缀下的请求整段放行
     * @return AsynGyanis::Net::MiddlewareFunc 可直接挂到路由器上的中间件
     * @note 只处理 POST/PUT/PATCH/DELETE；认证、Agent、实时通道、固件流式上传、一次性令牌与
     *       含口令的用户/租户管理接口一律放行
     * @note 令牌无效时不建占位也不重放，直接交给处理器给出 401
     */
    [[nodiscard]] AsynGyanis::Net::MiddlewareFunc idempotencyMiddleware(AsynGyanis::Core::EventLoop &loop, std::shared_ptr<IdempotencyRepository> repository,
                                                                        Auth::AuthService &auth, std::string excludedPrefix = {});
} // namespace EdgePulse::Web
