/**
 * @file PasswordHash.h
 * @brief PBKDF2-HMAC-SHA256 口令哈希
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace EdgePulse::Common
{
    inline constexpr int         kPbkdf2Iterations = 210000;
    inline constexpr std::size_t kPbkdf2SaltBytes  = 16;
    inline constexpr std::size_t kPbkdf2HashBytes  = 32;

    /**
     * @brief 生成口令哈希
     * @param password 明文口令，允许任意字节但不接受空串由调用方判定
     * @return 形如 pbkdf2_sha256$210000$<salt_b64>$<hash_b64> 的字符串
     * @throws Base::Exception 随机源取不出盐
     */
    [[nodiscard]] std::string hashPassword(std::string_view password);

    /**
     * @brief 校验口令
     * @param storedHash 已存储的哈希文本
     * @param password 待校验明文
     * @return true 格式合法且口令匹配
     * @return false 格式非法、算法不符或口令不匹配
     * @note 比较用恒定时间；解析失败一律返回 false，不向调用方区分是哪一种失败
     */
    [[nodiscard]] bool verifyPassword(std::string_view storedHash, std::string_view password);
} // namespace EdgePulse::Common
