#include "Core/Auth/CaptchaService.h"
#include "Core/Cache/MemoryKeyValueStore.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cctype>
#include <memory>
#include <string>

// 钉住：答案是一次性的，大小写不敏感，第二次提交同一条必失败
TEST(CaptchaService, ConsumesOnceCaseInsensitively)
{
    auto                                    current = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    auto                                    cache   = std::make_shared<EdgePulse::Cache::MemoryKeyValueStore>([current] { return *current; });
    EdgePulse::Auth::CaptchaService         service(cache, EdgePulse::Auth::CaptchaOptions{120, 30});
    EdgePulse::TestSupport::EventLoopRunner runner;

    const auto challenge = runner.run(service.issue(runner.loop(), "127.0.0.1"));
    ASSERT_TRUE(challenge.has_value());
    EXPECT_EQ(4U, challenge->code.size());
    EXPECT_NE(std::string::npos, challenge->svg.find("<svg"));
    EXPECT_EQ(32U, challenge->uuid.size());

    std::string lowercase = challenge->code;
    for (char &character: lowercase)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    EXPECT_TRUE(runner.run(service.consume(runner.loop(), challenge->uuid, lowercase)));
    EXPECT_FALSE(runner.run(service.consume(runner.loop(), challenge->uuid, challenge->code)));
}

// 钉住：超过有效期后拒绝
TEST(CaptchaService, ExpiresAfterTtl)
{
    auto                                    current = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    auto                                    cache   = std::make_shared<EdgePulse::Cache::MemoryKeyValueStore>([current] { return *current; });
    EdgePulse::Auth::CaptchaService         service(cache, EdgePulse::Auth::CaptchaOptions{60, 30});
    EdgePulse::TestSupport::EventLoopRunner runner;

    const auto challenge = runner.run(service.issue(runner.loop(), "127.0.0.1"));
    ASSERT_TRUE(challenge.has_value());
    *current += std::chrono::seconds{61};
    EXPECT_FALSE(runner.run(service.consume(runner.loop(), challenge->uuid, challenge->code)));
}

// 钉住：同一 IP 超过每分钟上限时签发失败，窗口过去后恢复
TEST(CaptchaService, LimitsPerIpAndRecoversNextWindow)
{
    auto                                    current = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    auto                                    cache   = std::make_shared<EdgePulse::Cache::MemoryKeyValueStore>([current] { return *current; });
    EdgePulse::Auth::CaptchaService         service(cache, EdgePulse::Auth::CaptchaOptions{120, 2});
    EdgePulse::TestSupport::EventLoopRunner runner;

    EXPECT_TRUE(runner.run(service.issue(runner.loop(), "10.0.0.1")).has_value());
    EXPECT_TRUE(runner.run(service.issue(runner.loop(), "10.0.0.1")).has_value());
    const auto limited = runner.run(service.issue(runner.loop(), "10.0.0.1"));
    ASSERT_FALSE(limited.has_value());
    EXPECT_EQ(EdgePulse::Auth::CaptchaErrorReason::RateLimited, limited.error().reason);

    // 另一个 IP 不受影响
    EXPECT_TRUE(runner.run(service.issue(runner.loop(), "10.0.0.2")).has_value());

    *current += std::chrono::seconds{61};
    EXPECT_TRUE(runner.run(service.issue(runner.loop(), "10.0.0.1")).has_value());
}
