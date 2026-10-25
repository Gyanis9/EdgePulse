#include "AgentApplication.h"
#include "AgentClient.h"
#include "AgentConfiguration.h"
#include "AgentLoop.h"
#include "AgentState.h"
#include "AgentWebSocket.h"
#include "CertificateFixture.h"
#include "CommandDispatcher.h"
#include "Executors/CommandExecutor.h"
#include "Executors/RestartExecutor.h"
#include "TestSupport/HttpClientHarness.h"
#include "TestSupport/TemporaryDirectory.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LoggerRegistry.h"
#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "Net/WebSocket/WebSocketPeer.h"
#include "Net/WebSocket/WebSocketUpgradePolicy.h"

#include "Platform/FileSystem/AtomicFileWriter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace EdgePulse::Agent
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;
        using AsynGyanis::Core::EventLoop;
        using AsynGyanis::Core::Task;

        /**
         * @brief 用例结束时摘掉根日志器的全部 Sink
         * @details 文件 Sink 在 Windows 上一直持有日志文件，不摘掉会拦住临时目录清理。
         */
        class LoggingSinkReset
        {
        public:
            ~LoggingSinkReset()
            {
                AsynGyanis::Base::LoggerRegistry::instance().getRootLogger().clearSinks();
            }
        };

        /**
         * @brief 拼出指向回环服务器的明文测试配置
         * @param port 服务端端口
         * @return AgentConfiguration 已允许明文的配置
         */
        [[nodiscard]] AgentConfiguration plaintextConfiguration(const std::uint16_t port)
        {
            AgentConfiguration configuration;
            configuration.baseUrl                = "http://127.0.0.1:" + std::to_string(port);
            configuration.allowPlaintextForTests = true;
            configuration.name                   = "edge-01";
            configuration.tenantCode             = "tenant-a";
            configuration.requestTimeoutSeconds  = 5;
            return configuration;
        }

        /**
         * @brief 测试身份：指纹固定，不依赖真实证书文件
         * @return AgentIdentity 身份
         */
        [[nodiscard]] AgentIdentity testIdentity()
        {
            AgentIdentity identity;
            identity.fingerprint = std::string(64, 'a');
            identity.expiresAt   = "2030-01-01T00:00:00Z";
            return identity;
        }

        /**
         * @brief 统计调用次数并交回固定成功的执行器
         */
        class FakeExecutor final : public CommandExecutor
        {
        public:
            explicit FakeExecutor(std::string type) : m_type(std::move(type))
            {
            }

            bool supports(const std::string_view type) const override
            {
                return type == m_type;
            }

            Task<CommandResult> execute(EventLoop &, const AgentCommand &) override
            {
                m_executions.fetch_add(1, std::memory_order_relaxed);
                co_return CommandResult{true, R"({"ok":true})", "执行完成"};
            }

            [[nodiscard]] int executions() const
            {
                return m_executions.load(std::memory_order_acquire);
            }

        private:
            std::string      m_type;          ///< 支持的命令类型
            std::atomic<int> m_executions{0}; ///< 执行次数
        };

        /**
         * @brief 在阻塞执行器上等待一个承诺，避免占住事件循环
         * @param loop 协程所属事件循环
         * @param gate 待完成的闸门
         * @return Task<> 闸门完成后返回
         */
        Task<> waitForGate(EventLoop &loop, std::shared_ptr<std::promise<void>> gate)
        {
            std::shared_future<void> future = gate->get_future().share();
            static_cast<void>(co_await AsynGyanis::Core::AsyncExecutor::shared().submit<bool>(loop,
                                                                                              [future = std::move(future)]() mutable
                                                                                              {
                                                                                                  future.wait();
                                                                                                  return true;
                                                                                              }));
            co_return;
        }

        /**
         * @brief 两条命令共用的执行器：慢命令在闸门处挂起，快命令记录是否与慢命令重叠
         * @details 用原子计数观测「同一时刻有多少条命令在执行」。只要第二条在第一条释放前开始，
         *          active 就会到 2，测试无需依赖睡眠时长来猜时序。
         */
        class GatedExecutor final : public CommandExecutor
        {
        public:
            /**
             * @brief 绑定慢命令闸门与启动信号
             * @param slowGate 慢命令等待的闸门；测试完成承诺前一直挂起
             * @param slowStarted 慢命令开始执行时完成
             * @param fastDone 快命令执行完成时完成
             */
            GatedExecutor(std::shared_ptr<std::promise<void>> slowGate, std::shared_ptr<std::promise<void>> slowStarted, std::shared_ptr<std::promise<void>> fastDone) :
                m_slowGate(std::move(slowGate)), m_slowStarted(std::move(slowStarted)), m_fastDone(std::move(fastDone))
            {
            }

            bool supports(const std::string_view type) const override
            {
                return type == "fake.slow" || type == "fake.fast";
            }

            Task<CommandResult> execute(EventLoop &loop, const AgentCommand &command) override
            {
                const int active = m_active.fetch_add(1, std::memory_order_acq_rel) + 1;
                int       peak   = m_peak.load(std::memory_order_acquire);
                while (active > peak && !m_peak.compare_exchange_weak(peak, active, std::memory_order_acq_rel))
                {
                }
                if (command.type == "fake.slow")
                {
                    m_slowStartOrder.store(m_sequence.fetch_add(1, std::memory_order_acq_rel), std::memory_order_release);
                    m_slowStarted->set_value();
                    // 闸门等在阻塞执行器上：慢命令挂起期间事件循环仍能推进轮询与 WebSocket 接收
                    co_await waitForGate(loop, m_slowGate);
                    m_slowFinished.store(true, std::memory_order_release);
                    m_slowEndOrder.store(m_sequence.fetch_add(1, std::memory_order_acq_rel), std::memory_order_release);
                } else
                {
                    m_fastStartedWhileSlow.store(!m_slowFinished.load(std::memory_order_acquire), std::memory_order_release);
                    m_fastStartOrder.store(m_sequence.fetch_add(1, std::memory_order_acq_rel), std::memory_order_release);
                    try
                    {
                        m_fastDone->set_value();
                    } catch (...)
                    {
                    }
                }
                m_active.fetch_sub(1, std::memory_order_acq_rel);
                co_return CommandResult{true, "{}", "执行完成"};
            }

            [[nodiscard]] int peakActive() const
            {
                return m_peak.load(std::memory_order_acquire);
            }

            [[nodiscard]] bool fastStarted() const
            {
                return m_fastStartOrder.load(std::memory_order_acquire) >= 0;
            }

            [[nodiscard]] bool fastStartedWhileSlow() const
            {
                return m_fastStartedWhileSlow.load(std::memory_order_acquire);
            }

            [[nodiscard]] int slowStartOrder() const
            {
                return m_slowStartOrder.load(std::memory_order_acquire);
            }

            [[nodiscard]] int slowEndOrder() const
            {
                return m_slowEndOrder.load(std::memory_order_acquire);
            }

            [[nodiscard]] int fastStartOrder() const
            {
                return m_fastStartOrder.load(std::memory_order_acquire);
            }

        private:
            std::shared_ptr<std::promise<void>> m_slowGate;                    ///< 慢命令等待的闸门
            std::shared_ptr<std::promise<void>> m_slowStarted;                 ///< 慢命令已开始
            std::shared_ptr<std::promise<void>> m_fastDone;                    ///< 快命令已完成
            std::atomic<int>                    m_active{0};                   ///< 当前执行中的命令数
            std::atomic<int>                    m_peak{0};                     ///< 观测到的最大并发数
            std::atomic<int>                    m_sequence{0};                 ///< 开始/结束事件的全局顺序
            std::atomic<int>                    m_slowStartOrder{-1};          ///< 慢命令开始序号
            std::atomic<int>                    m_slowEndOrder{-1};            ///< 慢命令结束序号
            std::atomic<int>                    m_fastStartOrder{-1};          ///< 快命令开始序号
            std::atomic<bool>                   m_slowFinished{false};         ///< 慢命令是否已结束
            std::atomic<bool>                   m_fastStartedWhileSlow{false}; ///< 快命令开始时慢命令是否仍在执行
        };

        /**
         * @brief 命令结果上报的捕获点
         */
        struct ResultCapture
        {
            std::mutex               mutex;       ///< 保护 bodies 与 paths
            std::vector<std::string> bodies;      ///< 收到的请求正文
            std::vector<std::string> paths;       ///< 收到的请求路径
            std::atomic<int>         status{200}; ///< 下一次响应的状态码
            std::atomic<int>         posts{0};    ///< 已收到的上报次数

            /**
             * @brief 记录一次上报
             * @param path 请求路径
             * @param body 请求正文
             */
            void record(std::string path, std::string body)
            {
                const std::lock_guard lock(mutex);
                paths.push_back(std::move(path));
                bodies.push_back(std::move(body));
                posts.fetch_add(1, std::memory_order_release);
            }

            /**
             * @brief 取正文副本
             * @param index 序号
             * @return std::string 正文
             */
            [[nodiscard]] std::string bodyAt(const std::size_t index)
            {
                const std::lock_guard lock(mutex);
                return index < bodies.size() ? bodies[index] : std::string{};
            }

            /**
             * @brief 取路径副本
             * @param index 序号
             * @return std::string 路径
             */
            [[nodiscard]] std::string pathAt(const std::size_t index)
            {
                const std::lock_guard lock(mutex);
                return index < paths.size() ? paths[index] : std::string{};
            }
        };

        /**
         * @brief 安装命令结果上报路由
         * @param router 目标路由器
         * @param capture 捕获点
         */
        void installResultRoute(AsynGyanis::Net::Router &router, const std::shared_ptr<ResultCapture> &capture)
        {
            router.post("/api/agent/commands/:id/result",
                        [capture](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> Task<>
                        {
                            capture->record(std::string{request.path()}, std::string{request.body()});
                            const int status = capture->status.load(std::memory_order_acquire);
                            response.setStatus(status);
                            response.setBody(status == 200 ? R"({"code":200,"message":"ok","data":null,"traceId":"trace-result"})"
                                                           : R"({"code":500,"message":"暂时失败","data":null,"traceId":"trace-result"})");
                            co_return;
                        });
        }

        /**
         * @brief 安装返回固定命令的拉取路由
         * @param router 目标路由器
         * @param command 命令对象
         */
        void installNextRoute(AsynGyanis::Net::Router &router, ConfigValue command)
        {
            router.get("/api/agent/commands/next",
                       [command = std::move(command)](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> Task<>
                       {
                           const ConfigValue envelope = ConfigValue::object({{"code", 200}, {"message", "ok"}, {"data", command}, {"traceId", "trace-next"}});
                           response.setStatus(200);
                           response.setBody(envelope.dump());
                           co_return;
                       });
        }

        /**
         * @brief 开放一个临时状态库并写入节点标识
         * @param directory 临时目录
         * @return 已就绪的状态库
         */
        [[nodiscard]] std::expected<AgentState, std::string> openState(TestSupport::TemporaryDirectory &directory)
        {
            auto state = AgentState::open(directory.path() / "state" / "agent.db");
            if (state.has_value())
            {
                state->set("node_id", "77");
            }
            return state;
        }

        /**
         * @brief 在有时限的轮询里等待条件成立
         * @param timeout 等待上限
         * @param predicate 条件
         * @return 条件在时限内成立返回 true
         */
        [[nodiscard]] bool waitFor(const std::chrono::seconds timeout, const std::function<bool()> &predicate)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (predicate())
                {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
            }
            return predicate();
        }
    } // namespace

    TEST(AgentCommand, HeartbeatParsesDesiredVersionAndInterval)
    {
        struct HeartbeatCapture
        {
            std::mutex  mutex; ///< 保护 body
            std::string body;  ///< 心跳请求正文
        };
        auto capture = std::make_shared<HeartbeatCapture>();

        TestSupport::ServerHarness server(
                [capture](AsynGyanis::Net::Router &router, EventLoop &)
                {
                    router.post("/api/agent/heartbeat",
                                [capture](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> Task<>
                                {
                                    {
                                        const std::lock_guard lock(capture->mutex);
                                        capture->body = std::string{request.body()};
                                    }
                                    response.setStatus(200);
                                    response.setBody(
                                            R"({"code":200,"message":"ok","data":{"id":77,"desiredConfigVersion":12,"heartbeatIntervalSeconds":42},"traceId":"trace-heartbeat"})");
                                    co_return;
                                });
                });

        AgentLoop     loop;
        AgentClient   client(loop.loop(), plaintextConfiguration(server.port()), testIdentity());
        LoopStopGuard stopGuard{loop};
        const auto    outcome = loop.run(client.heartbeat(77, "1.2.3"), std::chrono::seconds{15}, "心跳");
        ASSERT_TRUE(outcome.has_value());
        ASSERT_TRUE(outcome->has_value()) << outcome->error();
        EXPECT_EQ(12, (*outcome)->desiredConfigVersion);
        EXPECT_EQ(42, (*outcome)->heartbeatIntervalSeconds);

        std::string body;
        {
            const std::lock_guard lock(capture->mutex);
            body = capture->body;
        }
        const auto sent = AsynGyanis::Base::parseConfigValue(body);
        ASSERT_TRUE(sent.has_value()) << body;
        EXPECT_EQ(77, sent->value("nodeId", 0));
        EXPECT_EQ("1.2.3", sent->value("agentVersion", std::string{}));
    }

    TEST(AgentCommand, PollerExecutesOnceAndAcksResult)
    {
        ConfigValue command = ConfigValue::object({{"id", 501}, {"nodeId", 77}, {"type", "fake.echo"}, {"payload", R"({"value":1})"}, {"timeoutSeconds", 30}});
        auto        capture = std::make_shared<ResultCapture>();

        TestSupport::ServerHarness server(
                [command, capture](AsynGyanis::Net::Router &router, EventLoop &)
                {
                    installNextRoute(router, command);
                    installResultRoute(router, capture);
                });

        TestSupport::TemporaryDirectory directory;
        auto                            state = openState(directory);
        ASSERT_TRUE(state.has_value()) << state.error();

        AgentLoop         loop;
        AgentClient       client(loop.loop(), plaintextConfiguration(server.port()), testIdentity());
        auto              executor = std::make_shared<FakeExecutor>("fake.echo");
        CommandDispatcher dispatcher(client, *state, {executor});
        LoopStopGuard     stopGuard{loop};

        ASSERT_TRUE(loop.run(dispatcher.pollOnce(), std::chrono::seconds{15}, "轮询命令"));
        EXPECT_EQ(1, executor->executions());
        ASSERT_EQ(1, capture->posts.load(std::memory_order_acquire));

        const auto posted = AsynGyanis::Base::parseConfigValue(capture->bodyAt(0));
        ASSERT_TRUE(posted.has_value()) << capture->bodyAt(0);
        EXPECT_EQ(77, posted->value("nodeId", 0));
        EXPECT_EQ("succeeded", posted->value("status", std::string{}));
        EXPECT_EQ(R"({"ok":true})", posted->value("result", std::string{}));
        EXPECT_EQ("执行完成", posted->value("message", std::string{}));
        EXPECT_EQ("/api/agent/commands/501/result", capture->pathAt(0));
        EXPECT_TRUE(state->takeResults(10).empty()) << "服务端确认后队列行应已删除";
    }

    TEST(AgentCommand, DuplicateDeliveryExecutesExactlyOnce)
    {
        auto capture = std::make_shared<ResultCapture>();
        capture->status.store(500, std::memory_order_release);

        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, EventLoop &) { installResultRoute(router, capture); });

        TestSupport::TemporaryDirectory directory;
        auto                            state = openState(directory);
        ASSERT_TRUE(state.has_value()) << state.error();

        AgentLoop          loop;
        AgentClient        client(loop.loop(), plaintextConfiguration(server.port()), testIdentity());
        auto               executor = std::make_shared<FakeExecutor>("fake.echo");
        CommandDispatcher  dispatcher(client, *state, {executor});
        const AgentCommand command{601, "fake.echo", R"({"value":2})", 30};
        LoopStopGuard      stopGuard{loop};

        ASSERT_TRUE(loop.run(dispatcher.onWebSocketCommand(command), std::chrono::seconds{15}, "首次投递"));
        ASSERT_TRUE(loop.run(dispatcher.onWebSocketCommand(command), std::chrono::seconds{15}, "重复投递"));

        EXPECT_EQ(1, executor->executions());
        EXPECT_EQ(1, capture->posts.load(std::memory_order_acquire));
        EXPECT_EQ(1U, state->takeResults(10).size()) << "未获确认的结果行应保留";
    }

    TEST(AgentCommand, FailedResultPostKeepsRowUntilConfirmed)
    {
        auto capture = std::make_shared<ResultCapture>();
        capture->status.store(500, std::memory_order_release);

        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, EventLoop &) { installResultRoute(router, capture); });

        TestSupport::TemporaryDirectory directory;
        auto                            state = openState(directory);
        ASSERT_TRUE(state.has_value()) << state.error();
        state->enqueueResult(701, ConfigValue::object({{"status", "succeeded"}, {"result", R"({"kept":true})"}, {"message", "第一次"}}).dump());

        AgentLoop         loop;
        AgentClient       client(loop.loop(), plaintextConfiguration(server.port()), testIdentity());
        CommandDispatcher dispatcher(client, *state, {});
        LoopStopGuard     stopGuard{loop};

        const auto first = loop.run(dispatcher.flushResults(), std::chrono::seconds{15}, "首次上报");
        ASSERT_TRUE(first.has_value());
        EXPECT_TRUE(first->failed);
        EXPECT_EQ(0U, first->delivered);
        EXPECT_EQ(1U, state->takeResults(10).size()) << "首次失败后结果行必须保留";

        capture->status.store(200, std::memory_order_release);
        const auto second = loop.run(dispatcher.flushResults(), std::chrono::seconds{15}, "第二次上报");
        ASSERT_TRUE(second.has_value());
        EXPECT_FALSE(second->failed);
        EXPECT_EQ(1U, second->delivered);
        EXPECT_TRUE(state->takeResults(10).empty());
        EXPECT_EQ(2, capture->posts.load(std::memory_order_acquire));
    }

    TEST(AgentCommand, TerminalResultStatusDropsRowAndContinues)
    {
        struct Capture
        {
            std::mutex               mutex;    ///< 保护 paths
            std::vector<std::string> paths;    ///< 收到的结果上报路径
            std::atomic<int>         posts{0}; ///< 已收到的上报次数
        };
        auto capture = std::make_shared<Capture>();

        TestSupport::ServerHarness server(
                [capture](AsynGyanis::Net::Router &router, EventLoop &)
                {
                    router.post("/api/agent/commands/:id/result",
                                [capture](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> Task<>
                                {
                                    const std::string path{request.path()};
                                    {
                                        const std::lock_guard lock(capture->mutex);
                                        capture->paths.push_back(path);
                                    }
                                    capture->posts.fetch_add(1, std::memory_order_release);
                                    if (path.find("/1201/") != std::string::npos)
                                    {
                                        response.setStatus(409);
                                        response.setBody(R"({"code":409,"message":"命令已结束","data":null,"traceId":"trace-result"})");
                                    } else
                                    {
                                        response.setStatus(200);
                                        response.setBody(R"({"code":200,"message":"ok","data":null,"traceId":"trace-result"})");
                                    }
                                    co_return;
                                });
                });

        TestSupport::TemporaryDirectory directory;
        auto                            state = openState(directory);
        ASSERT_TRUE(state.has_value()) << state.error();
        state->enqueueResult(1201, ConfigValue::object({{"status", "succeeded"}, {"result", "{}"}, {"message", "已结束的命令"}}).dump());
        state->enqueueResult(1202, ConfigValue::object({{"status", "succeeded"}, {"result", "{}"}, {"message", "后续命令"}}).dump());

        AgentLoop         loop;
        AgentClient       client(loop.loop(), plaintextConfiguration(server.port()), testIdentity());
        CommandDispatcher dispatcher(client, *state, {});
        LoopStopGuard     stopGuard{loop};

        const auto report = loop.run(dispatcher.flushResults(), std::chrono::seconds{15}, "终态结果上报");
        ASSERT_TRUE(report.has_value());
        EXPECT_EQ(2, capture->posts.load(std::memory_order_acquire)) << "队首被拒绝后仍应继续投递下一条结果";
        EXPECT_EQ(1U, report->delivered);
        EXPECT_FALSE(report->failed);
        EXPECT_TRUE(state->takeResults(10).empty()) << "终态拒绝与成功确认的行都应从队列删除";

        std::vector<std::string> paths;
        {
            const std::lock_guard lock(capture->mutex);
            paths = capture->paths;
        }
        ASSERT_EQ(2U, paths.size());
        EXPECT_NE(std::string::npos, paths[0].find("/1201/"));
        EXPECT_NE(std::string::npos, paths[1].find("/1202/"));
    }

    TEST(AgentCommand, WebSocketFallbackPollsUntilChannelConnected)
    {
        WebSocketFallbackState fallback;
        EXPECT_TRUE(fallback.shouldPoll()) << "启动时通道尚未确认，必须立即轮询";
        EXPECT_EQ(0, fallback.consecutiveFailures());

        fallback.recordConnectFailure();
        EXPECT_TRUE(fallback.shouldPoll()) << "连接失败期间轮询必须继续";
        EXPECT_EQ(1, fallback.consecutiveFailures());

        fallback.recordConnectFailure();
        EXPECT_TRUE(fallback.shouldPoll());
        EXPECT_EQ(2, fallback.consecutiveFailures());

        fallback.recordConnected();
        EXPECT_FALSE(fallback.shouldPoll()) << "只有 hello 确认的连接会暂停轮询";
        EXPECT_EQ(0, fallback.consecutiveFailures());
    }

    TEST(AgentCommand, WebSocketFallbackResumesPollingAfterNormalDisconnect)
    {
        WebSocketFallbackState fallback;
        fallback.recordConnected();
        ASSERT_FALSE(fallback.shouldPoll());

        fallback.recordDisconnected();
        EXPECT_TRUE(fallback.shouldPoll()) << "正常断开后轮询必须立即恢复，不能等连续失败三次";
        EXPECT_EQ(0, fallback.consecutiveFailures());
    }

    TEST(AgentCommand, WebSocketFallbackFailureCountOnlyDrivesBackoff)
    {
        WebSocketFallbackState fallback;
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            fallback.recordConnectFailure();
            EXPECT_TRUE(fallback.shouldPoll()) << "失败次数不改变轮询判定";
        }
        EXPECT_EQ(5, fallback.consecutiveFailures());
    }

    TEST(AgentCommand, WebSocketFallbackBackoffTable)
    {
        EXPECT_EQ(1, webSocketReconnectBackoffSeconds(0));
        EXPECT_EQ(1, webSocketReconnectBackoffSeconds(1));
        EXPECT_EQ(2, webSocketReconnectBackoffSeconds(2));
        EXPECT_EQ(5, webSocketReconnectBackoffSeconds(3));
        EXPECT_EQ(15, webSocketReconnectBackoffSeconds(4));
        EXPECT_EQ(30, webSocketReconnectBackoffSeconds(5));
        EXPECT_EQ(60, webSocketReconnectBackoffSeconds(6));
        EXPECT_EQ(60, webSocketReconnectBackoffSeconds(7));
        EXPECT_EQ(60, webSocketReconnectBackoffSeconds(100));
    }

    TEST(AgentCommand, WebSocketFallbackForcedPollWhileConnected)
    {
        // 时钟由用例推进：断言自己造出「到点」与「未到点」两种状态，不靠真实调度时序
        std::chrono::steady_clock::time_point now{};
        WebSocketFallbackState                fallback([&now] { return now; });
        const auto                            configuredInterval = std::chrono::seconds{15};
        const auto                            connectedInterval  = std::chrono::seconds{60};
        const auto                            oneSecond          = std::chrono::seconds{1};

        EXPECT_TRUE(fallback.shouldPoll()) << "启动时通道未确认，轮询必须运行";
        fallback.recordConnected();
        EXPECT_TRUE(fallback.isConnected());
        EXPECT_FALSE(fallback.shouldPoll()) << "连接确认后暂停连续轮询";
        EXPECT_EQ(connectedInterval, fallback.nextPollDelay(configuredInterval));

        now += std::chrono::seconds{59};
        EXPECT_FALSE(fallback.shouldPoll()) << "连接期间 60 秒未到不得强制轮询";
        EXPECT_EQ(oneSecond, fallback.nextPollDelay(configuredInterval));

        now += std::chrono::seconds{1};
        EXPECT_TRUE(fallback.shouldPoll()) << "连接期间每 60 秒必须强制轮询一次";
        EXPECT_FALSE(fallback.shouldPoll()) << "同一次到点只消费一个轮询时隙";
        EXPECT_EQ(connectedInterval, fallback.nextPollDelay(configuredInterval));

        now += std::chrono::seconds{60};
        EXPECT_TRUE(fallback.shouldPoll());

        fallback.recordDisconnected();
        EXPECT_FALSE(fallback.isConnected());
        EXPECT_TRUE(fallback.shouldPoll()) << "断开后立即恢复轮询";
        EXPECT_EQ(configuredInterval, fallback.nextPollDelay(configuredInterval));
        EXPECT_EQ(configuredInterval, fallback.nextPollDelay(std::chrono::seconds{0})) << "非法配置退回 15 秒";
    }

    TEST(AgentCommand, WebSocketCommandExecutesOnceAndPostsResult)
    {
        auto capture      = std::make_shared<ResultCapture>();
        auto helloCapture = std::make_shared<std::string>();
        auto helloMutex   = std::make_shared<std::mutex>();

        TestSupport::ServerHarness server(
                [capture, helloCapture, helloMutex](AsynGyanis::Net::Router &router, EventLoop &)
                {
                    installResultRoute(router, capture);
                    router.get("/api/agent/ws",
                               [capture, helloCapture, helloMutex](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> Task<>
                               {
                                   AsynGyanis::Net::WebSocketUpgradePolicy policy;
                                   policy.originMode = AsynGyanis::Net::WebSocketOriginMode::AllowAny;
                                   response.upgradeToWebSocket(
                                           [capture, helloCapture, helloMutex](AsynGyanis::Net::WebSocketPeer &peer) -> Task<>
                                           {
                                               const auto hello = co_await peer.receive();
                                               if (!hello.has_value())
                                               {
                                                   co_return;
                                               }
                                               {
                                                   const std::lock_guard lock(*helloMutex);
                                                   *helloCapture = hello->payload;
                                               }
                                               static_cast<void>(co_await peer.sendText(R"({"type":"hello","ok":true})"));
                                               static_cast<void>(co_await peer.sendText(
                                                       R"({"type":"command","command":{"id":801,"nodeId":77,"type":"fake.echo","payload":"{\"value\":3}","timeoutSeconds":30}})"));
                                               static_cast<void>(co_await peer.close(1000, "done"));
                                               co_return;
                                           },
                                           std::move(policy));
                                   co_return;
                               });
                });

        TestSupport::TemporaryDirectory directory;
        auto                            state = openState(directory);
        ASSERT_TRUE(state.has_value()) << state.error();

        AgentLoop         loop;
        AgentClient       client(loop.loop(), plaintextConfiguration(server.port()), testIdentity());
        auto              executor = std::make_shared<FakeExecutor>("fake.echo");
        CommandDispatcher dispatcher(client, *state, {executor});
        AgentWebSocket    socket(loop.loop(), plaintextConfiguration(server.port()));
        LoopStopGuard     stopGuard{loop};

        std::atomic<bool> established{false};
        const auto        outcome = loop.run(socket.serve(dispatcher, 77, "1.2.3",
                                                          [&established](const bool connected)
                                                          {
                                                       if (connected)
                                                       {
                                                           established.store(true, std::memory_order_release);
                                                       }
                                                          }),
                                             std::chrono::seconds{20}, "WebSocket 会话");
        ASSERT_TRUE(outcome.has_value());
        EXPECT_TRUE(outcome->has_value()) << outcome->error();
        EXPECT_TRUE(established.load(std::memory_order_acquire));

        EXPECT_EQ(1, executor->executions());
        ASSERT_EQ(1, capture->posts.load(std::memory_order_acquire));
        const auto posted = AsynGyanis::Base::parseConfigValue(capture->bodyAt(0));
        ASSERT_TRUE(posted.has_value()) << capture->bodyAt(0);
        EXPECT_EQ(77, posted->value("nodeId", 0));
        EXPECT_EQ("succeeded", posted->value("status", std::string{}));

        std::string helloPayload;
        {
            const std::lock_guard lock(*helloMutex);
            helloPayload = *helloCapture;
        }
        const auto hello = AsynGyanis::Base::parseConfigValue(helloPayload);
        ASSERT_TRUE(hello.has_value()) << helloPayload;
        EXPECT_EQ("hello", hello->value("type", std::string{}));
        EXPECT_EQ(77, hello->value("nodeId", 0));
    }

    TEST(AgentCommand, WebSocketCommandDeduplicatesAcrossOverlappingDeliveries)
    {
        auto                       capture = std::make_shared<ResultCapture>();
        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, EventLoop &) { installResultRoute(router, capture); });

        TestSupport::TemporaryDirectory directory;
        auto                            state = openState(directory);
        ASSERT_TRUE(state.has_value()) << state.error();

        AgentLoop         loop;
        AgentClient       client(loop.loop(), plaintextConfiguration(server.port()), testIdentity());
        auto              executor = std::make_shared<FakeExecutor>("fake.echo");
        CommandDispatcher dispatcher(client, *state, {executor});
        LoopStopGuard     stopGuard{loop};

        // 轮询兜底与 WebSocket 推送可能各交来一份同标识命令；第一次的结果已被服务端确认并删除，
        // 第二次仍必须按已执行命令丢弃，不能靠结果队列里恰好还有一行来挡
        ASSERT_TRUE(loop.run(dispatcher.onWebSocketCommand(AgentCommand{1501, "fake.echo", "{}", 30}), std::chrono::seconds{15}, "首次投递"));
        ASSERT_TRUE(loop.run(dispatcher.onWebSocketCommand(AgentCommand{1501, "fake.echo", "{}", 30}), std::chrono::seconds{15}, "重复投递"));

        EXPECT_EQ(1, executor->executions()) << "同一命令标识只能执行一次";
        EXPECT_EQ(1, capture->posts.load(std::memory_order_acquire)) << "重复投递不能重复上报结果";
    }

    TEST(AgentCommand, PolledCommandWaitsForRunningWebSocketCommand)
    {
        auto capture = std::make_shared<ResultCapture>();

        // 三个闸门分别卡住轮询响应、WebSocket 推送与关闭；慢命令的闸门在 Agent 循环上等
        auto pushGate      = std::make_shared<std::promise<void>>();
        auto nextGate      = std::make_shared<std::promise<void>>();
        auto closeGate     = std::make_shared<std::promise<void>>();
        auto slowGate      = std::make_shared<std::promise<void>>();
        auto slowStarted   = std::make_shared<std::promise<void>>();
        auto fastDone      = std::make_shared<std::promise<void>>();
        auto nextRequested = std::make_shared<std::promise<void>>();
        auto connected     = std::make_shared<std::promise<void>>();
        auto pollDone      = std::make_shared<std::promise<void>>();

        const AsynGyanis::Base::ConfigValue fastCommand =
                AsynGyanis::Base::ConfigValue::object({{"id", 1602}, {"nodeId", 77}, {"type", "fake.fast"}, {"payload", "{}"}, {"timeoutSeconds", 30}});

        TestSupport::ServerHarness server(
                [capture, pushGate, nextGate, closeGate, nextRequested, fastCommand](AsynGyanis::Net::Router &router, EventLoop &loop)
                {
                    installResultRoute(router, capture);
                    router.get("/api/agent/ws",
                               [&loop, pushGate, closeGate](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> Task<>
                               {
                                   AsynGyanis::Net::WebSocketUpgradePolicy policy;
                                   policy.originMode = AsynGyanis::Net::WebSocketOriginMode::AllowAny;
                                   response.upgradeToWebSocket(
                                           [&loop, pushGate, closeGate](AsynGyanis::Net::WebSocketPeer &peer) -> Task<>
                                           {
                                               const auto hello = co_await peer.receive();
                                               if (!hello.has_value() || !co_await peer.sendText(R"({"type":"hello","ok":true})"))
                                               {
                                                   co_return;
                                               }
                                               // 轮询先拿到响应之前不推送慢命令，测试因此不需要靠 sleep 抢时序
                                               co_await waitForGate(loop, pushGate);
                                               if (!co_await peer.sendText(
                                                           R"({"type":"command","command":{"id":1601,"nodeId":77,"type":"fake.slow","payload":"{}","timeoutSeconds":30}})"))
                                               {
                                                   co_return;
                                               }
                                               co_await waitForGate(loop, closeGate);
                                               static_cast<void>(co_await peer.close(1000, "done"));
                                               co_return;
                                           },
                                           std::move(policy));
                                   co_return;
                               });
                    router.get("/api/agent/commands/next",
                               [&loop, nextGate, nextRequested, fastCommand](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> Task<>
                               {
                                   try
                                   {
                                       nextRequested->set_value();
                                   } catch (...)
                                   {
                                   }
                                   co_await waitForGate(loop, nextGate);
                                   const AsynGyanis::Base::ConfigValue envelope =
                                           AsynGyanis::Base::ConfigValue::object({{"code", 200}, {"message", "ok"}, {"data", fastCommand}, {"traceId", "trace-next"}});
                                   response.setStatus(200);
                                   response.setBody(envelope.dump());
                                   co_return;
                               });
                });

        TestSupport::TemporaryDirectory directory;
        auto                            state = openState(directory);
        ASSERT_TRUE(state.has_value()) << state.error();

        AgentLoop         loop;
        AgentClient       client(loop.loop(), plaintextConfiguration(server.port()), testIdentity());
        auto              executor = std::make_shared<GatedExecutor>(slowGate, slowStarted, fastDone);
        CommandDispatcher dispatcher(client, *state, {executor});
        AgentWebSocket    socket(loop.loop(), plaintextConfiguration(server.port()));
        LoopStopGuard     stopGuard{loop};

        struct GateReleaser
        {
            std::vector<std::shared_ptr<std::promise<void>>> gates; ///< 用例提前失败时也要放行的闸门

            ~GateReleaser()
            {
                for (const auto &gate: gates)
                {
                    try
                    {
                        gate->set_value();
                    } catch (...)
                    {
                    }
                }
            }
        } releaser{{pushGate, nextGate, closeGate, slowGate}};

        // 观察者必须是具名左值：serve 的形参是 const 引用，临时 lambda 活不过首次恢复
        const AgentWebSocket::ConnectionObserver observer = [connected](const bool isConnected)
        {
            if (isConnected)
            {
                try
                {
                    connected->set_value();
                } catch (...)
                {
                }
            }
        };
        auto       serveDone  = std::make_shared<std::promise<void>>();
        const auto serveFrame = std::make_shared<Task<>>(
                [](Task<std::expected<void, std::string>> operation, std::shared_ptr<std::promise<void>> done) -> Task<>
                {
                    try
                    {
                        static_cast<void>(co_await std::move(operation));
                    } catch (...)
                    {
                    }
                    try
                    {
                        done->set_value();
                    } catch (...)
                    {
                    }
                    co_return;
                }(socket.serve(dispatcher, 77, "1.2.3", observer), serveDone));
        loop.loop().scheduler().scheduleRemote(serveFrame->handle());
        ASSERT_EQ(std::future_status::ready, connected->get_future().wait_for(std::chrono::seconds{10})) << "WebSocket 未建立";

        const auto pollFrame = std::make_shared<Task<>>(
                [](Task<> operation, std::shared_ptr<std::promise<void>> done) -> Task<>
                {
                    try
                    {
                        co_await std::move(operation);
                    } catch (...)
                    {
                    }
                    try
                    {
                        done->set_value();
                    } catch (...)
                    {
                    }
                    co_return;
                }(dispatcher.pollOnce(), pollDone));
        loop.loop().scheduler().scheduleRemote(pollFrame->handle());
        ASSERT_EQ(std::future_status::ready, nextRequested->get_future().wait_for(std::chrono::seconds{10})) << "轮询请求未到达服务端";

        // 轮询响应还卡着时启动慢命令；随后放开轮询响应，取回的快命令必须排队
        server.loop().scheduler().postRemote([pushGate] { pushGate->set_value(); });
        ASSERT_EQ(std::future_status::ready, slowStarted->get_future().wait_for(std::chrono::seconds{10})) << "慢命令没有开始执行";
        server.loop().scheduler().postRemote([nextGate] { nextGate->set_value(); });
        ASSERT_EQ(std::future_status::ready, pollDone->get_future().wait_for(std::chrono::seconds{10})) << "轮询没有完成";

        EXPECT_TRUE(dispatcher.hasInFlight());
        EXPECT_FALSE(executor->fastStarted()) << "慢命令未结束时不能开始执行快命令";
        EXPECT_EQ(1, executor->peakActive()) << "同一时刻只能有一条命令在执行";

        loop.loop().scheduler().postRemote([slowGate] { slowGate->set_value(); });
        ASSERT_EQ(std::future_status::ready, fastDone->get_future().wait_for(std::chrono::seconds{10})) << "排队命令没有在慢命令结束后执行";

        EXPECT_TRUE(executor->fastStarted());
        EXPECT_FALSE(executor->fastStartedWhileSlow()) << "快命令开始时慢命令必须已经结束";
        EXPECT_EQ(1, executor->peakActive()) << "两条命令不能并发执行";
        EXPECT_LT(executor->slowStartOrder(), executor->slowEndOrder());
        EXPECT_LT(executor->slowEndOrder(), executor->fastStartOrder()) << "快命令必须在慢命令之后开始";

        // 正常关闭 WebSocket，让 serve 帧在用例结束前收口，避免析构悬挂的客户端协程
        server.loop().scheduler().postRemote([closeGate] { closeGate->set_value(); });
        EXPECT_EQ(std::future_status::ready, serveDone->get_future().wait_for(std::chrono::seconds{5}));
    }

    TEST(AgentCommand, RestartRunsAfterResultAcknowledged)
    {
        auto capture = std::make_shared<ResultCapture>();

        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, EventLoop &) { installResultRoute(router, capture); });

        TestSupport::TemporaryDirectory directory;
        auto                            state = openState(directory);
        ASSERT_TRUE(state.has_value()) << state.error();

        AgentLoop   loop;
        AgentClient client(loop.loop(), plaintextConfiguration(server.port()), testIdentity());

        std::atomic<bool> handlerRan{false};
        std::atomic<bool> rowGoneAtHandler{false};
        std::atomic<int>  postsAtHandler{0};
        auto              restartExecutor = std::make_shared<RestartExecutor>(true);
        CommandDispatcher dispatcher(client, *state, {restartExecutor},
                                     [&handlerRan, &rowGoneAtHandler, &postsAtHandler, &state, &capture]() -> Task<>
                                     {
                                         handlerRan.store(true, std::memory_order_release);
                                         postsAtHandler.store(capture->posts.load(std::memory_order_acquire), std::memory_order_release);
                                         rowGoneAtHandler.store(!state->hasResult(1401), std::memory_order_release);
                                         co_return;
                                     });
        LoopStopGuard     stopGuard{loop};

        ASSERT_TRUE(loop.run(dispatcher.onWebSocketCommand(AgentCommand{1401, "agent.restart", "{}", 30}), std::chrono::seconds{15}, "重启命令分发"));

        ASSERT_EQ(1, capture->posts.load(std::memory_order_acquire));
        const auto posted = AsynGyanis::Base::parseConfigValue(capture->bodyAt(0));
        ASSERT_TRUE(posted.has_value()) << capture->bodyAt(0);
        EXPECT_EQ("succeeded", posted->value("status", std::string{}));
        EXPECT_TRUE(handlerRan.load(std::memory_order_acquire)) << "重启处理器必须被调用";
        EXPECT_EQ(1, postsAtHandler.load(std::memory_order_acquire)) << "重启必须发生在结果上报尝试之后";
        EXPECT_TRUE(rowGoneAtHandler.load(std::memory_order_acquire)) << "服务端已确认的结果行必须在重启前删除";
    }

    TEST(AgentCommand, RestartStillRunsWhenResultCannotBeDelivered)
    {
        auto capture = std::make_shared<ResultCapture>();
        capture->status.store(500, std::memory_order_release);

        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, EventLoop &) { installResultRoute(router, capture); });

        TestSupport::TemporaryDirectory directory;
        auto                            state = openState(directory);
        ASSERT_TRUE(state.has_value()) << state.error();

        AgentLoop         loop;
        AgentClient       client(loop.loop(), plaintextConfiguration(server.port()), testIdentity());
        std::atomic<bool> handlerRan{false};
        auto              restartExecutor = std::make_shared<RestartExecutor>(true);
        CommandDispatcher dispatcher(client, *state, {restartExecutor},
                                     [&handlerRan]() -> Task<>
                                     {
                                         handlerRan.store(true, std::memory_order_release);
                                         co_return;
                                     });
        LoopStopGuard     stopGuard{loop};

        ASSERT_TRUE(loop.run(dispatcher.onWebSocketCommand(AgentCommand{1402, "agent.restart", "{}", 30}), std::chrono::seconds{15}, "重启命令分发"));

        EXPECT_TRUE(handlerRan.load(std::memory_order_acquire)) << "上报失败也必须重启，队列会在新进程继续补报";
        EXPECT_TRUE(state->hasResult(1402)) << "未确认的结果行必须留在队列里";
    }

    TEST(AgentCommand, UnsupportedTypeIsReportedAsFailed)
    {
        ConfigValue command = ConfigValue::object({{"id", 901}, {"nodeId", 77}, {"type", "unknown.type"}, {"payload", "{}"}, {"timeoutSeconds", 30}});
        auto        capture = std::make_shared<ResultCapture>();

        TestSupport::ServerHarness server(
                [command, capture](AsynGyanis::Net::Router &router, EventLoop &)
                {
                    installNextRoute(router, command);
                    installResultRoute(router, capture);
                });

        TestSupport::TemporaryDirectory directory;
        auto                            state = openState(directory);
        ASSERT_TRUE(state.has_value()) << state.error();

        AgentLoop         loop;
        AgentClient       client(loop.loop(), plaintextConfiguration(server.port()), testIdentity());
        CommandDispatcher dispatcher(client, *state, {});
        LoopStopGuard     stopGuard{loop};

        ASSERT_TRUE(loop.run(dispatcher.pollOnce(), std::chrono::seconds{15}, "轮询未知类型"));
        ASSERT_EQ(1, capture->posts.load(std::memory_order_acquire));
        const auto posted = AsynGyanis::Base::parseConfigValue(capture->bodyAt(0));
        ASSERT_TRUE(posted.has_value()) << capture->bodyAt(0);
        EXPECT_EQ("failed", posted->value("status", std::string{}));
        EXPECT_NE(std::string::npos, posted->value("message", std::string{}).find("不支持"));
    }

    TEST(AgentCommand, ApplicationHeartbeatPersistsDesiredConfigVersion)
    {
        TestSupport::TemporaryDirectory    directory;
        const Testing::CertificateMaterial material = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");

        TestSupport::ServerHarness server(
                [](AsynGyanis::Net::Router &router, EventLoop &)
                {
                    router.post("/api/agent/register",
                                [](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> Task<>
                                {
                                    response.setStatus(200);
                                    response.setBody(R"({"code":200,"message":"ok","data":{"id":77},"traceId":"trace-register"})");
                                    co_return;
                                });
                    router.post("/api/agent/heartbeat",
                                [](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> Task<>
                                {
                                    response.setStatus(200);
                                    response.setBody(
                                            R"({"code":200,"message":"ok","data":{"id":77,"desiredConfigVersion":12,"heartbeatIntervalSeconds":5},"traceId":"trace-heartbeat"})");
                                    co_return;
                                });
                    router.get("/api/agent/commands/next",
                               [](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> Task<>
                               {
                                   response.setStatus(200);
                                   response.setBody(R"({"code":200,"message":"ok","data":null,"traceId":"trace-next"})");
                                   co_return;
                               });
                });

        const auto configurationPath = directory.write("agent.yaml", std::string{"agent:\n"
                                                                                 "  server:\n"
                                                                                 "    base_url: http://127.0.0.1:" +
                                                                                 std::to_string(server.port()) +
                                                                                 "\n"
                                                                                 "    allow_plaintext_for_tests: true\n"
                                                                                 "    ca_file: agent.pem\n"
                                                                                 "    certificate_file: agent.pem\n"
                                                                                 "    private_key_file: agent-key.pem\n"
                                                                                 "    name: edge-01\n"
                                                                                 "    websocket_enabled: false\n"
                                                                                 "    heartbeat_interval_seconds: 5\n"
                                                                                 "    command_poll_interval_seconds: 5\n"
                                                                                 "    request_timeout_seconds: 5\n"
                                                                                 "  state:\n"
                                                                                 "    directory: state\n"
                                                                                 "  logging:\n"
                                                                                 "    file: state/agent.log\n"});

        LoggingSinkReset loggingReset;
        const auto       configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        std::atomic<int> exitCode{-1};
        std::thread      runner([&application, &exitCode] { exitCode.store(application.run(), std::memory_order_release); });

        const bool persisted = waitFor(std::chrono::seconds{15},
                                       [&directory]
                                       {
                                           auto state = AgentState::open(directory.path() / "state" / "agent.db");
                                           return state.has_value() && state->get("desired_config_version").value_or("") == "12";
                                       });
        application.requestStop();
        runner.join();

        EXPECT_TRUE(persisted) << "心跳下发的配置版本未写入状态库";
        EXPECT_EQ(0, exitCode.load(std::memory_order_acquire));
        EXPECT_FALSE(material.fingerprint.empty());
    }

    namespace
    {
        /**
         * @brief 读取文件正文
         * @param path 文件路径
         * @return std::string 内容；打不开时为空
         */
        [[nodiscard]] std::string readIdentityFile(const std::filesystem::path &path)
        {
            std::ifstream stream(path, std::ios::binary);
            return std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        }

        /**
         * @brief 原子覆盖身份文件，权限 0600
         * @param path 目标路径
         * @param text 正文
         * @return bool 写入成功
         */
        [[nodiscard]] bool writeIdentityFile(const std::filesystem::path &path, const std::string &text)
        {
            std::string error;
            return AsynGyanis::Platform::AtomicFileWriter::writeText(path, text, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, &error);
        }
    } // namespace

    TEST(AgentCommand, WebSocketReloadIsNoOpOnPlaintextChannel)
    {
        AgentConfiguration configuration;
        configuration.baseUrl                = "http://127.0.0.1:9";
        configuration.allowPlaintextForTests = true;

        AsynGyanis::Core::EventLoop loop;
        AgentWebSocket              socket(loop, configuration);
        EXPECT_TRUE(socket.reloadIdentityFromDisk());
    }

    TEST(AgentCommand, WebSocketReloadPicksUpNewCertificateFiles)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      initial = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        const auto                      next    = Testing::writeSelfSignedCertificate(directory, 60L * 24L * 60L * 60L, "next");

        AgentConfiguration configuration;
        configuration.baseUrl               = "https://127.0.0.1:1";
        configuration.caFile                = initial.certificateFile;
        configuration.certificateFile       = initial.certificateFile;
        configuration.privateKeyFile        = initial.privateKeyFile;
        configuration.name                  = "edge-01";
        configuration.requestTimeoutSeconds = 5;

        AsynGyanis::Core::EventLoop loop;
        AgentWebSocket              socket(loop, configuration);

        ASSERT_TRUE(writeIdentityFile(configuration.certificateFile, readIdentityFile(next.certificateFile)));
        ASSERT_TRUE(writeIdentityFile(configuration.privateKeyFile, readIdentityFile(next.privateKeyFile)));
        EXPECT_TRUE(socket.reloadIdentityFromDisk());

        // 新私钥坏了，换代必须失败并保留旧上下文
        ASSERT_TRUE(writeIdentityFile(configuration.privateKeyFile, "not-a-private-key"));
        EXPECT_FALSE(socket.reloadIdentityFromDisk());
    }
} // namespace EdgePulse::Agent
