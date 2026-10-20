/**
 * @file TimeFormat.h
 * @brief UTC 时间文本格式化
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
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

    /**
     * @brief 把时刻格式化成 SQL DATETIME 可接受的 UTC 文本
     * @param point 待格式化的时刻
     * @return std::string 形如 2026-10-20 12:34:56
     * @note MySQL 的 DATETIME 不接受 RFC 3339 的 T 与 Z，写库统一用这一档
     */
    [[nodiscard]] std::string formatDateTimeUtc(std::chrono::system_clock::time_point point);

    /**
     * @brief 取当前时刻的 SQL DATETIME UTC 文本
     * @return std::string 形如 2026-10-20 12:34:56
     */
    [[nodiscard]] std::string nowDateTimeUtc();
} // namespace EdgePulse::Common
