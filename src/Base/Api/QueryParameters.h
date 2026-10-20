/**
 * @file QueryParameters.h
 * @brief 查询串取值的统一入口
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace EdgePulse::Common
{
    using QueryMap = std::unordered_map<std::string, std::string>;

    /**
     * @brief 取文本参数
     * @param query 查询参数表
     * @param key 参数名
     * @return std::string 值；缺失时为空串
     */
    [[nodiscard]] inline std::string queryText(const QueryMap &query, const std::string_view key)
    {
        const auto iterator = query.find(std::string{key});
        return iterator == query.end() ? std::string{} : iterator->second;
    }

    /**
     * @brief 取整数参数
     * @param query 查询参数表
     * @param key 参数名
     * @return std::optional<std::int64_t> 合法整数；缺失或不是整数时为空
     */
    [[nodiscard]] inline std::optional<std::int64_t> queryInteger(const QueryMap &query, const std::string_view key)
    {
        const auto iterator = query.find(std::string{key});
        if (iterator == query.end() || iterator->second.empty())
        {
            return std::nullopt;
        }
        std::int64_t value      = 0;
        const auto [end, error] = std::from_chars(iterator->second.data(), iterator->second.data() + iterator->second.size(), value);
        if (error != std::errc{} || end != iterator->second.data() + iterator->second.size())
        {
            return std::nullopt;
        }
        return value;
    }

    /**
     * @brief 取布尔参数（true/1/false/0）
     * @param query 查询参数表
     * @param key 参数名
     * @param fallback 缺失或无法识别时的取值
     * @return bool 取值
     */
    [[nodiscard]] inline bool queryBoolean(const QueryMap &query, const std::string_view key, const bool fallback = false)
    {
        const std::string value = queryText(query, key);
        if (value == "true" || value == "1")
        {
            return true;
        }
        if (value == "false" || value == "0")
        {
            return false;
        }
        return fallback;
    }
} // namespace EdgePulse::Common
