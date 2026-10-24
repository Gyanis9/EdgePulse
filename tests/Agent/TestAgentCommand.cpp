#include "AgentApplication.h"
#include "AgentClient.h"
#include "AgentConfiguration.h"
#include "AgentState.h"
#include "AgentWebSocket.h"
#include "CertificateFixture.h"
#include "CommandDispatcher.h"
#include "Executors/CommandExecutor.h"
#include "TestSupport/HttpClientHarness.h"
#include "TestSupport/TemporaryDirectory.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LoggerRegistry.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "Net/WebSocket/WebSocketPeer.h"
#include "Net/WebSocket/WebSocketUpgradePolicy.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
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
         * @brief 自持事件循环与协程帧的执行器
         * @details 协程帧留在本对象里，直到循环停止后才销毁，避免帧与循环的销毁顺序颠倒。
         */
        class AgentLoop
        {
        public:
            AgentLoop() : m_thread([this] { m_loop.run(); })
            {
            }

            ~AgentLoop()
            {
                stop();
            }

            AgentLoop(const AgentLoop &)            = delete;
            AgentLoop &operator=(const AgentLoop &) = delete;

            /**
             * @brief 停止并等待循环线程退出
             * @note 依赖循环的对象必须在调用本函数之后才析构，客户端连接池不能在循环运行时收尾
             */
            void stop()
            {
                m_loop.stop();
                if (m_thread.joinable())
                {
                    m_thread.join();
                }
            }

            [[nodiscard]] EventLoop &loop() noexcept
            {
                return m_loop;
            }

            /**
             * @brief 把一条协程投到循环上并等待结果
             * @tparam T 协程返回值类型
             * @param operation 待执行的协程
             * @param timeout 等待上限
             * @param what 用例失败时的动作名
             * @return 有时限内完成时返回结果；超时或抛出异常时为空
             */
            template<typename T>
            std::optional<T> run(Task<T> operation, const std::chrono::seconds timeout, const char *what)
            {
                auto promise = std::make_shared<std::promise<T>>();
                auto future  = promise->get_future();
                auto frame   = forward(std::move(operation), promise);
                m_loop.scheduler().scheduleRemote(frame.handle());
                m_frames.push_back(std::move(frame));

                if (future.wait_for(timeout) != std::future_status::ready)
                {
                    ADD_FAILURE() << what << " 未在时限内完成";
                    return std::nullopt;
                }
                try
                {
                    return future.get();
                } catch (const std::exception &failure)
                {
                    ADD_FAILURE() << what << " 抛出异常：" << failure.what();
                    return std::nullopt;
                }
            }

            /**
             * @brief 把一条无返回值的协程投到循环上并等待完成
             * @param operation 待执行的协程
             * @param timeout 等待上限
             * @param what 用例失败时的动作名
             * @return 在时限内正常完成返回 true
             */
            bool run(Task<> operation, const std::chrono::seconds timeout, const char *what)
            {
                auto promise = std::make_shared<std::promise<void>>();
                auto future  = promise->get_future();
                auto frame   = forwardVoid(std::move(operation), promise);
                m_loop.scheduler().scheduleRemote(frame.handle());
                m_frames.push_back(std::move(frame));

                if (future.wait_for(timeout) != std::future_status::ready)
                {
                    ADD_FAILURE() << what << " 未在时限内完成";
                    return false;
                }
                try
                {
                    future.get();
                    return true;
                } catch (const std::exception &failure)
                {
                    ADD_FAILURE() << what << " 抛出异常：" << failure.what();
                    return false;
                }
            }

        private:
            template<typename T>
            static Task<> forward(Task<T> operation, std::shared_ptr<std::promise<T>> promise)
            {
                try
                {
                    promise->set_value(co_await std::move(operation));
                } catch (...)
                {
                    promise->set_exception(std::current_exception());
                }
                co_return;
            }

            static Task<> forwardVoid(Task<> operation, std::shared_ptr<std::promise<void>> promise)
            {
                try
                {
                    co_await std::move(operation);
                    promise->set_value();
                } catch (...)
                {
                    promise->set_exception(std::current_exception());
                }
                co_return;
            }

            EventLoop           m_loop;   ///< 执行 Agent 协程的循环
            std::jthread        m_thread; ///< 驱动循环的线程
            std::vector<Task<>> m_frames; ///< 协程帧，活到本对象析构
        };

        /**
         * @brief 在用例离开作用域时先停掉 Agent 循环
         * @details 依赖循环的客户端与分发器通常声明在循环之后，本守卫必须最后声明，才能保证它们先析构。
         */
        class LoopStopGuard
        {
        public:
            explicit LoopStopGuard(AgentLoop &loop) : m_loop(loop)
            {
            }

            ~LoopStopGuard()
            {
                m_loop.stop();
            }

            LoopStopGuard(const LoopStopGuard &)            = delete;
            LoopStopGuard &operator=(const LoopStopGuard &) = delete;

        private:
            AgentLoop &m_loop; ///< 受保护的循环
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
} // namespace EdgePulse::Agent
