/**
 * @file TimeFormat.h
 * @brief UTC 时间文本格式化
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <chrono>
#include <string>

namespace EdgePulse::Common
{
    /**
     * @brief 把时刻格式化成 RFC 3339 的 UTC 文本
     * @param point 待格式化的时刻
     * @return std::string 形如 2026-10-20T12:34:56Z
     */
    [[nodiscard]] std::string formatUtc(std::chrono::system_clock::time_point point);

    /**
     * @brief 取当前时刻的 RFC 3339 UTC 文本
     * @return std::string 形如 2026-10-20T12:34:56Z
     */
    [[nodiscard]] std::string nowIso8601Utc();
} // namespace EdgePulse::Common
