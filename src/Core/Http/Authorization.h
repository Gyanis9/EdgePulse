/**
 * @file Authorization.h
 * @brief Authorization 头的 Bearer 令牌解析
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <optional>
#include <string>

namespace AsynGyanis::Net
{
    class HttpRequest;
}

namespace EdgePulse::Web
{
    /**
     * @brief 从 Authorization 头取 Bearer 令牌
     * @param request 当前请求
     * @return 令牌文本；头缺失、前缀不匹配或为空时为空
     * @note 前缀大小写不敏感，令牌本体原样保留
     */
    [[nodiscard]] std::optional<std::string> bearerToken(const AsynGyanis::Net::HttpRequest &request);
} // namespace EdgePulse::Web
