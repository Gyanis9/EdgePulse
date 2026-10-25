#include "Base/Config/ConfigValue.h"
#include "Core/Command/AgentChannelRegistry.h"
#include "Core/Command/AgentCommandDelivery.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Http/RealtimeBus.h"
#include "Core/Http/SseBroadcaster.h"
#include "Core/Http/WebSocketEndpoints.h"
#include "Net/WebSocket/WebSocketPeer.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using AsynGyanis::Core::Task;
    using EdgePulse::Web::AgentMessage;

    /**
     * @brief 记录 PUBLISH 信封的发布连接替身
     */
    struct EnvelopeCapture
    {
        std::mutex               mutex;       ///< 保护 envelopes
        std::vector<std::string> envelopes;   ///< 已发出的信封，按发送顺序
        std::atomic<int>         calls{0};    ///< PUBLISH 调用次数
        std::atomic<int>         connects{0}; ///< connect 调用次数
    };

    /**
     * @brief 只记录信封、不做网络往返的发布连接
     */
    class CapturingPublisher final : public EdgePulse::Web::PublisherTransport
    {
    public:
        /**
         * @brief 绑定共享捕获点
         * @param capture 用例持有的捕获点
         */
        explicit CapturingPublisher(std::shared_ptr<EnvelopeCapture> capture) : m_capture(std::move(capture))
        {
        }

        bool connect() override
        {
            m_capture->connects.fetch_add(1, std::memory_order_release);
            return true;
        }

        void disconnect() override
        {
        }

        [[nodiscard]] bool isConnected() const override
        {
            return true;
        }

        [[nodiscard]] bool probeAlive() noexcept override
        {
            return true;
        }

        [[nodiscard]] bool publish(std::string_view, std::string_view envelope) override
        {
            {
                const std::lock_guard lock(m_capture->mutex);
                m_capture->envelopes.emplace_back(envelope);
            }
            m_capture->calls.fetch_add(1, std::memory_order_release);
            return true;
        }

        [[nodiscard]] std::string lastError() const override
        {
            return {};
        }

    private:
        std::shared_ptr<EnvelopeCapture> m_capture; ///< 用例捕获点
    };

    /**
     * @brief 记录已写出帧的 WebSocket 对端
     */
    struct PeerCapture
    {
        std::mutex               mutex;        ///< 保护 frames
        std::vector<std::string> frames;       ///< 已写出的完整帧
        bool                     accept{true}; ///< 写帧是否成功
    };

    /**
     * @brief 造一个只记录帧的对端，不连任何套接字
     * @param capture 帧捕获点
     * @return std::unique_ptr<AsynGyanis::Net::WebSocketPeer> 对端
     */
    [[nodiscard]] std::unique_ptr<AsynGyanis::Net::WebSocketPeer> makePeer(const std::shared_ptr<PeerCapture> &capture)
    {
        return std::make_unique<AsynGyanis::Net::WebSocketPeer>(
                [capture](std::string_view frame) -> Task<bool>
                {
                    {
                        const std::lock_guard lock(capture->mutex);
                        capture->frames.emplace_back(frame);
                    }
                    co_return capture->accept;
                });
    }

    /**
     * @brief 取对端写出的最后一帧副本
     * @param capture 帧捕获点
     * @return std::string 最后一帧；没有时为 empty
     */
    [[nodiscard]] std::string lastFrame(const std::shared_ptr<PeerCapture> &capture)
    {
        const std::lock_guard lock(capture->mutex);
        return capture->frames.empty() ? std::string{} : capture->frames.back();
    }

    /**
     * @brief 读环境变量文本
     * @param name 变量名
     * @return std::optional<std::string> 非空值
     */
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
     * @brief 读端口环境变量
     * @param name 变量名
     * @param fallback 缺省端口
     * @return std::uint16_t 端口
     */
    [[nodiscard]] std::uint16_t environmentPort(const char *name, const std::uint16_t fallback)
    {
        const std::optional<std::string> text = environmentText(name);
        return text.has_value() ? static_cast<std::uint16_t>(std::stoul(*text)) : fallback;
    }
} // namespace

