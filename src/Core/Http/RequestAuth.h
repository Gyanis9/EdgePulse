/**
 * @file RequestAuth.h
 * @brief 控制器共用的会话与权限校验
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/SessionStore.h"
#include "Core/Http/Authorization.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace EdgePulse::Web
{
    /**
     * @brief 校验会话与权限；失败时已写好响应
     * @param auth 认证服务
     * @param loop 恢复协程用的事件循环
     * @param request 当前请求
     * @param response 当前响应
     * @param permission 需要的权限码；空表示只要求登录
     * @param traceId 请求标识
     * @return std::optional<Auth::SessionRecord> 通过时返回身份；失败为空
     */
    [[nodiscard]] inline AsynGyanis::Core::Task<std::optional<Auth::SessionRecord>> requireSession(Auth::AuthService &auth, AsynGyanis::Core::EventLoop &loop,
                                                                                                   AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response,
                                                                                                   const std::string_view permission, const std::string_view traceId)
    {
        const std::optional<std::string>   token = bearerToken(request);
        std::optional<Auth::SessionRecord> session;
        if (token.has_value())
        {
            session = co_await auth.authenticate(loop, *token);
        }
        // 未认证与无权限是两件事：401 让前端回登录页，403 让前端提示无权访问
        if (!session.has_value())
        {
            Common::writeFailure(response, Common::kCodeUnauthorized, Common::kCodeUnauthorized, "未登录或会话已过期", traceId);
            co_return std::nullopt;
        }
        if (!permission.empty() && std::find(session->permissions.begin(), session->permissions.end(), permission) == session->permissions.end())
        {
            Common::writeFailure(response, Common::kCodeForbidden, Common::kCodeForbidden, "无权执行该操作", traceId);
            co_return std::nullopt;
        }
        co_return session;
    }

    /**
     * @brief 判断身份是否拥有权限码
     * @param session 身份
     * @param permission 权限码
     * @return bool 是否拥有
     */
    [[nodiscard]] inline bool hasPermission(const Auth::SessionRecord &session, const std::string_view permission)
    {
        return std::find(session.permissions.begin(), session.permissions.end(), permission) != session.permissions.end();
    }
} // namespace EdgePulse::Web
