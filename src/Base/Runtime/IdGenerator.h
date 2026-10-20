/**
 * @file IdGenerator.h
 * @brief 进程内时间有序的 64 位标识生成
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>

namespace EdgePulse::Common
{
    /**
     * @brief 生成一个时间有序的正整数标识
     * @return std::int64_t 毫秒时间左移 10 位后拼上进程内序列
     * @note 线程安全；同一毫秒内超过 2^10 个标识时序列会回绕，业务量不会触发
     * @note 取值保持在 JavaScript 安全整数（2^53 - 1）以内：管理端把标识当数字解析，
     *       超出范围会被舍入成另一个值，后续编辑与删除都会指向不存在的行
     */
    [[nodiscard]] std::int64_t nextIdentifier();
} // namespace EdgePulse::Common