// 钉住：publishAgent 只走 Agent handler，本地浏览器扇出不接收 agent 消息；浏览器发布仍照旧本地投递
TEST(AgentCommandBus, LocalAgentHandlerReceivesWithoutBrowserFanout)
{
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::Web::BrowserWebSocket        webSocket;
    EdgePulse::Web::SseBroadcaster          sse;

    std::atomic<int>          browserDeliveries{0};
    std::vector<AgentMessage> received;
    std::mutex                receivedMutex;

    EdgePulse::Web::RealtimeBusOptions options;
    options.localDelivery = [&browserDeliveries](std::string_view, std::int64_t, std::string_view) -> Task<>
    {
        browserDeliveries.fetch_add(1, std::memory_order_release);
        co_return;
    };
    EdgePulse::Web::RealtimeBus bus(runner.loop(), webSocket, sse, std::move(options));
    bus.setAgentMessageHandler(
            [&received, &receivedMutex](const AgentMessage &message) -> Task<>
            {
                {
                    const std::lock_guard lock(receivedMutex);
                    received.push_back(message);
                }
                co_return;
            });

    runner.run(bus.publishAgent(runner.loop(), AgentMessage{.nodeId = 7, .payload = R"({"type":"command"})", .commandId = 42}));
    EXPECT_EQ(0, browserDeliveries.load(std::memory_order_acquire)) << "agent 消息不能进入浏览器扇出";
    {
        const std::lock_guard lock(receivedMutex);
        ASSERT_EQ(1U, received.size());
        EXPECT_EQ(7, received[0].nodeId);
        EXPECT_EQ(42, received[0].commandId);
        EXPECT_EQ(R"({"type":"command"})", received[0].payload);
        EXPECT_FALSE(received[0].close);
    }

    runner.run(bus.publish("nodes", 1, R"({"type":"node.status"})"));
    EXPECT_EQ(1, browserDeliveries.load(std::memory_order_acquire));
    {
        const std::lock_guard lock(receivedMutex);
        EXPECT_EQ(1U, received.size()) << "浏览器事件不能进入 Agent handler";
    }
}

// 钉住：跨实例信封按 target 区分；Agent 信封带 nodeId/commandId/close 字段，浏览器信封默认 target=browser
TEST(AgentCommandBus, EnvelopeCarriesTargetNodeAndCloseFields)
{
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::Web::BrowserWebSocket        webSocket;
    EdgePulse::Web::SseBroadcaster          sse;

    auto capture = std::make_shared<EnvelopeCapture>();

    EdgePulse::Web::RealtimeBusOptions options;
    options.enabled          = true;
    options.instanceId       = "node-a";
    options.startSubscriber  = false;
    options.publisherFactory = [capture] { return std::make_unique<CapturingPublisher>(capture); };
    options.localDelivery    = [](std::string_view, std::int64_t, std::string_view) -> Task<> { co_return; };

    EdgePulse::Web::RealtimeBus bus(runner.loop(), webSocket, sse, std::move(options));
    bus.start();
    ASSERT_EQ(1, capture->connects.load(std::memory_order_acquire)) << "start 时必须建立发布连接";

    runner.run(bus.publishAgent(runner.loop(), AgentMessage{.nodeId = 9, .payload = R"({"type":"command","command":{"id":5}})", .commandId = 5}));
    runner.run(bus.publishAgent(runner.loop(), AgentMessage{.nodeId = 9, .close = true, .closeCode = 1008, .closeReason = "certificate revoked"}));
    runner.run(bus.publish("nodes", 3, R"({"type":"node.status"})"));

    std::vector<std::string> envelopes;
    {
        const std::lock_guard lock(capture->mutex);
        envelopes = capture->envelopes;
    }
    ASSERT_EQ(3U, envelopes.size());

    const auto commandEnvelope = AsynGyanis::Base::parseConfigValue(envelopes[0]);
    ASSERT_TRUE(commandEnvelope.has_value());
    EXPECT_EQ("agent", (commandEnvelope->value("target", std::string{})));
    EXPECT_EQ(9, (commandEnvelope->value("nodeId", std::int64_t{0})));
    EXPECT_EQ(5, (commandEnvelope->value("commandId", std::int64_t{0})));
    EXPECT_FALSE((commandEnvelope->value("close", true)));
    EXPECT_EQ(R"({"type":"command","command":{"id":5}})", (commandEnvelope->value("payload", std::string{})));

    const auto closeEnvelope = AsynGyanis::Base::parseConfigValue(envelopes[1]);
    ASSERT_TRUE(closeEnvelope.has_value());
    EXPECT_EQ("agent", (closeEnvelope->value("target", std::string{})));
    EXPECT_EQ(9, (closeEnvelope->value("nodeId", std::int64_t{0})));
    EXPECT_TRUE((closeEnvelope->value("close", false)));
    EXPECT_EQ(1008, (closeEnvelope->value("closeCode", 0)));
    EXPECT_EQ("certificate revoked", (closeEnvelope->value("closeReason", std::string{})));
    EXPECT_TRUE((closeEnvelope->value("payload", std::string{"non-empty"}).empty()));

    const auto browserEnvelope = AsynGyanis::Base::parseConfigValue(envelopes[2]);
    ASSERT_TRUE(browserEnvelope.has_value());
    EXPECT_EQ("browser", (browserEnvelope->value("target", std::string{})));
    EXPECT_EQ("nodes", (browserEnvelope->value("base", std::string{})));
    EXPECT_EQ(3, (browserEnvelope->value("tenantId", std::int64_t{0})));
    EXPECT_EQ(0, (browserEnvelope->value("nodeId", std::int64_t{-1})));
    EXPECT_EQ(0, (browserEnvelope->value("commandId", std::int64_t{-1})));
}

