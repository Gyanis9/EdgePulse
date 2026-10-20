#include "Auth/CaptchaService.h"

#include <gtest/gtest.h>

#include <cctype>
#include <chrono>
#include <memory>

// 钉住：答案是一次性的，大小写不敏感，第二次提交同一条必失败
TEST(CaptchaService, ConsumesOnceCaseInsensitively)
{
    auto                            current = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    EdgePulse::Auth::CaptchaService service(EdgePulse::Auth::CaptchaOptions{120, 30}, [current] { return *current; });

    const auto challenge = service.issue("127.0.0.1");
    ASSERT_TRUE(challenge.has_value());
    EXPECT_EQ(4U, challenge->code.size());
    EXPECT_NE(std::string::npos, challenge->svg.find("<svg"));
    EXPECT_EQ(32U, challenge->uuid.size());

    std::string lowercase = challenge->code;
    for (char &character: lowercase)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    EXPECT_TRUE(service.consume(challenge->uuid, lowercase));
    EXPECT_FALSE(service.consume(challenge->uuid, challenge->code));
}

// 钉住：超过有效期后拒绝，且过期项不会留在缓存里
TEST(CaptchaService, ExpiresAfterTtl)
{
    auto                            current = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    EdgePulse::Auth::CaptchaService service(EdgePulse::Auth::CaptchaOptions{60, 30}, [current] { return *current; });

    const auto challenge = service.issue("127.0.0.1");
    ASSERT_TRUE(challenge.has_value());
    *current += std::chrono::seconds{61};
    EXPECT_FALSE(service.consume(challenge->uuid, challenge->code));
    EXPECT_EQ(0U, service.size());
}

// 钉住：同一 IP 超过每分钟上限时签发失败，窗口过去后恢复
TEST(CaptchaService, LimitsPerIpAndRecoversNextWindow)
{
    auto                            current = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    EdgePulse::Auth::CaptchaService service(EdgePulse::Auth::CaptchaOptions{120, 2}, [current] { return *current; });

    EXPECT_TRUE(service.issue("10.0.0.1").has_value());
    EXPECT_TRUE(service.issue("10.0.0.1").has_value());
    const auto limited = service.issue("10.0.0.1");
    ASSERT_FALSE(limited.has_value());
    EXPECT_EQ(EdgePulse::Auth::CaptchaErrorReason::RateLimited, limited.error().reason);

    // 另一个 IP 不受影响
    EXPECT_TRUE(service.issue("10.0.0.2").has_value());

    *current += std::chrono::seconds{61};
    EXPECT_TRUE(service.issue("10.0.0.1").has_value());
}
