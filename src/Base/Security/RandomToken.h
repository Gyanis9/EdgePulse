/**
 * @file RandomToken.h
 * @brief 会话与验证码标识的随机文本
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <string>

namespace EdgePulse::Common
{
    /**
     * @brief 取一段小写十六进制随机文本
     * @param byteCount 随机字节数，输出长度是它的两倍
     * @return std::string 十六进制文本
     * @throws Base::Exception 随机源不可用
     */
    [[nodiscard]] std::string randomTokenHex(std::size_t byteCount);
} // namespace EdgePulse::Common
