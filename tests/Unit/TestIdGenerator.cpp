/**
 * @file TestIdGenerator.cpp
 * @brief 标识生成的范围约束
 * @author Gyanis
 * @date 2026-10-21
 */

#include "Base/Runtime/IdGenerator.h"

#include <gtest/gtest.h>

#include <cstdint>

// 钉住：标识始终落在 JavaScript 安全整数范围内，管理端解析后不会被舍入
TEST(IdGenerator, StaysWithinJavaScriptSafeIntegerRange)
{
    constexpr std::int64_t kMaximumSafeIdentifier = (std::int64_t{1} << 53) - 1;
    for (int index = 0; index < 2048; ++index)
    {
        const std::int64_t identifier = EdgePulse::Common::nextIdentifier();
        EXPECT_GT(identifier, 0);
        EXPECT_LE(identifier, kMaximumSafeIdentifier);
    }
}
