/**
 * @file RequestParsing.h
 * @brief 路径参数与查询参数的小工具
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Config/ConfigValue.h"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace EdgePulse::Common
{
    /**
     * @brief 解析正整数标识
     * @param text 文本
     * @return std::optional<std::int64_t> 标识；空、非数字或非正数时为空
     */
    [[nodiscard]] inline std::optional<std::int64_t> parsePositiveIdentifier(const std::optional<std::string> &text)
    {
        if (!text.has_value() || text->empty())
        {
            return std::nullopt;
        }
        std::int64_t value      = 0;
        const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
        if (error != std::errc{} || end != text->data() + text->size() || value <= 0)
        {
            return std::nullopt;
        }
        return value;
    }

    /**
     * @brief 取请求体里的整数字段
     * @param body 请求体
     * @param key 字段名
     * @return std::optional<std::int64_t> 整数
     */
    [[nodiscard]] inline std::optional<std::int64_t> integerField(const AsynGyanis::Base::ConfigValue &body, const std::string_view key)
    {
        if (!body.is_object())
        {
            return std::nullopt;
        }
        const auto iterator = body.find(std::string{key});
        if (iterator == body.end() || !iterator->is_number_integer())
        {
            return std::nullopt;
        }
        return iterator->get<std::int64_t>();
    }

    /**
     * @brief 取请求体里的布尔字段
     * @param body 请求体
     * @param key 字段名
     * @return std::optional<bool> 布尔
     */
    [[nodiscard]] inline std::optional<bool> booleanField(const AsynGyanis::Base::ConfigValue &body, const std::string_view key)
    {
        if (!body.is_object())
        {
            return std::nullopt;
        }
        const auto iterator = body.find(std::string{key});
        if (iterator == body.end() || !iterator->is_boolean())
        {
            return std::nullopt;
        }
        return iterator->get<bool>();
    }
} // namespace EdgePulse::Common
