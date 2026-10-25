#include "Core/Http/RealtimeBus.h"
#include "Core/Http/SseBroadcaster.h"
#include "Core/Http/WebSocketEndpoints.h"
#include "Database/Redis/RedisConnection.h"
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

    /**
     * @brief 读端口环境变量，缺省时退回给定值
     */
    [[nodiscard]] std::uint16_t environmentPort(const char *name, const std::uint16_t fallback)
    {
        const std::optional<std::string> text = environmentText(name);
        if (!text.has_value())
        {
            return fallback;
        }
        return static_cast<std::uint16_t>(std::stoul(*text));
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
    optionsA.port          = environmentPort("EDGEPULSE_TEST_REDIS_PORT", 6379);
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

// 钉住：发布连接被服务端单方面丢弃后，发布路径会重连并恢复扇出，不需要重启进程（真实 Redis，缺环境变量即跳过）
TEST(RealtimeBus, RedisPublisherRecoversAfterServerSideKill)
{
    const std::optional<std::string> host = environmentText("EDGEPULSE_TEST_REDIS_HOST");
    if (!host.has_value())
    {
        GTEST_SKIP() << "缺少 EDGEPULSE_TEST_REDIS_HOST，跳过真实 Redis 用例";
    }
    const std::string   password = environmentText("EDGEPULSE_TEST_REDIS_PASSWORD").value_or("");
    const std::uint16_t port     = environmentPort("EDGEPULSE_TEST_REDIS_PORT", 6379);
    const std::string   channel  = "edgepulse:test:publisher-recovery:" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    EdgePulse::TestSupport::EventLoopRunner runnerA;
    EdgePulse::TestSupport::EventLoopRunner runnerB;
    EdgePulse::Web::BrowserWebSocket        webSocketA;
    EdgePulse::Web::BrowserWebSocket        webSocketB;
    EdgePulse::Web::SseBroadcaster          sseA;
    EdgePulse::Web::SseBroadcaster          sseB;
    auto                                    received = std::make_shared<std::promise<std::string>>();

    EdgePulse::Web::RealtimeBusOptions optionsA;
    optionsA.enabled                            = true;
    optionsA.host                               = *host;
    optionsA.port                               = port;
    optionsA.password                           = password;
    optionsA.database                           = 15;
    optionsA.channel                            = channel;
    optionsA.instanceId                         = "node-a";
    EdgePulse::Web::RealtimeBusOptions optionsB = optionsA;
    optionsB.instanceId                         = "node-b";
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
    for (int attempt = 0; attempt < 50 && !(busA.isHealthy() && busB.isHealthy()); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    ASSERT_TRUE(busA.isHealthy());
    ASSERT_TRUE(busB.isHealthy());

    AsynGyanis::Database::ConnectionConfig adminConfig = AsynGyanis::Database::ConnectionConfig::redisDefault();
    adminConfig.host                                   = *host;
    adminConfig.port                                   = port;
    adminConfig.password                               = password;
    adminConfig.database                               = "15";
    AsynGyanis::Database::RedisConnection admin(adminConfig);
    ASSERT_TRUE(admin.connect()) << admin.lastError();
    // 订阅连接是 pubsub 类型，不受 TYPE normal 影响；这里只踢掉两个发布连接
    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> killed = admin.executeCommand({"CLIENT", "KILL", "TYPE", "normal", "SKIPME", "yes"});
    ASSERT_NE(nullptr, killed) << admin.lastError();
    ASSERT_TRUE(killed->next());

    const std::string payload = R"({"type":"alarm","data":{"id":2}})";
    auto              future  = received->get_future();
    for (int attempt = 0; attempt < 20 && future.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready; ++attempt)
    {
        runnerA.run(busA.publish("alarms", 7, payload));
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
    ASSERT_EQ(std::future_status::ready, future.wait_for(std::chrono::seconds{2}));
    EXPECT_EQ(payload, future.get());
    EXPECT_TRUE(busA.isHealthy()) << "发布连接被杀后健康状态没有恢复";
}
