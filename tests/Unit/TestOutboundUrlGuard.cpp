#include "Base/Security/OutboundUrlGuard.h"

#include <gtest/gtest.h>

#include <string_view>


namespace
{
    [[nodiscard]] bool isBlocked(const std::string_view address)
    {
        return EdgePulse::Common::OutboundUrlGuard::isPrivateOrReservedAddress(address);
    }
} // namespace

// 钉住：私网、回环、链路本地、云元数据与 IPv4-mapped IPv6 全部判为不可出站
TEST(OutboundUrlGuard, ClassifiesReservedAddresses)
{
    EXPECT_TRUE(isBlocked("0.0.0.0"));
    EXPECT_TRUE(isBlocked("0.1.2.3"));
    EXPECT_TRUE(isBlocked("10.0.0.8"));
    EXPECT_TRUE(isBlocked("100.64.0.1"));
    EXPECT_TRUE(isBlocked("100.127.255.255"));
    EXPECT_TRUE(isBlocked("127.0.0.1"));
    EXPECT_TRUE(isBlocked("169.254.1.1"));
    EXPECT_TRUE(isBlocked("169.254.169.254"));
    EXPECT_TRUE(isBlocked("172.16.0.1"));
    EXPECT_TRUE(isBlocked("172.31.255.255"));
    EXPECT_TRUE(isBlocked("192.0.0.1"));
    EXPECT_TRUE(isBlocked("192.0.2.10"));
    EXPECT_TRUE(isBlocked("192.168.1.1"));
    EXPECT_TRUE(isBlocked("198.18.0.1"));
    EXPECT_TRUE(isBlocked("198.19.255.255"));
    EXPECT_TRUE(isBlocked("198.51.100.7"));
    EXPECT_TRUE(isBlocked("203.0.113.9"));
    EXPECT_TRUE(isBlocked("224.0.0.1"));
    EXPECT_TRUE(isBlocked("240.0.0.1"));
    EXPECT_TRUE(isBlocked("255.255.255.255"));
    EXPECT_FALSE(isBlocked("1.1.1.1"));
    EXPECT_FALSE(isBlocked("8.8.8.8"));
    EXPECT_FALSE(isBlocked("93.184.216.34"));
    EXPECT_FALSE(isBlocked("100.128.0.1"));
    EXPECT_FALSE(isBlocked("172.32.0.1"));
    EXPECT_FALSE(isBlocked("192.169.0.1"));
    EXPECT_FALSE(isBlocked("198.20.0.1"));

    EXPECT_TRUE(isBlocked("::"));
    EXPECT_TRUE(isBlocked("::1"));
    EXPECT_TRUE(isBlocked("fc00::1"));
    EXPECT_TRUE(isBlocked("fd12:3456::1"));
    EXPECT_TRUE(isBlocked("fe80::1"));
    EXPECT_TRUE(isBlocked("ff02::1"));
    EXPECT_TRUE(isBlocked("2001:db8::1"));
    EXPECT_TRUE(isBlocked("::ffff:127.0.0.1"));
    EXPECT_TRUE(isBlocked("::ffff:10.0.0.1"));
    EXPECT_TRUE(isBlocked("[::ffff:192.168.1.1]"));
    EXPECT_FALSE(isBlocked("::ffff:8.8.8.8"));
    EXPECT_FALSE(isBlocked("2001:4860:4860::8888"));
    EXPECT_FALSE(isBlocked("2606:4700:4700::1111"));

    EXPECT_TRUE(EdgePulse::Common::OutboundUrlGuard::isMetadataAddress("169.254.169.254"));
    EXPECT_TRUE(EdgePulse::Common::OutboundUrlGuard::isMetadataAddress("::ffff:169.254.169.254"));
    EXPECT_FALSE(EdgePulse::Common::OutboundUrlGuard::isMetadataAddress("127.0.0.1"));
    EXPECT_FALSE(EdgePulse::Common::OutboundUrlGuard::isMetadataAddress("93.184.216.34"));
    EXPECT_FALSE(EdgePulse::Common::OutboundUrlGuard::isMetadataAddress("not-an-address"));
}
