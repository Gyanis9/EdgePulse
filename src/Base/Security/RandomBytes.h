/**
 * @file RandomBytes.h
 * @brief 应用层的随机字节入口
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace EdgePulse::Common
{
    /**
     * @brief 取公开随机字节
     * @param byteCount 字节数
     * @return std::vector<std::uint8_t> 随机字节
     * @throws EdgePulseException 随机源不可用
     */
    [[nodiscard]] std::vector<std::uint8_t> randomBytes(std::size_t byteCount);

    /**
     * @brief 取密钥材料随机字节
     * @param byteCount 字节数
     * @return std::vector<std::uint8_t> 随机字节
     * @throws EdgePulseException 随机源不可用
     */
    [[nodiscard]] std::vector<std::uint8_t> randomSecretBytes(std::size_t byteCount);
} // namespace EdgePulse::Common
