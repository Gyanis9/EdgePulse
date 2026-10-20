/**
 * @file TestUdpTelemetryProtocol.cpp
 * @brief UDP 遥测数据报的 HMAC、解析与重放防护
 * @author Gyanis
 * @date 2026-10-21
 */

#include "Base/Config/ConfigValue.h"
#include "Base/Security/Hmac.h"
#include "Core/Telemetry/UdpTelemetryProtocol.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace
{
    using AsynGyanis::Base::ConfigValue;

    struct SignedDatagram
    {
        std::string  text;
        std::string  nonce;
        std::int64_t timestamp{0};
    };

    [[nodiscard]] SignedDatagram sign(const std::string &secret, const std::int64_t nodeId, const std::int64_t timestamp, const std::string &nonce, const ConfigValue &body)
    {
        const std::string canonical = "v1\n" + std::to_string(nodeId) + "\n" + std::to_string(timestamp) + "\n" + nonce + "\n" + body.dump();
        const std::string signature = EdgePulse::Common::hmacSha256Hex(secret, canonical);
        return SignedDatagram{ConfigValue::object({{"nodeId", nodeId}, {"timestamp", timestamp}, {"nonce", nonce}, {"body", body}, {"signature", signature}}).dump(), nonce,
                              timestamp};
    }

    [[nodiscard]] ConfigValue sampleBody()
    {
        return ConfigValue::object({{"samples", ConfigValue::array({ConfigValue::object({{"metric", "cpu.usage"}, {"value", 42.5}, {"unit", "%"}})})}});
    }
} // namespace

// 钉住：HMAC-SHA256 与 RFC 4231 的测试向量一致
TEST(UdpTelemetry, HmacMatchesRfc4231Vectors)
{
    EXPECT_EQ("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", EdgePulse::Common::hmacSha256Hex(std::string(20, '\x0b'), "Hi There"));
    EXPECT_EQ("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", EdgePulse::Common::hmacSha256Hex("Jefe", "what do ya want for nothing?"));
    EXPECT_TRUE(EdgePulse::Common::constantTimeHexEquals("ABCDEF", "abcdef"));
    EXPECT_FALSE(EdgePulse::Common::constantTimeHexEquals("abcdef", "abcdeg"));
}

// 钉住：正确签名在时间窗内可解析，坏签名与过期报文被归为未认证
TEST(UdpTelemetry, RejectsBadSignatureAndStaleTimestamp)
{
    const std::string secret = "0123456789abcdef0123456789abcdef";
    const auto        now    = std::int64_t{1760000000};
    const auto        valid  = sign(secret, 7, now, "nonce-0001", sampleBody());

    const auto parsed = EdgePulse::Telemetry::parseUdpTelemetryDatagram(valid.text, secret, now);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(7, parsed.datagram.nodeId);
    ASSERT_EQ(1U, parsed.datagram.samples.size());
    EXPECT_EQ("cpu.usage", parsed.datagram.samples[0].metric);
    EXPECT_DOUBLE_EQ(42.5, parsed.datagram.samples[0].value);

    std::string tampered = valid.text;
    const auto  position = tampered.find("\"signature\":\"");
    ASSERT_NE(std::string::npos, position);
    tampered[position + 13] = tampered[position + 13] == 'a' ? 'b' : 'a';
    const auto badSignature = EdgePulse::Telemetry::parseUdpTelemetryDatagram(tampered, secret, now);
    EXPECT_FALSE(badSignature.ok);
    EXPECT_TRUE(badSignature.unauthorized);

    const auto stale       = sign(secret, 7, now - 3600, "nonce-0002", sampleBody());
    const auto staleResult = EdgePulse::Telemetry::parseUdpTelemetryDatagram(stale.text, secret, now);
    EXPECT_FALSE(staleResult.ok);
    EXPECT_TRUE(staleResult.unauthorized);
}

// 钉住：通过认证但样本越界的报文给出可读原因，不归为认证失败
TEST(UdpTelemetry, ReportsSampleValidationAfterAuthentication)
{
    const std::string secret  = "0123456789abcdef0123456789abcdef";
    const auto        now     = std::int64_t{1760000000};
    ConfigValue       samples = ConfigValue::array();
    for (int index = 0; index < 501; ++index)
    {
        samples.push_back(ConfigValue::object({{"metric", "cpu.usage"}, {"value", 1}}));
    }
    const auto datagram = sign(secret, 7, now, "nonce-0003", ConfigValue::object({{"samples", samples}}));
    const auto result   = EdgePulse::Telemetry::parseUdpTelemetryDatagram(datagram.text, secret, now);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.unauthorized);
    EXPECT_FALSE(result.error.empty());
}

// 钉住：同一节点同一随机串只收一次，窗口外的旧记录会被清理
TEST(UdpTelemetry, ReplayGuardRejectsDuplicatesAndPrunes)
{
    EdgePulse::Telemetry::UdpTelemetryReplayGuard guard(120);
    const auto                                    now = std::int64_t{1760000000};
    EXPECT_TRUE(guard.accept(7, "nonce-0001", now, now));
    EXPECT_FALSE(guard.accept(7, "nonce-0001", now, now));
    EXPECT_TRUE(guard.accept(8, "nonce-0001", now, now));
    EXPECT_EQ(2U, guard.size());
    EXPECT_TRUE(guard.accept(7, "nonce-0001", now + 1000, now + 1000));
    EXPECT_EQ(1U, guard.size());
}