// 钉住：只有真正写出成功才把命令推进到 running；没有连接或写失败都保持 pending
TEST(AgentCommandBus, MarksDispatchedOnlyAfterSuccessfulSend)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;

    auto       nodes    = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto       commands = std::make_shared<EdgePulse::Command::CommandRepository>(context.database);
    const auto node     = runner.run(nodes->create(runner.loop(), "agent-bus-node", "{}"));

    auto first = runner.run(commands->create(runner.loop(), node.id, "reboot", "{}", 1, 60));
    ASSERT_TRUE(first.has_value());

    EdgePulse::Command::AgentChannelRegistry channels;
    EdgePulse::Command::AgentCommandDelivery delivery(channels, commands->scoped({0, true}));

    runner.run(delivery.deliver(runner.loop(), EdgePulse::Command::makeAgentCommandMessage(*first)));
    auto stored = runner.run(commands->find(runner.loop(), first->id));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ("pending", stored->status) << "没有连接时命令必须保持待执行";

    auto capture = std::make_shared<PeerCapture>();
    auto peer    = makePeer(capture);
    channels.registerNode(node.id, peer.get());
    runner.run(delivery.deliver(runner.loop(), EdgePulse::Command::makeAgentCommandMessage(*first)));
    stored = runner.run(commands->find(runner.loop(), first->id));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ("running", stored->status);
    ASSERT_FALSE(stored->dispatchedAt.empty());
    {
        const std::string frame = lastFrame(capture);
        ASSERT_FALSE(frame.empty());
        EXPECT_NE(std::string::npos, frame.find("\"type\":\"command\""));
    }

    auto second = runner.run(commands->create(runner.loop(), node.id, "reboot", "{}", 1, 60));
    ASSERT_TRUE(second.has_value());
    auto failingCapture    = std::make_shared<PeerCapture>();
    failingCapture->accept = false;
    auto failingPeer       = makePeer(failingCapture);
    channels.registerNode(node.id, failingPeer.get());
    runner.run(delivery.deliver(runner.loop(), EdgePulse::Command::makeAgentCommandMessage(*second)));
    stored = runner.run(commands->find(runner.loop(), second->id));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ("pending", stored->status) << "写出失败不能把命令标成已下发";
    EXPECT_FALSE(failingPeer->isOpen());
}

// 钉住：close 消息关闭本地连接并摘表，状态码与原因随帧带到对端
TEST(AgentCommandBus, CloseMessageClosesAndUnregistersPeer)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;

    auto       nodes    = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto       commands = std::make_shared<EdgePulse::Command::CommandRepository>(context.database);
    const auto node     = runner.run(nodes->create(runner.loop(), "agent-close-node", "{}"));

    EdgePulse::Command::AgentChannelRegistry channels;
    EdgePulse::Command::AgentCommandDelivery delivery(channels, commands->scoped({0, true}));
    auto                                     capture = std::make_shared<PeerCapture>();
    auto                                     peer    = makePeer(capture);
    channels.registerNode(node.id, peer.get());
    ASSERT_TRUE(channels.isConnected(node.id));

    runner.run(delivery.deliver(runner.loop(), EdgePulse::Command::makeAgentCloseMessage(node.id, 1008, "certificate revoked")));
    EXPECT_FALSE(channels.isConnected(node.id));
    EXPECT_FALSE(peer->isOpen());

    const std::string frame = lastFrame(capture);
    ASSERT_GE(frame.size(), 4U) << "关闭帧没有写出";
    EXPECT_EQ(static_cast<unsigned char>(0x88), static_cast<unsigned char>(frame[0])) << "最后一帧必须是 Close";
    EXPECT_EQ(1008, (static_cast<int>(static_cast<unsigned char>(frame[2])) << 8) | static_cast<int>(static_cast<unsigned char>(frame[3])));
    EXPECT_NE(std::string::npos, frame.find("certificate revoked"));
}

