/**
 * @file Environment.h
 * @brief 环境变量读取
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace EdgePulse::Common
{
    /**
     * @brief 读一个环境变量
     * @param name 变量名
     * @return 有值时返回文本；变量不存在或为空时为空
     * @note MSVC 下走 _dupenv_s，避免 getenv 的 C4996 告警
     */
    [[nodiscard]] std::optional<std::string> environmentValue(std::string_view name);
} // namespace EdgePulse::Common
