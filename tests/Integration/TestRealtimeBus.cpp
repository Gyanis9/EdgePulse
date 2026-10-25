#include "Core/Http/RealtimeBus.h"
#include "Core/Http/SseBroadcaster.h"
#include "Core/Http/WebSocketEndpoints.h"
#include "Base/Config/ConfigValue.h"
#include "Database/Redis/RedisConnection.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace
{
    /**
     * @brief 注入的发布连接观测点
     * @details 用例在线程之间共享计数与闸门；发布连接的实现只读这块状态，不碰真实网络
     */
    struct FakePublisherState
    {
        std::atomic<int>   connectCalls{0};         ///< connect 调用次数
        std::atomic<int>   probeCalls{0};           ///< PING 调用次数
        std::atomic<int>   publishCalls{0};         ///< PUBLISH 调用次数
        std::atomic<bool>  connectResult{true};     ///< 下一次 connect 的结论
        std::atomic<bool>  publishResult{true};     ///< 下一次 PUBLISH 的结论
        std::atomic<bool>  connected{false};        ///< 本端连接标志
        std::atomic<bool>  blockNextPublish{false}; ///< 下一次 PUBLISH 是否在闸门处停住
        std::promise<void> publishEntered;          ///< 已进入 PUBLISH 的闸门
        std::future<void>  publishRelease;          ///< 用例放开 PUBLISH 的闸门
        std::mutex         envelopeMutex;           ///< 保护 lastEnvelope
        std::string        lastEnvelope;            ///< 最近一次 PUBLISH 的信封
    };

    /**
     * @brief 发布连接替身：按观测点的脚本返回，不执行任何网络往返
     */
    class FakePublisherTransport final : public EdgePulse::Web::PublisherTransport
    {
    public:
        /**
         * @brief 绑定共享观测点
         * @param state 用例持有的观测点
         */
        explicit FakePublisherTransport(std::shared_ptr<FakePublisherState> state) : m_state(std::move(state))
        {
        }

        bool connect() override
        {
            ++m_state->connectCalls;
            m_state->connected.store(m_state->connectResult.load());
            return m_state->connected.load();
        }

        void disconnect() override
        {
            m_state->connected.store(false);
        }

        [[nodiscard]] bool isConnected() const override
        {
            return m_state->connected.load();
        }

        [[nodiscard]] bool probeAlive() noexcept override
        {
            ++m_state->probeCalls;
            return m_state->connected.load();
        }

        [[nodiscard]] bool publish(std::string_view, std::string_view envelope) override
        {
            ++m_state->publishCalls;
            {
                const std::lock_guard lock(m_state->envelopeMutex);
                m_state->lastEnvelope = std::string{envelope};
            }
            if (m_state->blockNextPublish.exchange(false))
            {
                m_state->publishEntered.set_value();
                m_state->publishRelease.wait();
            }
            if (!m_state->publishResult.load())
            {
                m_state->connected.store(false);
                return false;
            }
            return true;
        }

        [[nodiscard]] std::string lastError() const override
        {
            return "注入的发布连接失败";
        }

    private:
        std::shared_ptr<FakePublisherState> m_state; ///< 用例观测点
    };

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

// 钉住：浏览器发布仍是 target=browser 的本地扇出加一次 PUBLISH，agent 路径不改变它
TEST(RealtimeBus, BrowserPublishKeepsBrowserTargetAndLocalFanout)
{
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::Web::BrowserWebSocket        webSocket;
    EdgePulse::Web::SseBroadcaster          sse;

    auto             state = std::make_shared<FakePublisherState>();
    std::atomic<int> localDeliveries{0};

    EdgePulse::Web::RealtimeBusOptions options;
    options.enabled          = true;
    options.instanceId       = "node-browser";
    options.startSubscriber  = false;
    options.publisherFactory = [state] { return std::make_unique<FakePublisherTransport>(state); };
    options.localDelivery    = [&localDeliveries](std::string_view, std::int64_t, std::string_view) -> AsynGyanis::Core::Task<>
    {
        ++localDeliveries;
        co_return;
    };

    EdgePulse::Web::RealtimeBus bus(runner.loop(), webSocket, sse, std::move(options));
    bus.start();
    runner.run(bus.publish("nodes", 7, R"({"type":"node.status"})"));

    EXPECT_EQ(1, localDeliveries.load(std::memory_order_relaxed));
    EXPECT_EQ(1, state->publishCalls.load(std::memory_order_relaxed));
    std::string envelope;
    {
        const std::lock_guard lock(state->envelopeMutex);
        envelope = state->lastEnvelope;
    }
    const auto parsed = AsynGyanis::Base::parseConfigValue(envelope);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ("browser", (parsed->value("target", std::string{})));
    EXPECT_EQ("nodes", (parsed->value("base", std::string{})));
    EXPECT_EQ(7, (parsed->value("tenantId", std::int64_t{0})));
    EXPECT_EQ(R"({"type":"node.status"})", (parsed->value("payload", std::string{})));
}

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

// 钉住：发布连接不健康时，事件循环上的 publish 不重连也不 PING，只做本地扇出；跳过的发布不改健康标记
TEST(RealtimeBus, UnhealthyPublisherSkipsRedisWithoutReconnecting)
{
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::Web::BrowserWebSocket        webSocket;
    EdgePulse::Web::SseBroadcaster          sse;

    auto state           = std::make_shared<FakePublisherState>();
    state->connectResult = false;
    std::atomic<int> localDeliveries{0};

    EdgePulse::Web::RealtimeBusOptions options;
    options.enabled          = true;
    options.startSubscriber  = false;
    options.publisherFactory = [state] { return std::make_unique<FakePublisherTransport>(state); };
    options.localDelivery    = [&localDeliveries](std::string_view, std::int64_t, std::string_view) -> AsynGyanis::Core::Task<>
    {
        ++localDeliveries;
        co_return;
    };

    EdgePulse::Web::RealtimeBus bus(runner.loop(), webSocket, sse, std::move(options));
    bus.start();
    ASSERT_EQ(1, state->connectCalls.load(std::memory_order_relaxed));
    ASSERT_FALSE(state->connected.load(std::memory_order_relaxed));

    const std::string payload = R"({"type":"alarm","data":{"id":1}})";
    runner.run(bus.publish("alarms", 7, payload));
    EXPECT_EQ(1, localDeliveries.load(std::memory_order_relaxed));
    EXPECT_EQ(1, state->connectCalls.load(std::memory_order_relaxed));
    EXPECT_EQ(0, state->probeCalls.load(std::memory_order_relaxed));
    EXPECT_EQ(0, state->publishCalls.load(std::memory_order_relaxed));

    // 跳过的发布不能把状态改成健康：连接仍然失败时，第二次发布依旧不碰 PUBLISH
    state->connectResult.store(true, std::memory_order_relaxed);
    runner.run(bus.publish("alarms", 7, payload));
    EXPECT_EQ(2, localDeliveries.load(std::memory_order_relaxed));
    EXPECT_EQ(1, state->connectCalls.load(std::memory_order_relaxed));
    EXPECT_EQ(0, state->probeCalls.load(std::memory_order_relaxed));
    EXPECT_EQ(0, state->publishCalls.load(std::memory_order_relaxed));
}

// 钉住：发布锁被占住时，事件循环上的 publish 立即跳过跨实例转发，不等锁、不重连、不 PING
TEST(RealtimeBus, BusyPublisherLockSkipsWithoutBlockingLoop)
{
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::Web::BrowserWebSocket        webSocket;
    EdgePulse::Web::SseBroadcaster          sse;

    auto state                = std::make_shared<FakePublisherState>();
    auto release              = std::make_shared<std::promise<void>>();
    state->publishRelease     = release->get_future();
    std::future<void> entered = state->publishEntered.get_future();

    EdgePulse::Web::RealtimeBusOptions options;
    options.enabled          = true;
    options.startSubscriber  = false;
    options.publisherFactory = [state] { return std::make_unique<FakePublisherTransport>(state); };
    options.localDelivery    = [](std::string_view, std::int64_t, std::string_view) -> AsynGyanis::Core::Task<> { co_return; };

    EdgePulse::Web::RealtimeBus bus(runner.loop(), webSocket, sse, std::move(options));
    bus.start();
    ASSERT_TRUE(state->connected.load(std::memory_order_relaxed));

    // publish 的载荷是引用参数，必须让字符串活过协程恢复；直接传字面量会在恢复前析构
    const std::string payload = R"({"type":"alarm","data":{"id":3}})";
    state->blockNextPublish.store(true, std::memory_order_relaxed);
    // 持锁线程代表订阅线程的探针：它通过真实发布路径拿到发布锁后停在 PUBLISH 里。
    // 这里用本地投递覆盖口，协程不碰事件循环，放在独立线程上运行不会与循环争用。
    std::thread holder(
            [&bus, &payload]
            {
                AsynGyanis::Core::Task<> task = bus.publish("alarms", 7, payload);
                task.handle().resume();
            });
    // 断言中途失败也要先放开闸门再 join，避免把持锁线程留在运行中
    struct HolderGuard
    {
        std::shared_ptr<std::promise<void>> release;
        std::thread                        &thread;

        void finish()
        {
            if (release)
            {
                try
                {
                    release->set_value();
                } catch (...)
                {
                }
                release.reset();
            }
            if (thread.joinable())
            {
                thread.join();
            }
        }

        ~HolderGuard()
        {
            finish();
        }
    } guard{release, holder};

    // 只等持锁线程进入 PUBLISH；闸门由用例自己造，超时只是防止用例本身挂死
    ASSERT_EQ(std::future_status::ready, entered.wait_for(std::chrono::seconds{10}))
            << "connectCalls=" << state->connectCalls.load() << " publishCalls=" << state->publishCalls.load() << " connected=" << state->connected.load();

    const auto begin    = std::chrono::steady_clock::now();
    bool       finished = true;
    try
    {
        runner.run(bus.publish("alarms", 7, payload));
    } catch (const std::exception &)
    {
        finished = false;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);

    EXPECT_TRUE(finished) << "发布路径在发布锁被占住时没有返回";
    EXPECT_LT(elapsed, std::chrono::milliseconds{1000});
    EXPECT_EQ(1, state->publishCalls.load(std::memory_order_relaxed));
    EXPECT_EQ(1, state->connectCalls.load(std::memory_order_relaxed));
    EXPECT_EQ(0, state->probeCalls.load(std::memory_order_relaxed));

    guard.finish();

    // 锁释放后同一条发布路径仍能正常转发
    runner.run(bus.publish("alarms", 7, payload));
    EXPECT_EQ(2, state->publishCalls.load(std::memory_order_relaxed));
}
