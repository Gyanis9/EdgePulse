/**
 * @file SqlHelpers.h
 * @brief 结果集列读取的共用小工具
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace EdgePulse::Sql
{
    /**
     * @brief 读一列文本；NULL 或非文本时交回空串
     * @param result 当前结果集
     * @param column 列名
     * @return std::string 文本
     */
    [[nodiscard]] inline std::string text(const AsynGyanis::Database::DatabaseResult &result, const std::string_view column)
    {
        const AsynGyanis::Database::DatabaseValue value = result.getValue(column);
        if (const auto *textValue = std::get_if<std::string>(&value); textValue != nullptr)
        {
            return *textValue;
        }
        return {};
    }

    /**
     * @brief 读一列可选文本；NULL 时为空
     * @param result 当前结果集
     * @param column 列名
     * @return std::optional<std::string> 文本；NULL 或类型不符时为空
     */
    [[nodiscard]] inline std::optional<std::string> optionalText(const AsynGyanis::Database::DatabaseResult &result, const std::string_view column)
    {
        const AsynGyanis::Database::DatabaseValue value = result.getValue(column);
        if (const auto *textValue = std::get_if<std::string>(&value); textValue != nullptr)
        {
            return *textValue;
        }
        return std::nullopt;
    }

    /**
     * @brief 读一列整数；NULL 或非整数时交回 0
     * @param result 当前结果集
     * @param column 列名
     * @return std::int64_t 整数
     */
    [[nodiscard]] inline std::int64_t integer(const AsynGyanis::Database::DatabaseResult &result, const std::string_view column)
    {
        const AsynGyanis::Database::DatabaseValue value = result.getValue(column);
        if (const auto *integerValue = std::get_if<std::int64_t>(&value); integerValue != nullptr)
        {
            return *integerValue;
        }
        return 0;
    }

    /**
     * @brief 读一列布尔；NULL 或非整数时按 false
     * @param result 当前结果集
     * @param column 列名
     * @return bool 布尔值
     */
    [[nodiscard]] inline bool boolean(const AsynGyanis::Database::DatabaseResult &result, const std::string_view column)
    {
        return integer(result, column) != 0;
    }

    /**
     * @brief 读一列日期时间并统一成 DATETIME 文本
     * @param result 当前结果集
     * @param column 列名
     * @return std::string 形如 2026-10-20 12:34:56；NULL 或无法识别时为空
     */
    [[nodiscard]] inline std::string dateTimeText(const AsynGyanis::Database::DatabaseResult &result, const std::string_view column)
    {
        const AsynGyanis::Database::DatabaseValue value = result.getValue(column);
        if (const auto *text = std::get_if<std::string>(&value); text != nullptr)
        {
            return *text;
        }
        if (const auto *local = std::get_if<AsynGyanis::Database::LocalDateTime>(&value); local != nullptr)
        {
            return std::format("{:%Y-%m-%d %H:%M:%S}", *local);
        }
        if (const auto *instant = std::get_if<AsynGyanis::Database::Timestamp>(&value); instant != nullptr)
        {
            return std::format("{:%Y-%m-%d %H:%M:%S}", *instant);
        }
        return {};
    }
} // namespace EdgePulse::Sql
