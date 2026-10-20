/**
 * @file Hmac.h
 * @brief HMAC-SHA256 与十六进制恒定时间比较
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <string>
#include <string_view>

namespace EdgePulse::Common
{
    /**
     * @brief 计算 HMAC-SHA256 并输出小写十六进制
     * @param key 密钥字节
     * @param data 待认证数据
     * @return std::string 64 个十六进制字符；OpenSSL 初始化失败时为空
     */
    [[nodiscard]] std::string hmacSha256Hex(std::string_view key, std::string_view data);

    /**
     * @brief 比较两段十六进制文本，耗时与内容无关
     * @param left 左侧文本
     * @param right 右侧文本
     * @return bool 长度相同且逐字节相等（大小写不敏感）
     */
    [[nodiscard]] bool constantTimeHexEquals(std::string_view left, std::string_view right);
} // namespace EdgePulse::Common
