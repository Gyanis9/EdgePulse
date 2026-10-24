#include "Core/Auth/Totp.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>

namespace
{
    constexpr const char *kRfcSecretText = "12345678901234567890";
    constexpr const char *kRfcBase32     = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";

    [[nodiscard]] std::span<const std::uint8_t> bytesOf(const std::string &text)
    {
        return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
    }
} // namespace

// 钉住：base32 无填充编码与 RFC 4648 一致，解码大小写都可接受且拒绝非法字符
TEST(Totp, Base32MatchesRfc4648AndRoundTrips)
{
    const std::string secretText = kRfcSecretText;
    const std::string encoded    = EdgePulse::Auth::Totp::base32Encode(bytesOf(secretText));
    EXPECT_EQ(kRfcBase32, encoded);

    const auto decoded = EdgePulse::Auth::Totp::base32Decode(kRfcBase32);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(secretText, std::string(decoded->begin(), decoded->end()));

    const auto lowercase = EdgePulse::Auth::Totp::base32Decode("gezdgnbvgy3tqojqgezdgnbvgy3tqojq");
    ASSERT_TRUE(lowercase.has_value());
    EXPECT_EQ(secretText, std::string(lowercase->begin(), lowercase->end()));

    EXPECT_FALSE(EdgePulse::Auth::Totp::base32Decode("not-base32!").has_value());
    EXPECT_FALSE(EdgePulse::Auth::Totp::base32Decode("A").has_value());
}

// 钉住：RFC 6238 附录 B 的 SHA-1 测试向量在 8 位输出下逐条命中
TEST(Totp, MatchesRfc6238Sha1Vectors)
{
    const std::string secret = kRfcBase32;
    EXPECT_EQ("94287082", EdgePulse::Auth::Totp::code(secret, 59 / 30, 8));
    EXPECT_EQ("07081804", EdgePulse::Auth::Totp::code(secret, 1111111109 / 30, 8));
    EXPECT_EQ("14050471", EdgePulse::Auth::Totp::code(secret, 1111111111 / 30, 8));
    EXPECT_EQ("89005924", EdgePulse::Auth::Totp::code(secret, 1234567890 / 30, 8));
    EXPECT_EQ("69279037", EdgePulse::Auth::Totp::code(secret, 2000000000 / 30, 8));
    EXPECT_EQ("65353130", EdgePulse::Auth::Totp::code(secret, 20000000000 / 30, 8));
}

// 钉住：默认 6 位输出取 RFC 8 位向量的后 6 位并补足前导零
TEST(Totp, DefaultDigitsTakeTailOfRfcVector)
{
    const std::string secret = kRfcBase32;
    EXPECT_EQ("287082", EdgePulse::Auth::Totp::code(secret, 59 / 30));
    EXPECT_EQ("081804", EdgePulse::Auth::Totp::code(secret, 1111111109 / 30));
    EXPECT_EQ("005924", EdgePulse::Auth::Totp::code(secret, 1234567890 / 30));
}

// 钉住：±1 步窗口内接受，窗口外拒绝，已消费的步号拒绝重放
TEST(Totp, VerifyHonorsWindowAndRejectsReplay)
{
    const std::string  secret  = kRfcBase32;
    const std::int64_t nowStep = 100;
    const std::string  current = EdgePulse::Auth::Totp::code(secret, nowStep);

    EXPECT_TRUE(EdgePulse::Auth::Totp::verify(secret, current, nowStep, 0));
    EXPECT_TRUE(EdgePulse::Auth::Totp::verify(secret, EdgePulse::Auth::Totp::code(secret, nowStep - 1), nowStep, 0));
    EXPECT_TRUE(EdgePulse::Auth::Totp::verify(secret, EdgePulse::Auth::Totp::code(secret, nowStep + 1), nowStep, 0));
    EXPECT_FALSE(EdgePulse::Auth::Totp::verify(secret, EdgePulse::Auth::Totp::code(secret, nowStep - 2), nowStep, 0));
    EXPECT_FALSE(EdgePulse::Auth::Totp::verify(secret, EdgePulse::Auth::Totp::code(secret, nowStep + 2), nowStep, 0));

    EXPECT_FALSE(EdgePulse::Auth::Totp::verify(secret, current, nowStep, nowStep));
    EXPECT_FALSE(EdgePulse::Auth::Totp::verify(secret, current, nowStep, nowStep + 1));
    // 上一步已消费不影响当前步，步号比较的是严格大于
    EXPECT_TRUE(EdgePulse::Auth::Totp::verify(secret, current, nowStep, nowStep - 1));
    EXPECT_FALSE(EdgePulse::Auth::Totp::verify(secret, "abcdef", nowStep, 0));
    EXPECT_FALSE(EdgePulse::Auth::Totp::verify("not-base32!", current, nowStep, 0));

    EXPECT_TRUE(EdgePulse::Auth::Totp::verify(secret, current, nowStep, 0, 0));
    EXPECT_FALSE(EdgePulse::Auth::Totp::verify(secret, EdgePulse::Auth::Totp::code(secret, nowStep - 1), nowStep, 0, 0));
}

// 钉住：生成的秘密是 20 字节随机量，base32 展示为 32 个字符且每次不同
TEST(Totp, GenerateSecretIsTwentyRandomBytesInBase32)
{
    const std::string first  = EdgePulse::Auth::Totp::generateSecret();
    const std::string second = EdgePulse::Auth::Totp::generateSecret();
    EXPECT_EQ(32U, first.size());
    EXPECT_NE(first, second);

    const auto decoded = EdgePulse::Auth::Totp::base32Decode(first);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(20U, decoded->size());
}
