#include "Base/Security/OutboundUrlGuard.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using EdgePulse::Common::HostAddressResolver;
    using EdgePulse::Common::OutboundUrlGuard;
    using EdgePulse::Common::OutboundUrlGuardOptions;

    [[nodiscard]] HostAddressResolver fixedResolver(std::vector<std::string> addresses)
    {
        return [addresses = std::move(addresses)](std::string_view) { return addresses; };
    }
} // namespace

// 钉住：allow_private_webhooks 放行自建内网，但云元数据地址仍然拒绝
TEST(OutboundUrlGuard, MetadataIsRejectedEvenWhenPrivateNetworksAreAllowed)
{
    EdgePulse::TestSupport::EventLoopRunner runner;
    const OutboundUrlGuard                  guard(OutboundUrlGuardOptions{.allowPrivateNetworks = true}, fixedResolver({"169.254.169.254"}));
    const auto                              metadata = runner.run(guard.validate(runner.loop(), "http://metadata.example/hook"));
    ASSERT_FALSE(metadata.has_value());
    EXPECT_NE(std::string::npos, metadata.error().find("元数据"));

    const OutboundUrlGuard privateGuard(OutboundUrlGuardOptions{.allowPrivateNetworks = true}, fixedResolver({"10.0.0.8"}));
    EXPECT_TRUE(runner.run(privateGuard.validate(runner.loop(), "http://gateway.internal/hook")).has_value());

    const OutboundUrlGuard publicGuard(OutboundUrlGuardOptions{}, fixedResolver({"93.184.216.34"}));
    EXPECT_TRUE(runner.run(publicGuard.validate(runner.loop(), "https://example.com/hook")).has_value());
    EXPECT_FALSE(runner.run(publicGuard.validate(runner.loop(), "http://127.0.0.1/hook")).has_value());
}

// 钉住：默认配置拒绝回环、内网与保留地址，公网地址照常放行
TEST(OutboundUrlGuard, RejectsPrivateAndReservedTargets)
{
    EdgePulse::TestSupport::EventLoopRunner runner;
    const OutboundUrlGuard                  guard(OutboundUrlGuardOptions{}, fixedResolver({"10.1.2.3"}));

    const auto hostName = runner.run(guard.validate(runner.loop(), "https://hooks.internal.example/hook"));
    ASSERT_FALSE(hostName.has_value());
    EXPECT_NE(std::string::npos, hostName.error().find("私网或保留地址"));

    for (const std::string &url: {"http://127.0.0.1/hook", "http://[::1]/hook", "http://192.168.1.5/hook", "http://169.254.1.1/hook"})
    {
        const auto rejected = runner.run(guard.validate(runner.loop(), url));
        EXPECT_FALSE(rejected.has_value()) << url;
        EXPECT_NE(std::string::npos, rejected.error().find("私网或保留地址")) << rejected.error();
    }

    const OutboundUrlGuard publicGuard(OutboundUrlGuardOptions{}, fixedResolver({"93.184.216.34"}));
    EXPECT_TRUE(runner.run(publicGuard.validate(runner.loop(), "http://public.example/hook")).has_value());
}

// 钉住：元数据主机名不解析也拒绝；解析结果为空时如实报解析失败
TEST(OutboundUrlGuard, RejectsMetadataHostNameAndEmptyResolution)
{
    EdgePulse::TestSupport::EventLoopRunner runner;
    const OutboundUrlGuard                  publicResolver(OutboundUrlGuardOptions{}, fixedResolver({"93.184.216.34"}));
    const auto                              metadataHost = runner.run(publicResolver.validate(runner.loop(), "http://metadata.google.internal/hook"));
    ASSERT_FALSE(metadataHost.has_value());
    EXPECT_NE(std::string::npos, metadataHost.error().find("元数据"));

    const OutboundUrlGuard emptyResolver(OutboundUrlGuardOptions{}, fixedResolver({}));
    const auto             unresolved = runner.run(emptyResolver.validate(runner.loop(), "http://missing.example/hook"));
    ASSERT_FALSE(unresolved.has_value());
    EXPECT_NE(std::string::npos, unresolved.error().find("解析失败"));
}

