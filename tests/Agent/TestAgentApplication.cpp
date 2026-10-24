#include "AgentApplication.h"

#include "AgentState.h"
#include "CertificateFixture.h"
#include "AgentConfiguration.h"
#include "TestSupport/HttpClientHarness.h"
#include "TestSupport/TemporaryDirectory.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Runtime/Environment.h"
#include "Core/Coroutine/Task.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace EdgePulse::Agent
{
    namespace
    {
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
         * @brief 在用例存续期间设置环境变量，析构时恢复原值
         * @details 同一进程内还会跑其它用例，改动过的变量必须复原。
         */
        class ScopedEnvironmentVariable
        {
        public:
            ScopedEnvironmentVariable(std::string name, std::string value) : m_name(std::move(name)), m_previous(Common::environmentValue(m_name))
            {
                assign(value);
            }

            ~ScopedEnvironmentVariable()
            {
                if (m_previous.has_value())
                {
                    assign(*m_previous);
                } else
                {
                    remove();
                }
            }

            ScopedEnvironmentVariable(const ScopedEnvironmentVariable &)            = delete;
            ScopedEnvironmentVariable &operator=(const ScopedEnvironmentVariable &) = delete;

        private:
            /**
             * @brief 写入变量值
             * @param value 新值
             */
            void assign(const std::string &value) const
            {
#if defined(_WIN32)
                _putenv_s(m_name.c_str(), value.c_str());
#else
                ::setenv(m_name.c_str(), value.c_str(), 1);
#endif
            }

            /**
             * @brief 删除变量
             */
            void remove() const
            {
#if defined(_WIN32)
                _putenv_s(m_name.c_str(), "");
#else
                ::unsetenv(m_name.c_str());
#endif
            }

            std::string                m_name;     ///< 变量名
            std::optional<std::string> m_previous; ///< 用例开始前的原值
        };

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

    TEST(AgentApplication, RejectsStartupWhenLogFileCannotBeOpened)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      configurationPath = directory.write("agent.yaml", R"(
agent:
  server:
    base_url: https://127.0.0.1:9443
    ca_file: ca.pem
    certificate_file: agent.pem
    private_key_file: agent-key.pem
    name: edge-01
  state:
    directory: state
  logging:
    file: state/agent.log
)");

        // 把日志路径占成目录：文件 Sink 打不开它，启动必须失败而不是降级为仅控制台
        std::error_code error;
        std::filesystem::create_directories(directory.path() / "state" / "agent.log", error);
        ASSERT_FALSE(error) << error.message();

        const auto configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        EXPECT_EQ(1, application.run());
    }

    TEST(AgentApplication, RejectsStartupWhenIdentityCannotBeLoaded)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      configurationPath = directory.write("agent.yaml", R"(
agent:
  server:
    base_url: https://127.0.0.1:9443
    ca_file: missing-ca.pem
    certificate_file: missing-agent.pem
    private_key_file: missing-agent-key.pem
    name: edge-01
  state:
    directory: state
  logging:
    file: state/agent.log
)");

        LoggingSinkReset loggingReset;
        const auto       configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        EXPECT_EQ(1, application.run());
    }

    TEST(AgentApplication, RegistersAndPersistsNodeIdentity)
    {
        TestSupport::TemporaryDirectory    directory;
        const Testing::CertificateMaterial material = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");

        TestSupport::ServerHarness server(
                [](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                {
                    router.post("/api/agent/register",
                                [](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    response.setStatus(200);
                                    response.setBody(R"({"code":200,"message":"ok","data":{"id":77},"traceId":"trace-agent"})");
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

        const bool registered = waitFor(std::chrono::seconds{15},
                                        [&directory]
                                        {
                                            auto persisted = AgentState::open(directory.path() / "state" / "agent.db");
                                            return persisted.has_value() && persisted->get("node_id").value_or("") == "77";
                                        });
        application.requestStop();
        runner.join();

        EXPECT_TRUE(registered) << "注册结果未在时限内写入状态库";
        EXPECT_EQ(0, exitCode.load(std::memory_order_acquire));

        auto state = AgentState::open(directory.path() / "state" / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();
        EXPECT_EQ("77", state->get("node_id").value_or(""));
        EXPECT_EQ(material.fingerprint, state->get("cert_fingerprint").value_or(""));
        EXPECT_EQ(material.expiresAt, state->get("cert_expiry").value_or(""));
        EXPECT_FALSE(state->get("agent_version").value_or("").empty());
    }

    TEST(AgentApplication, RebindTokenIsReadFromEnvironmentWhenStateHasNodeId)
    {
        TestSupport::TemporaryDirectory directory;
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        struct Capture
        {
            std::mutex  mutex; ///< 保护 body
            std::string body;  ///< 服务端收到的注册正文
        };
        auto                       capture = std::make_shared<Capture>();
        TestSupport::ServerHarness server(
                [capture](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                {
                    router.post("/api/agent/register",
                                [capture](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    {
                                        const std::lock_guard lock(capture->mutex);
                                        capture->body = std::string{request.body()};
                                    }
                                    response.setStatus(200);
                                    response.setBody(R"({"code":200,"message":"ok","data":{"id":88},"traceId":"trace-rebind"})");
                                    co_return;
                                });
                });

        const ScopedEnvironmentVariable enrollmentToken("EDGEPULSE_AGENT_TEST_ENROLLMENT_TOKEN", "enroll-token-value");
        const ScopedEnvironmentVariable rebindToken("EDGEPULSE_AGENT_TEST_REBIND_TOKEN", "rebind-token-value");

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
                                                                                 "    enrollment_token_env: EDGEPULSE_AGENT_TEST_ENROLLMENT_TOKEN\n"
                                                                                 "    rebind_token_env: EDGEPULSE_AGENT_TEST_REBIND_TOKEN\n"
                                                                                 "    request_timeout_seconds: 5\n"
                                                                                 "  state:\n"
                                                                                 "    directory: state\n"
                                                                                 "  logging:\n"
                                                                                 "    file: state/agent.log\n"});

        {
            auto state = AgentState::open(directory.path() / "state" / "agent.db");
            ASSERT_TRUE(state.has_value()) << state.error();
            state->set("node_id", "42");
        }

        LoggingSinkReset loggingReset;
        const auto       configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        std::atomic<int> exitCode{-1};
        std::thread      runner([&application, &exitCode] { exitCode.store(application.run(), std::memory_order_release); });

        static_cast<void>(waitFor(std::chrono::seconds{15},
                                  [&capture]
                                  {
                                      const std::lock_guard lock(capture->mutex);
                                      return !capture->body.empty();
                                  }));
        application.requestStop();
        runner.join();
        EXPECT_EQ(0, exitCode.load(std::memory_order_acquire));

        std::string body;
        {
            const std::lock_guard lock(capture->mutex);
            body = capture->body;
        }
        const auto registration = AsynGyanis::Base::parseConfigValue(body);
        ASSERT_TRUE(registration.has_value()) << body;
        EXPECT_EQ("rebind-token-value", registration->value("enrollmentToken", std::string{}));
        EXPECT_EQ(std::string::npos, body.find("enroll-token-value"));
    }

    TEST(AgentApplication, StopsAfterThreeConsecutiveConflicts)
    {
        TestSupport::TemporaryDirectory directory;
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        auto                       attempts = std::make_shared<std::atomic<int>>(0);
        TestSupport::ServerHarness server(
                [attempts](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                {
                    router.post("/api/agent/register",
                                [attempts](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    const int attempt = attempts->fetch_add(1, std::memory_order_relaxed) + 1;
                                    // 第 4 次返回成功限制用例时长：客户端若未在 3 次后停止，会以退出码 0 失败而不是永久挂起
                                    if (attempt <= 3)
                                    {
                                        response.setStatus(409);
                                        response.setBody(R"({"code":409,"message":"并发注册冲突，请稍后重试","data":null,"traceId":"trace-conflict"})");
                                    } else
                                    {
                                        response.setStatus(200);
                                        response.setBody(R"({"code":200,"message":"ok","data":{"id":91},"traceId":"trace-conflict"})");
                                    }
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
                                                                                 "    request_timeout_seconds: 5\n"
                                                                                 "  state:\n"
                                                                                 "    directory: state\n"
                                                                                 "  logging:\n"
                                                                                 "    file: state/agent.log\n"});

        LoggingSinkReset loggingReset;
        const auto       configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        EXPECT_EQ(1, application.run());
        EXPECT_EQ(3, attempts->load());
    }

    TEST(AgentApplication, StopRequestedFromAnotherThreadReturnsWithinDeadline)
    {
        TestSupport::TemporaryDirectory directory;
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        auto heartbeats = std::make_shared<std::atomic<int>>(0);
        TestSupport::ServerHarness server(
                [heartbeats](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                {
                    router.post("/api/agent/register",
                                [](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    response.setStatus(200);
                                    response.setBody(R"({"code":200,"message":"ok","data":{"id":77},"traceId":"trace-register"})");
                                    co_return;
                                });
                    router.post("/api/agent/heartbeat",
                                [heartbeats](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    heartbeats->fetch_add(1, std::memory_order_release);
                                    response.setStatus(200);
                                    response.setBody(R"({"code":200,"message":"ok","data":{"id":77,"heartbeatIntervalSeconds":5},"traceId":"trace-heartbeat"})");
                                    co_return;
                                });
                    router.get("/api/agent/commands/next",
                               [](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
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

        const bool running = waitFor(std::chrono::seconds{8}, [&heartbeats] { return heartbeats->load(std::memory_order_acquire) >= 1; });
        EXPECT_TRUE(running) << "Agent 未在时限内进入常驻运行";
        EXPECT_EQ(-1, exitCode.load(std::memory_order_acquire));

        application.requestStop();
        const bool stopped = waitFor(std::chrono::seconds{5}, [&exitCode] { return exitCode.load(std::memory_order_acquire) != -1; });
        runner.join();

        EXPECT_TRUE(stopped) << "跨线程停止请求未在 5 秒内结束 run()";
        EXPECT_EQ(0, exitCode.load(std::memory_order_acquire));
    }

    TEST(AgentApplication, HeartbeatRetriesAfterServerFailure)
    {
        TestSupport::TemporaryDirectory directory;
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        auto heartbeats = std::make_shared<std::atomic<int>>(0);
        TestSupport::ServerHarness server(
                [heartbeats](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                {
                    router.post("/api/agent/register",
                                [](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    response.setStatus(200);
                                    response.setBody(R"({"code":200,"message":"ok","data":{"id":77},"traceId":"trace-register"})");
                                    co_return;
                                });
                    router.post("/api/agent/heartbeat",
                                [heartbeats](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    const int sequence = heartbeats->fetch_add(1, std::memory_order_acq_rel) + 1;
                                    if (sequence == 1)
                                    {
                                        response.setStatus(500);
                                        response.setBody(R"({"code":500,"message":"暂时不可用","data":null,"traceId":"trace-heartbeat"})");
                                    } else
                                    {
                                        response.setStatus(200);
                                        response.setBody(R"({"code":200,"message":"ok","data":{"id":77,"heartbeatIntervalSeconds":5},"traceId":"trace-heartbeat"})");
                                    }
                                    co_return;
                                });
                    router.get("/api/agent/commands/next",
                               [](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
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

        const bool retried = waitFor(std::chrono::seconds{8}, [&heartbeats] { return heartbeats->load(std::memory_order_acquire) >= 2; });
        EXPECT_TRUE(retried) << "心跳失败后未在时限内发出下一次心跳";
        EXPECT_EQ(-1, exitCode.load(std::memory_order_acquire)) << "心跳失败不应结束 Agent";

        application.requestStop();
        const bool stopped = waitFor(std::chrono::seconds{5}, [&exitCode] { return exitCode.load(std::memory_order_acquire) != -1; });
        runner.join();

        EXPECT_TRUE(stopped) << "停止请求未在 5 秒内结束 run()";
        EXPECT_EQ(0, exitCode.load(std::memory_order_acquire));
    }
} // namespace EdgePulse::Agent
