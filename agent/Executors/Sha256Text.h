/**
 * @file Sha256Text.h
 * @brief SHA-256 十六进制文本的判定与小写化
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace EdgePulse::Agent
{
    /// SHA-256 十六进制文本的固定长度
    inline constexpr std::size_t kSha256HexLength = 64;

    /**
     * @brief 判断文本是否恰好 64 个十六进制字符
     * @param text 待判断文本
     * @return true 表示长度与字符集都符合 SHA-256 十六进制写法
     */
    [[nodiscard]] inline bool isSha256HexText(const std::string_view text)
    {
        if (text.size() != kSha256HexLength)
        {
            return false;
        }
        for (const char character: text)
        {
            const bool isDigit = character >= '0' && character <= '9';
            const bool isLower = character >= 'a' && character <= 'f';
            const bool isUpper = character >= 'A' && character <= 'F';
            if (!isDigit && !isLower && !isUpper)
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 把十六进制文本折成小写
     * @param text 十六进制文本
     * @return std::string 小写副本
     */
    [[nodiscard]] inline std::string lowercaseHexText(const std::string_view text)
    {
        std::string lowered;
        lowered.reserve(text.size());
        for (const char character: text)
        {
            lowered.push_back(character >= 'A' && character <= 'F' ? static_cast<char>(character - 'A' + 'a') : character);
        }
        return lowered;
    }
} // namespace EdgePulse::Agent
