/**
 * @file JsonFields.h
 * @brief JSON 请求体字段的严格读取
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Config/ConfigValue.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace EdgePulse::Json
{
    using AsynGyanis::Base::ConfigValue;

    /**
     * @brief 读取字符串字段；类型不符时回落到默认值
     * @param body 请求体，必须是对象
     * @param key 字段名
     * @param fallback 缺失或类型不符时的取值
     * @return std::string 字段文本
     */
    [[nodiscard]] inline std::string stringField(const ConfigValue &body, const std::string_view key, std::string fallback = {})
    {
        if (!body.is_object())
        {
            return fallback;
        }
        const auto iterator = body.find(std::string{key});
        if (iterator == body.end() || !iterator->is_string())
        {
            return fallback;
        }
        return iterator->get<std::string>();
    }

    /**
     * @brief 读取可选字符串字段；字段缺失或类型不符时为空
     * @param body 请求体
     * @param key 字段名
     * @return std::optional<std::string> 字段文本
     */
    [[nodiscard]] inline std::optional<std::string> optionalString(const ConfigValue &body, const std::string_view key)
    {
        if (!body.is_object())
        {
            return std::nullopt;
        }
        const auto iterator = body.find(std::string{key});
        if (iterator == body.end() || !iterator->is_string())
        {
            return std::nullopt;
        }
        return iterator->get<std::string>();
    }

    /**
     * @brief 读取整数字段；浮点与字符串不隐式转换
     * @param body 请求体
     * @param key 字段名
     * @param fallback 缺失或类型不符时的取值
     * @return std::int64_t 整数值
     */
    [[nodiscard]] inline std::int64_t integerField(const ConfigValue &body, const std::string_view key, const std::int64_t fallback = 0)
    {
        if (!body.is_object())
        {
            return fallback;
        }
        const auto iterator = body.find(std::string{key});
        if (iterator == body.end() || !iterator->is_number_integer())
        {
            return fallback;
        }
        return iterator->get<std::int64_t>();
    }

    /**
     * @brief 读取可选整数字段
     * @param body 请求体
     * @param key 字段名
     * @return std::optional<std::int64_t> 整数值
     */
    [[nodiscard]] inline std::optional<std::int64_t> optionalInteger(const ConfigValue &body, const std::string_view key)
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
     * @brief 读取布尔字段；数字 0/1 不当作布尔
     * @param body 请求体
     * @param key 字段名
     * @param fallback 缺失或类型不符时的取值
     * @return bool 字段值
     */
    [[nodiscard]] inline bool boolField(const ConfigValue &body, const std::string_view key, const bool fallback = false)
    {
        if (!body.is_object())
        {
            return fallback;
        }
        const auto iterator = body.find(std::string{key});
        if (iterator == body.end() || !iterator->is_boolean())
        {
            return fallback;
        }
        return iterator->get<bool>();
    }

    /**
     * @brief 读取整数数组；非整数元素被丢弃
     * @param body 请求体
     * @param key 字段名
     * @return std::vector<std::int64_t> 整数值列表
     */
    [[nodiscard]] inline std::vector<std::int64_t> integerArrayField(const ConfigValue &body, const std::string_view key)
    {
        std::vector<std::int64_t> values;
        if (!body.is_object())
        {
            return values;
        }
        const auto iterator = body.find(std::string{key});
        if (iterator == body.end() || !iterator->is_array())
        {
            return values;
        }
        values.reserve(iterator->size());
        for (const ConfigValue &item: *iterator)
        {
            if (item.is_number_integer())
            {
                values.push_back(item.get<std::int64_t>());
            }
        }
        return values;
    }

    /**
     * @brief 构造字符串 JSON 数组
     * @param values 字符串列表
     * @return ConfigValue 数组
     */
    [[nodiscard]] inline ConfigValue stringArray(const std::vector<std::string> &values)
    {
        ConfigValue array = ConfigValue::array();
        for (const std::string &value: values)
        {
            array.push_back(value);
        }
        return array;
    }

    /**
     * @brief 构造整数 JSON 数组
     * @param values 整数列表
     * @return ConfigValue 数组
     */
    [[nodiscard]] inline ConfigValue integerArray(const std::vector<std::int64_t> &values)
    {
        ConfigValue array = ConfigValue::array();
        for (const std::int64_t value: values)
        {
            array.push_back(value);
        }
        return array;
    }

    /**
     * @brief 读取嵌套对象并序列化为紧凑文本；字段缺失时为空串
     * @param body 请求体
     * @param key 字段名
     * @return std::optional<std::string> JSON 文本；字段存在但既不是对象也不是字符串时为空
     */
    [[nodiscard]] inline std::optional<std::string> jsonText(const ConfigValue &body, const std::string_view key)
    {
        if (!body.is_object())
        {
            return std::nullopt;
        }
        const auto iterator = body.find(std::string{key});
        if (iterator == body.end() || iterator->is_null())
        {
            return std::string{};
        }
        if (iterator->is_string())
        {
            return iterator->get<std::string>();
        }
        if (iterator->is_object() || iterator->is_array())
        {
            return iterator->dump();
        }
        return std::nullopt;
    }
} // namespace EdgePulse::Json