// 钉住：两个实例共享 Redis 时，Agent 消息由远端 handler 收到，本地 handler 也收到，浏览器扇出两侧都不碰（真实 Redis，缺环境变量即跳过）
TEST(AgentCommandBus, RedisFanoutReachesAgentHandlerOnSecondInstance)
{
    const std::optional<std::string> host = environmentText("EDGEPULSE_TEST_REDIS_HOST");
    if (!host.has_value())
    {
        GTEST_SKIP() << "缺少 EDGEPULSE_TEST_REDIS_HOST，跳过真实 Redis 用例";
    }
    const std::string password = environmentText("EDGEPULSE_TEST_REDIS_PASSWORD").value_or("");
    const std::string channel  = "edgepulse:test:agent:" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    EdgePulse::TestSupport::EventLoopRunner runnerA;
    EdgePulse::TestSupport::EventLoopRunner runnerB;
    EdgePulse::Web::BrowserWebSocket        webSocketA;
    EdgePulse::Web::BrowserWebSocket        webSocketB;
    EdgePulse::Web::SseBroadcaster          sseA;
    EdgePulse::Web::SseBroadcaster          sseB;

    std::atomic<int> browserA{0};
    std::atomic<int> browserB{0};
    auto             received = std::make_shared<std::promise<AgentMessage>>();
    std::atomic<int> localAgentDeliveries{0};

    EdgePulse::Web::RealtimeBusOptions optionsA;
    optionsA.enabled       = true;
    optionsA.host          = *host;
    optionsA.port          = environmentPort("EDGEPULSE_TEST_REDIS_PORT", 6379);
    optionsA.password      = password;
    optionsA.database      = 15;
    optionsA.channel       = channel;
    optionsA.instanceId    = "node-a";
    optionsA.localDelivery = [&browserA](std::string_view, std::int64_t, std::string_view) -> Task<>
    {
        browserA.fetch_add(1, std::memory_order_release);
        co_return;
    };
    EdgePulse::Web::RealtimeBusOptions optionsB = optionsA;
    optionsB.instanceId                         = "node-b";
    optionsB.localDelivery                      = [&browserB](std::string_view, std::int64_t, std::string_view) -> Task<>
    {
        browserB.fetch_add(1, std::memory_order_release);
        co_return;
    };

    EdgePulse::Web::RealtimeBus busA(runnerA.loop(), webSocketA, sseA, std::move(optionsA));
    EdgePulse::Web::RealtimeBus busB(runnerB.loop(), webSocketB, sseB, std::move(optionsB));
    busA.setAgentMessageHandler(
            [&localAgentDeliveries](const AgentMessage &) -> Task<>
            {
                localAgentDeliveries.fetch_add(1, std::memory_order_release);
                co_return;
            });
    busB.setAgentMessageHandler(
            [received](const AgentMessage &message) -> Task<>
            {
                try
                {
                    received->set_value(message);
                } catch (...)
                {
                }
                co_return;
            });
    busA.start();
    busB.start();
    for (int attempt = 0; attempt < 50 && !(busA.isHealthy() && busB.isHealthy()); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    ASSERT_TRUE(busA.isHealthy());
    ASSERT_TRUE(busB.isHealthy());

    auto future = received->get_future();
    runnerA.run(busA.publishAgent(runnerA.loop(), AgentMessage{.nodeId = 11, .payload = R"({"type":"command","command":{"id":77}})", .commandId = 77}));

    ASSERT_EQ(std::future_status::ready, future.wait_for(std::chrono::seconds{5})) << "远端实例没有收到 Agent 消息";
    const AgentMessage message = future.get();
    EXPECT_EQ(11, message.nodeId);
    EXPECT_EQ(77, message.commandId);
    EXPECT_EQ(R"({"type":"command","command":{"id":77}})", message.payload);
    EXPECT_FALSE(message.close);
    EXPECT_EQ(1, localAgentDeliveries.load(std::memory_order_acquire)) << "发布实例也必须本地投递一次";
    EXPECT_EQ(0, browserA.load(std::memory_order_acquire));
    EXPECT_EQ(0, browserB.load(std::memory_order_acquire)) << "agent 消息不能进入远端浏览器扇出";
}
