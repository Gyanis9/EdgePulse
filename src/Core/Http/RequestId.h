/**
 * @file RequestId.h
 * @brief 请求标识的解析与补齐
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <string>

namespace AsynGyanis::Net
{
    class HttpRequest;
}

namespace EdgePulse::Web
{
    /**
     * @brief 取本次请求的 trace id：引擎已落定时沿用，缺失时现场生成并写回请求
     * @param request 当前请求
     * @return std::string 非空的请求标识
     */
    [[nodiscard]] std::string resolveRequestId(AsynGyanis::Net::HttpRequest &request);
} // namespace EdgePulse::Web
