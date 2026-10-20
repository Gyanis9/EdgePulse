/**
 * @file ApiResponse.h
 * @brief 统一 JSON 响应包络的写出入口
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <string_view>

#include "Base/Config/ConfigValue.h"

namespace AsynGyanis::Net
{
    class HttpResponse;
}

namespace EdgePulse::Common
{
    inline constexpr std::string_view kRequestIdHeader = "x-request-id";

    /**
     * @brief 写一条成功响应
     * @param response 目标响应
     * @param data 载荷，允许为对象、数组或 null
     * @param traceId 请求标识；非空时回写到 x-request-id
     */
    void writeSuccess(AsynGyanis::Net::HttpResponse &response, AsynGyanis::Base::ConfigValue data, std::string_view traceId);

    /**
     * @brief 写一条失败响应
     * @param response 目标响应
     * @param httpStatus HTTP 状态码
     * @param businessCode 业务码，通常与 httpStatus 同值
     * @param message 面向调用方的说明，不得包含内部异常原文
     * @param traceId 请求标识；非空时回写到 x-request-id
     */
    void writeFailure(AsynGyanis::Net::HttpResponse &response, int httpStatus, int businessCode, std::string_view message, std::string_view traceId);
} // namespace EdgePulse::Common