// 钉住：带凭据、畸形与非 http(s) 地址在解析前就被拒，错误文案不回显凭据
TEST(OutboundUrlGuard, RejectsUserInfoAndMalformedUrls)
{
    EdgePulse::TestSupport::EventLoopRunner runner;
    const OutboundUrlGuard                  guard(OutboundUrlGuardOptions{.allowPrivateNetworks = true}, fixedResolver({"93.184.216.34"}));

    const auto userInfo = runner.run(guard.validate(runner.loop(), "http://ops:topsecret@example.com/hook"));
    ASSERT_FALSE(userInfo.has_value());
    EXPECT_NE(std::string::npos, userInfo.error().find("不合法"));
    EXPECT_EQ(std::string::npos, userInfo.error().find("topsecret"));

    for (const std::string &url: {"example.com/hook", "ftp://example.com/hook", "http:///hook", "http://example.com:99999/hook"})
    {
        const auto rejected = runner.run(guard.validate(runner.loop(), url));
        EXPECT_FALSE(rejected.has_value()) << url;
        EXPECT_NE(std::string::npos, rejected.error().find("不合法")) << rejected.error();
    }
}

// 钉住：解析结果里只要混进一个私网地址就整条拒绝，不按第一个地址放行
TEST(OutboundUrlGuard, RejectsMixedPublicAndPrivateResolution)
{
    EdgePulse::TestSupport::EventLoopRunner runner;
    const OutboundUrlGuard                  guard(OutboundUrlGuardOptions{}, fixedResolver({"93.184.216.34", "10.0.0.8"}));
    const auto                              mixed = runner.run(guard.validate(runner.loop(), "https://rebind.example/hook"));
    ASSERT_FALSE(mixed.has_value());
    EXPECT_NE(std::string::npos, mixed.error().find("私网或保留地址"));
    EXPECT_NE(std::string::npos, mixed.error().find("10.0.0.8"));

    const OutboundUrlGuard permitted(OutboundUrlGuardOptions{.allowPrivateNetworks = true}, fixedResolver({"93.184.216.34", "10.0.0.8"}));
    EXPECT_TRUE(runner.run(permitted.validate(runner.loop(), "https://rebind.example/hook")).has_value());

    const OutboundUrlGuard metadataMixed(OutboundUrlGuardOptions{.allowPrivateNetworks = true}, fixedResolver({"93.184.216.34", "169.254.169.254"}));
    EXPECT_FALSE(runner.run(metadataMixed.validate(runner.loop(), "https://rebind.example/hook")).has_value());
}

// 钉住：同一主机在缓存期内只解析一次，过期由 TTL 决定
TEST(OutboundUrlGuard, CachesResolvedAddressesForTheTtl)
{
    EdgePulse::TestSupport::EventLoopRunner runner;
    std::atomic<int>                        lookups{0};
    const HostAddressResolver               resolver = [&lookups](std::string_view)
    {
        ++lookups;
        return std::vector<std::string>{"93.184.216.34"};
    };
    const OutboundUrlGuard guard(OutboundUrlGuardOptions{.cacheTtl = std::chrono::seconds{60}}, resolver);

    EXPECT_TRUE(runner.run(guard.validate(runner.loop(), "https://cached.example/hook")).has_value());
    EXPECT_TRUE(runner.run(guard.validate(runner.loop(), "https://CACHED.example/hook")).has_value());
    EXPECT_EQ(1, lookups.load());

    EXPECT_TRUE(runner.run(guard.validate(runner.loop(), "https://other.example/hook")).has_value());
    EXPECT_EQ(2, lookups.load());
}

// 钉住：localhost 这类本机主机名在禁止私网时直接拒绝，不把判断交给外部解析
TEST(OutboundUrlGuard, RejectsLocalHostNameWithoutResolving)
{
    EdgePulse::TestSupport::EventLoopRunner runner;
    std::atomic<int>                        lookups{0};
    const HostAddressResolver               resolver = [&lookups](std::string_view)
    {
        ++lookups;
        return std::vector<std::string>{"93.184.216.34"};
    };
    const OutboundUrlGuard guard(OutboundUrlGuardOptions{}, resolver);

    const auto rejected = runner.run(guard.validate(runner.loop(), "http://localhost/hook"));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_NE(std::string::npos, rejected.error().find("本机主机名"));
    EXPECT_EQ(0, lookups.load());
}
