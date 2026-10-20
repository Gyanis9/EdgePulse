/**
 * @file IdGenerator.h
 * @brief 进程内时间有序的 64 位标识生成
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>

namespace EdgePulse::Common
{
    /**
     * @brief 生成一个时间有序的正整数标识
     * @return std::int64_t 高 44 位为毫秒时间，低 20 位为进程内序列
     * @note 线程安全；同一毫秒内超过 2^20 个标识时序列会回绕，P0 的业务量不会触发
     */
    [[nodiscard]] std::int64_t nextIdentifier();
} // namespace EdgePulse::Common
