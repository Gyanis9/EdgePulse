#include "Core/Http/RealtimeBus.h"
#include "Core/Http/SseBroadcaster.h"
#include "Core/Http/WebSocketEndpoints.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <optional>
#include <string>
#include <thread>

namespace
{
    [[nodiscard]] std::optional<std::string> environmentText(const char *name)
    {
#if defined(_MSC_VER)
        char  *buffer = nullptr;
        size_t length = 0;
        if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr)
        {
            return std::nullopt;
        }
        std::string value{buffer};
        std::free(buffer);
        return value.empty() ? std::nullopt : std::optional<std::string>{value};
#else
        const char *value = std::getenv(name);
        if (value == nullptr || *value == '\0')
        {
            return std::nullopt;
        }
        return std::string{value};
#endif
    }
} // namespace

// 钉住：两个实例共享 Redis 时，一个实例发布的事件能在另一个实例本地扇出（真实 Redis，缺环境变量即跳过）
TEST(RealtimeBus, RedisFanoutReachesSecondInstance)
{
    const std::optional<std::string> host = environmentText("EDGEPULSE_TEST_REDIS_HOST");
    if (!host.has_value())
    {
        GTEST_SKIP() << "缺少 EDGEPULSE_TEST_REDIS_HOST，跳过真实 Redis 用例";
    }
    const std::string password = environmentText("EDGEPULSE_TEST_REDIS_PASSWORD").value_or("");
    const std::string channel  = "edgepulse:test:events:" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    EdgePulse::TestSupport::EventLoopRunner runnerA;
    EdgePulse::TestSupport::EventLoopRunner runnerB;
    EdgePulse::Web::BrowserWebSocket        webSocketA;
    EdgePulse::Web::BrowserWebSocket        webSocketB;
    EdgePulse::Web::SseBroadcaster          sseA;
    EdgePulse::Web::SseBroadcaster          sseB;
    auto                                    received = std::make_shared<std::promise<std::string>>();
    std::atomic<int>                        localCount{0};

    EdgePulse::Web::RealtimeBusOptions optionsA;
    optionsA.enabled       = true;
    optionsA.host          = *host;
    optionsA.port          = 6379;
    optionsA.password      = password;
    optionsA.database      = 15;
    optionsA.channel       = channel;
    optionsA.localDelivery = [&localCount](std::string_view, std::int64_t, std::string_view) -> AsynGyanis::Core::Task<>
    {
        ++localCount;
        co_return;
    };
    EdgePulse::Web::RealtimeBusOptions optionsB = optionsA;
    optionsB.localDelivery                      = [received](std::string_view base, std::int64_t tenantId, std::string_view payload) -> AsynGyanis::Core::Task<>
    {
        if (base == "alarms" && tenantId == 7)
        {
            try
            {
                received->set_value(std::string{payload});
            } catch (...)
            {
            }
        }
        co_return;
    };

    EdgePulse::Web::RealtimeBus busA(runnerA.loop(), webSocketA, sseA, std::move(optionsA));
    EdgePulse::Web::RealtimeBus busB(runnerB.loop(), webSocketB, sseB, std::move(optionsB));
    busA.start();
    busB.start();

    const std::string payload = R"({"type":"alarm","data":{"id":1}})";
    auto              future  = received->get_future();
    for (int attempt = 0; attempt < 20 && future.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready; ++attempt)
    {
        runnerA.run(busA.publish("alarms", 7, payload));
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
    ASSERT_EQ(std::future_status::ready, future.wait_for(std::chrono::seconds{2}));
    EXPECT_EQ(payload, future.get());
    EXPECT_GE(localCount.load(), 1);
}
