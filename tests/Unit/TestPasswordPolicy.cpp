#include "Base/Security/PasswordPolicy.h"

#include <gtest/gtest.h>

// 钉住：严格策略同时要求长度与四类字符，宽松策略只约束长度
TEST(PasswordPolicy, EnforcesLengthAndCharacterClasses)
{
    const EdgePulse::Common::PasswordPolicy strict(10, true);
    EXPECT_FALSE(strict.validate("short").has_value());
    EXPECT_FALSE(strict.validate("lowercase123").has_value());
    EXPECT_FALSE(strict.validate("UPPERCASE123").has_value());
    EXPECT_FALSE(strict.validate("NoDigitsHere!").has_value());
    EXPECT_TRUE(strict.validate("Strong-pass1").has_value());

    const EdgePulse::Common::PasswordPolicy lengthOnly(6, false);
    EXPECT_TRUE(lengthOnly.validate("simple").has_value());
    EXPECT_FALSE(lengthOnly.validate("12345").has_value());
}

// 钉住：超长口令与带空白的口令都不会被静默接受
TEST(PasswordPolicy, RejectsOverlongAndWhitespaceOnlyComplexity)
{
    const EdgePulse::Common::PasswordPolicy policy(8, true);
    EXPECT_FALSE(policy.validate(std::string{"Aa1!"} + std::string(61, 'a')).has_value());
    EXPECT_FALSE(policy.validate("Pass word1").has_value());
}

// 钉住：长度按 UTF-8 码点计，四个汉字不能靠 12 个字节冒充 10 个字符
TEST(PasswordPolicy, CountsCodePointsInsteadOfBytes)
{
    const EdgePulse::Common::PasswordPolicy policy(10, false);
    EXPECT_FALSE(policy.validate("口令测试").has_value());
    EXPECT_TRUE(policy.validate("口令测试口令测试口令").has_value());
    EXPECT_FALSE(policy.validate(std::string(65, 'a')).has_value());
}
