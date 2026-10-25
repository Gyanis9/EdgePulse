#include "AgentApplication.h"

#include "AgentLoop.h"
#include "AgentState.h"
#include "AgentVersion.h"
#include "CertificateFixture.h"
#include "AgentConfiguration.h"
#include "TestSupport/HttpClientHarness.h"
#include "TestSupport/TemporaryDirectory.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogEvent.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Runtime/Environment.h"
#include "Core/Coroutine/Task.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "Net/WebSocket/WebSocketPeer.h"
#include "Net/WebSocket/WebSocketUpgradePolicy.h"

#include "Platform/FileSystem/FileSystem.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

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
         * @brief 收集根日志器消息的测试 Sink
         */
        class CapturingLogSink : public AsynGyanis::Base::LogSink
        {
        public:
            /**
             * @brief 收下一条日志事件
             * @param event 日志事件
             */
            void write(const AsynGyanis::Base::LogEvent &event) override
            {
                const std::lock_guard lock(m_mutex);
                m_messages.push_back(event.message);
                m_levels.push_back(event.level);
            }

            /**
             * @brief 刷新输出；内存收集没有待落地的内容
             */
            void flush() override
            {
            }

            /**
             * @brief 是否收到过包含指定文本的消息
             * @param text 待查找文本
             * @return bool 存在匹配消息时返回 true
             */
            [[nodiscard]] bool contains(const std::string_view text) const
            {
                const std::lock_guard lock(m_mutex);
                for (const auto &message: m_messages)
                {
                    if (message.find(text) != std::string::npos)
                    {
                        return true;
                    }
                }
                return false;
            }

            /**
             * @brief 是否收到过指定等级且包含指定文本的消息
             * @param level 日志等级
             * @param text 待查找文本
             * @return bool 存在匹配消息时返回 true
             */
            [[nodiscard]] bool contains(const AsynGyanis::Base::LogLevel level, const std::string_view text) const
            {
                const std::lock_guard lock(m_mutex);
                for (std::size_t index = 0; index < m_messages.size(); ++index)
                {
                    if (m_levels[index] == level && m_messages[index].find(text) != std::string::npos)
                    {
                        return true;
                    }
                }
                return false;
            }

            /**
             * @brief 统计包含指定文本的消息条数
             * @param text 待查找文本
             * @return std::size_t 匹配条数
             */
            [[nodiscard]] std::size_t count(const std::string_view text) const
            {
                const std::lock_guard lock(m_mutex);
                std::size_t           matched = 0;
                for (const auto &message: m_messages)
                {
                    if (message.find(text) != std::string::npos)
                    {
                        ++matched;
                    }
                }
                return matched;
            }

            /**
             * @brief 取全部消息副本
             * @return std::vector<std::string> 消息副本
             */
            [[nodiscard]] std::vector<std::string> messages() const
            {
                const std::lock_guard lock(m_mutex);
                return m_messages;
            }

        private:
            mutable std::mutex                      m_mutex;    ///< 保护两个容器
            std::vector<std::string>                m_messages; ///< 已收到的消息正文
            std::vector<AsynGyanis::Base::LogLevel> m_levels;   ///< 与消息一一对应的等级
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

        /**
         * @brief 读取整个文本文件
         * @param path 文件路径
         * @return std::string 文件正文；打不开时为空
         */
        [[nodiscard]] std::string readTextFile(const std::filesystem::path &path)
        {
            std::ifstream stream(path, std::ios::binary);
            return std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        }

        /**
         * @brief 拼出证书安装中断标记的正文
         * @param certificateFile 正式证书路径
         * @param privateKeyFile 正式私钥路径
         * @return std::string 四行路径清单
         */
        [[nodiscard]] std::string certificateInstallMarkerText(const std::filesystem::path &certificateFile, const std::filesystem::path &privateKeyFile)
        {
            const auto utf8   = [](const std::filesystem::path &path) { return AsynGyanis::Platform::FileSystem::utf8FromPath(path); };
            const auto backup = [](const std::filesystem::path &path) { return std::filesystem::path{path.string() + ".bak"}; };
            return "certificate=" + utf8(certificateFile) + "\n" + "certificate_backup=" + utf8(backup(certificateFile)) + "\n" + "private_key=" + utf8(privateKeyFile) + "\n" +
                   "private_key_backup=" + utf8(backup(privateKeyFile)) + "\n";
        }

        /**
         * @brief 注册升级健康用例需要的服务端路由
         * @param router 服务端路由表
         * @param heartbeats 心跳计数
         * @details 注册与心跳返回成功；命令轮询返回空；结果上报返回 500 以便用例从本地队列核对内容。
         */
        void registerUpdateHealthRoutes(AsynGyanis::Net::Router &router, const std::shared_ptr<std::atomic<int>> &heartbeats)
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
                            heartbeats->fetch_add(1, std::memory_order_acq_rel);
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
            router.post("/api/agent/commands/:id/result",
                        [](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                        {
                            response.setStatus(500);
                            response.setBody(R"({"code":500,"message":"暂时不可用","data":null,"traceId":"trace-result"})");
                            co_return;
                        });
        }

        /**
         * @brief 构造升级健康用例的配置文本
         * @param port 回环服务器端口
         * @param websocketEnabled 是否启用 WebSocket 命令通道
         * @param pollIntervalSeconds 命令轮询间隔
         * @return std::string YAML 配置
         */
        [[nodiscard]] std::string updateHealthConfiguration(const std::uint16_t port, const bool websocketEnabled = false, const int pollIntervalSeconds = 5)
        {
            return std::string{"agent:\n"
                               "  server:\n"
                               "    base_url: http://127.0.0.1:" +
                               std::to_string(port) +
                               "\n"
                               "    allow_plaintext_for_tests: true\n"
                               "    ca_file: agent.pem\n"
                               "    certificate_file: agent.pem\n"
                               "    private_key_file: agent-key.pem\n"
                               "    name: edge-01\n"
                               "    websocket_enabled: " +
                               (websocketEnabled ? "true" : "false") +
                               "\n"
                               "    heartbeat_interval_seconds: 5\n"
                               "    command_poll_interval_seconds: " +
                               std::to_string(pollIntervalSeconds) +
                               "\n"
                               "    request_timeout_seconds: 5\n"
                               "  state:\n"
                               "    directory: state\n"
                               "  logging:\n"
                               "    file: state/agent.log\n"};
        }

        /**
         * @brief 命令结果上报的捕获点
         */
        struct CommandPostCapture
        {
            std::mutex               mutex;    ///< 保护 paths 与 bodies
            std::vector<std::string> paths;    ///< 收到的上报路径
            std::vector<std::string> bodies;   ///< 收到的上报正文
            std::atomic<int>         posts{0}; ///< 已收到的上报次数

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
             * @brief 取路径副本
             * @param index 序号
             * @return std::string 路径
             */
            [[nodiscard]] std::string pathAt(const std::size_t index)
            {
                const std::lock_guard lock(mutex);
                return index < paths.size() ? paths[index] : std::string{};
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
        };

        /**
         * @brief 安装命令结果上报路由
         * @param router 目标路由器
         * @param capture 捕获点
         */
        void installResultRoute(AsynGyanis::Net::Router &router, const std::shared_ptr<CommandPostCapture> &capture)
        {
            router.post("/api/agent/commands/:id/result",
                        [capture](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                        {
                            capture->record(std::string{request.path()}, std::string{request.body()});
                            response.setStatus(200);
                            response.setBody(R"({"code":200,"message":"ok","data":null,"traceId":"trace-result"})");
                            co_return;
                        });
        }

        /**
         * @brief 构造一条诊断命令的响应正文
         * @param commandId 命令标识
         * @return std::string JSON 响应
         */
        [[nodiscard]] std::string diagnosticsCommandResponse(const std::int64_t commandId)
        {
            const AsynGyanis::Base::ConfigValue command =
                    AsynGyanis::Base::ConfigValue::object({{"id", commandId}, {"nodeId", 77}, {"type", "diagnostics.collect"}, {"payload", "{}"}, {"timeoutSeconds", 30}});
            return AsynGyanis::Base::ConfigValue::object({{"code", 200}, {"message", "ok"}, {"data", command}, {"traceId", "trace-next"}}).dump();
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

    TEST(AgentApplication, RecoversInterruptedCertificateInstallBeforeLoadingIdentity)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      initial        = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        const auto                      next           = Testing::writeSelfSignedCertificate(directory, 60L * 24L * 60L * 60L, "next");
        const std::string               oldCertificate = readTextFile(initial.certificateFile);
        const std::string               oldKey         = readTextFile(initial.privateKeyFile);

        // 模拟进程死在私钥已换、证书未换之间，备份与标记都留在盘上
        static_cast<void>(directory.write("agent-key.pem", readTextFile(next.privateKeyFile)));
        static_cast<void>(directory.write("agent.pem", oldCertificate));
        static_cast<void>(directory.write("agent.pem.bak", oldCertificate));
        static_cast<void>(directory.write("agent-key.pem.bak", oldKey));
        const auto marker = directory.write("agent.pem.install-pending", certificateInstallMarkerText(initial.certificateFile, initial.privateKeyFile));

        TestSupport::ServerHarness server(
                [](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                {
                    router.post("/api/agent/register",
                                [](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    response.setStatus(403);
                                    response.setBody(R"({"code":403,"message":"拒绝","data":null,"traceId":"trace-recover"})");
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
        EXPECT_EQ(1, application.run()) << "注册被拒后应按启动失败退出，恢复动作在此之前完成";

        EXPECT_EQ(oldCertificate, readTextFile(initial.certificateFile)) << "启动时必须用备份恢复旧证书";
        EXPECT_EQ(oldKey, readTextFile(initial.privateKeyFile)) << "启动时必须用备份恢复旧私钥";
        EXPECT_FALSE(std::filesystem::exists(marker)) << "恢复后必须删除安装标记";

        const auto identity = AgentIdentity::load(*configuration);
        ASSERT_TRUE(identity.has_value()) << identity.error();
        EXPECT_EQ(initial.fingerprint, identity->fingerprint);
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

        auto                       heartbeats = std::make_shared<std::atomic<int>>(0);
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

        auto                       heartbeats = std::make_shared<std::atomic<int>>(0);
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

    TEST(AgentApplication, WritesHealthMarkerAndReportsUpdateAfterFirstHeartbeat)
    {
        TestSupport::TemporaryDirectory directory;
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        auto                       heartbeats = std::make_shared<std::atomic<int>>(0);
        TestSupport::ServerHarness server([heartbeats](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &) { registerUpdateHealthRoutes(router, heartbeats); });
        const auto                 configurationPath = directory.write("agent.yaml", updateHealthConfiguration(server.port()));

        {
            auto state = AgentState::open(directory.path() / "state" / "agent.db");
            ASSERT_TRUE(state.has_value()) << state.error();
            UpdateStateRecord record;
            record.version       = std::string{kAgentVersion};
            record.pendingResult = "{\"commandId\":123,\"version\":\"" + std::string{kAgentVersion} + "\",\"status\":\"pending\"}";
            const auto written   = state->setUpdateState(record);
            ASSERT_TRUE(written.has_value()) << written.error();

            // 安装器启动时已为同一命令入队过成功结果；健康确认必须替换它而不是被唯一索引丢弃
            state->enqueueResult(123, R"({"status":"succeeded","result":"{}","message":"升级包已校验，安装器已启动"})");
        }

        LoggingSinkReset loggingReset;
        const auto       configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        std::atomic<int> exitCode{-1};
        std::thread      runner([&application, &exitCode] { exitCode.store(application.run(), std::memory_order_release); });

        const std::filesystem::path healthMarker = directory.path() / "state" / "update-health";
        const bool                  marked       = waitFor(std::chrono::seconds{10}, [&healthMarker] { return std::filesystem::exists(healthMarker); });
        const bool                  cleared      = waitFor(std::chrono::seconds{5},
                                                           [&directory]
                                                           {
                                         auto state = AgentState::open(directory.path() / "state" / "agent.db");
                                         if (!state.has_value())
                                         {
                                             return false;
                                         }
                                         const auto record = state->updateState();
                                         return record.has_value() && record->pendingResult.empty();
                                                           });
        application.requestStop();
        const bool stopped = waitFor(std::chrono::seconds{5}, [&exitCode] { return exitCode.load(std::memory_order_acquire) != -1; });
        runner.join();

        ASSERT_TRUE(marked) << "首次心跳后未写出升级健康标记";
        ASSERT_TRUE(cleared) << "升级待上报结果未在时限内清除";
        ASSERT_TRUE(stopped) << "停止请求未在时限内结束 run()";
        EXPECT_EQ(0, exitCode.load(std::memory_order_acquire));

        auto state = AgentState::open(directory.path() / "state" / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();
        const std::string currentVersion = state->get("agent_version").value_or("");
        ASSERT_FALSE(currentVersion.empty());
        EXPECT_EQ(currentVersion, readTextFile(healthMarker)) << "健康标记必须只包含当前 Agent 版本";
        const auto record = state->updateState();
        ASSERT_TRUE(record.has_value()) << record.error();
        EXPECT_TRUE(record->pendingResult.empty());
        EXPECT_FALSE(record->healthyAt.empty()) << "健康确认时刻未写入状态库";

        const std::vector<QueuedResult> results = state->takeResults(10);
        ASSERT_EQ(1U, results.size()) << "健康确认应恰好入队一条升级结果";
        EXPECT_EQ(123, results.front().commandId);
        const auto payload = AsynGyanis::Base::parseConfigValue(results.front().payload);
        ASSERT_TRUE(payload.has_value()) << results.front().payload;
        EXPECT_EQ("succeeded", payload->value("status", std::string{}));
        EXPECT_EQ("升级成功", payload->value("message", std::string{}));
        const auto detail = AsynGyanis::Base::parseConfigValue(payload->value("result", std::string{}));
        ASSERT_TRUE(detail.has_value()) << payload->value("result", std::string{});
        EXPECT_EQ(currentVersion, detail->value("version", std::string{}));
    }

    TEST(AgentApplication, ReportsRolledBackUpdateAfterFirstHeartbeat)
    {
        TestSupport::TemporaryDirectory directory;
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        auto                       heartbeats = std::make_shared<std::atomic<int>>(0);
        TestSupport::ServerHarness server([heartbeats](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &) { registerUpdateHealthRoutes(router, heartbeats); });
        const auto                 configurationPath = directory.write("agent.yaml", updateHealthConfiguration(server.port()));

        const std::filesystem::path rollbackMarker = directory.path() / "state" / "update-rollback";
        {
            auto state = AgentState::open(directory.path() / "state" / "agent.db");
            ASSERT_TRUE(state.has_value()) << state.error();
            UpdateStateRecord record;
            record.version       = "9.9.9";
            record.pendingResult = R"({"commandId":456,"version":"9.9.9","status":"pending"})";
            const auto written   = state->setUpdateState(record);
            ASSERT_TRUE(written.has_value()) << written.error();

            // 安装器启动时已入队的成功结果仍在队列里；回滚结论必须替换它
            state->enqueueResult(456, R"({"status":"succeeded","result":"{}","message":"升级包已校验，安装器已启动"})");

            std::ofstream marker(rollbackMarker, std::ios::binary | std::ios::trunc);
            marker << "9.9.9\n";
            marker.close();
            ASSERT_TRUE(marker) << "无法写入升级回滚标记";
        }

        LoggingSinkReset loggingReset;
        const auto       configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        std::atomic<int> exitCode{-1};
        std::thread      runner([&application, &exitCode] { exitCode.store(application.run(), std::memory_order_release); });

        const bool cleared = waitFor(std::chrono::seconds{10},
                                     [&directory, &rollbackMarker]
                                     {
                                         if (std::filesystem::exists(rollbackMarker))
                                         {
                                             return false;
                                         }
                                         auto state = AgentState::open(directory.path() / "state" / "agent.db");
                                         if (!state.has_value())
                                         {
                                             return false;
                                         }
                                         const auto record = state->updateState();
                                         return record.has_value() && record->pendingResult.empty();
                                     });
        application.requestStop();
        const bool stopped = waitFor(std::chrono::seconds{5}, [&exitCode] { return exitCode.load(std::memory_order_acquire) != -1; });
        runner.join();

        ASSERT_TRUE(cleared) << "回滚标记或待上报结果未在时限内清除";
        ASSERT_TRUE(stopped) << "停止请求未在时限内结束 run()";
        EXPECT_EQ(0, exitCode.load(std::memory_order_acquire));
        EXPECT_FALSE(std::filesystem::exists(directory.path() / "state" / "update-health")) << "回滚后不得写健康标记";

        auto state = AgentState::open(directory.path() / "state" / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();
        const auto record = state->updateState();
        ASSERT_TRUE(record.has_value()) << record.error();
        EXPECT_TRUE(record->pendingResult.empty());
        EXPECT_TRUE(record->healthyAt.empty());

        const std::vector<QueuedResult> results = state->takeResults(10);
        ASSERT_EQ(1U, results.size()) << "回滚确认应恰好入队一条失败结果";
        EXPECT_EQ(456, results.front().commandId);
        const auto payload = AsynGyanis::Base::parseConfigValue(results.front().payload);
        ASSERT_TRUE(payload.has_value()) << results.front().payload;
        EXPECT_EQ("failed", payload->value("status", std::string{}));
        EXPECT_EQ("升级未通过健康检查，已回滚到旧版本", payload->value("message", std::string{}));
        EXPECT_EQ("{}", payload->value("result", std::string{}));
    }

    TEST(AgentApplication, ReportsFailedUpdateWhenTargetVersionDiffersFromRunningVersion)
    {
        TestSupport::TemporaryDirectory directory;
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        auto                       heartbeats = std::make_shared<std::atomic<int>>(0);
        TestSupport::ServerHarness server([heartbeats](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &) { registerUpdateHealthRoutes(router, heartbeats); });
        const auto                 configurationPath = directory.write("agent.yaml", updateHealthConfiguration(server.port()));

        {
            auto state = AgentState::open(directory.path() / "state" / "agent.db");
            ASSERT_TRUE(state.has_value()) << state.error();
            UpdateStateRecord record;
            record.version       = "9.9.9";
            record.healthyAt     = "2026-10-24T00:00:00Z";
            record.pendingResult = R"({"commandId":789,"version":"9.9.9","status":"pending"})";
            const auto written   = state->setUpdateState(record);
            ASSERT_TRUE(written.has_value()) << written.error();
            state->enqueueResult(789, R"({"status":"succeeded","result":"{}","message":"升级包已校验，安装器已启动"})");
        }

        LoggingSinkReset loggingReset;
        const auto       configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        std::atomic<int> exitCode{-1};
        std::thread      runner([&application, &exitCode] { exitCode.store(application.run(), std::memory_order_release); });

        const bool cleared = waitFor(std::chrono::seconds{10},
                                     [&directory]
                                     {
                                         auto state = AgentState::open(directory.path() / "state" / "agent.db");
                                         if (!state.has_value())
                                         {
                                             return false;
                                         }
                                         const auto record = state->updateState();
                                         return record.has_value() && record->pendingResult.empty();
                                     });
        application.requestStop();
        const bool stopped = waitFor(std::chrono::seconds{5}, [&exitCode] { return exitCode.load(std::memory_order_acquire) != -1; });
        runner.join();

        ASSERT_TRUE(cleared) << "目标版本不一致时未在时限内清除待上报结果";
        ASSERT_TRUE(stopped) << "停止请求未在时限内结束 run()";
        EXPECT_EQ(0, exitCode.load(std::memory_order_acquire));
        EXPECT_FALSE(std::filesystem::exists(directory.path() / "state" / "update-health")) << "目标版本不一致时不得写健康标记";

        auto state = AgentState::open(directory.path() / "state" / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();
        const std::string runningVersion = state->get("agent_version").value_or("");
        ASSERT_FALSE(runningVersion.empty());
        const auto record = state->updateState();
        ASSERT_TRUE(record.has_value()) << record.error();
        EXPECT_TRUE(record->pendingResult.empty());
        EXPECT_TRUE(record->healthyAt.empty()) << "失败结论不得保留健康确认时刻";

        const std::vector<QueuedResult> results = state->takeResults(10);
        ASSERT_EQ(1U, results.size()) << "失败结论应替换安装器启动时入队的结果";
        EXPECT_EQ(789, results.front().commandId);
        const auto payload = AsynGyanis::Base::parseConfigValue(results.front().payload);
        ASSERT_TRUE(payload.has_value()) << results.front().payload;
        EXPECT_EQ("failed", payload->value("status", std::string{}));
        const std::string message = payload->value("message", std::string{});
        EXPECT_NE(std::string::npos, message.find("9.9.9"));
        EXPECT_NE(std::string::npos, message.find(runningVersion));
        EXPECT_NE(std::string::npos, message.find("不一致"));
    }
    TEST(AgentApplication, PollsCommandWhenWebSocketCannotConnect)
    {
        TestSupport::TemporaryDirectory directory;
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        auto heartbeats = std::make_shared<std::atomic<int>>(0);
        auto posts      = std::make_shared<CommandPostCapture>();
        auto served     = std::make_shared<std::atomic<bool>>(false);

        TestSupport::ServerHarness server(
                [heartbeats, posts, served](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
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
                                    heartbeats->fetch_add(1, std::memory_order_acq_rel);
                                    response.setStatus(200);
                                    response.setBody(R"({"code":200,"message":"ok","data":{"id":77,"heartbeatIntervalSeconds":5},"traceId":"trace-heartbeat"})");
                                    co_return;
                                });
                    // 没有 /api/agent/ws 路由：WebSocket 握手必然失败，命令只能靠启动即运行的轮询取到
                    router.get("/api/agent/commands/next",
                               [served](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                               {
                                   response.setStatus(200);
                                   if (!served->exchange(true, std::memory_order_acq_rel))
                                   {
                                       response.setBody(diagnosticsCommandResponse(1501));
                                   } else
                                   {
                                       response.setBody(R"({"code":200,"message":"ok","data":null,"traceId":"trace-next"})");
                                   }
                                   co_return;
                               });
                    installResultRoute(router, posts);
                });

        const auto       configurationPath = directory.write("agent.yaml", updateHealthConfiguration(server.port(), true));
        LoggingSinkReset loggingReset;
        const auto       configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        std::atomic<int> exitCode{-1};
        std::thread      runner([&application, &exitCode] { exitCode.store(application.run(), std::memory_order_release); });

        const bool executed = waitFor(std::chrono::seconds{10}, [&posts] { return posts->posts.load(std::memory_order_acquire) >= 1; });
        application.requestStop();
        const bool stopped = waitFor(std::chrono::seconds{5}, [&exitCode] { return exitCode.load(std::memory_order_acquire) != -1; });
        runner.join();

        ASSERT_TRUE(executed) << "WebSocket 不可用时启动轮询必须在时限内执行命令";
        ASSERT_TRUE(stopped) << "停止请求未在时限内结束 run()";
        EXPECT_EQ(0, exitCode.load(std::memory_order_acquire));
        EXPECT_NE(std::string::npos, posts->pathAt(0).find("/1501/"));
        const auto result = AsynGyanis::Base::parseConfigValue(posts->bodyAt(0));
        ASSERT_TRUE(result.has_value()) << posts->bodyAt(0);
        EXPECT_EQ("succeeded", result->value("status", std::string{}));
    }

    TEST(AgentApplication, HeartbeatPollCatchesCommandWhileWebSocketConnected)
    {
        TestSupport::TemporaryDirectory directory;
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        auto heartbeats = std::make_shared<std::atomic<int>>(0);
        auto hellos     = std::make_shared<std::atomic<int>>(0);
        auto posts      = std::make_shared<CommandPostCapture>();
        auto served     = std::make_shared<std::atomic<bool>>(false);

        TestSupport::ServerHarness server(
                [heartbeats, hellos, posts, served](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
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
                                    heartbeats->fetch_add(1, std::memory_order_acq_rel);
                                    response.setStatus(200);
                                    response.setBody(R"({"code":200,"message":"ok","data":{"id":77,"heartbeatIntervalSeconds":5},"traceId":"trace-heartbeat"})");
                                    co_return;
                                });
                    router.get("/api/agent/commands/next",
                               [heartbeats, hellos, served](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                               {
                                   response.setStatus(200);
                                   // 命令只在通道登记并完成一次心跳之后才可见；此时轮询已被暂停
                                   if (hellos->load(std::memory_order_acquire) > 0 && heartbeats->load(std::memory_order_acquire) > 0 &&
                                       !served->exchange(true, std::memory_order_acq_rel))
                                   {
                                       response.setBody(diagnosticsCommandResponse(1502));
                                   } else
                                   {
                                       response.setBody(R"({"code":200,"message":"ok","data":null,"traceId":"trace-next"})");
                                   }
                                   co_return;
                               });
                    router.get("/api/agent/ws",
                               [hellos](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                               {
                                   AsynGyanis::Net::WebSocketUpgradePolicy policy;
                                   policy.originMode = AsynGyanis::Net::WebSocketOriginMode::AllowAny;
                                   response.upgradeToWebSocket(
                                           [hellos](AsynGyanis::Net::WebSocketPeer &peer) -> AsynGyanis::Core::Task<>
                                           {
                                               const auto hello = co_await peer.receive();
                                               if (!hello.has_value())
                                               {
                                                   co_return;
                                               }
                                               hellos->fetch_add(1, std::memory_order_acq_rel);
                                               static_cast<void>(co_await peer.sendText(R"({"type":"hello","ok":true})"));
                                               for (;;)
                                               {
                                                   const auto frame = co_await peer.receive();
                                                   if (!frame.has_value())
                                                   {
                                                       co_return;
                                                   }
                                                   if (frame->opCode == AsynGyanis::Net::WebSocketOpCode::Text && frame->payload.find("\"ping\"") != std::string::npos)
                                                   {
                                                       static_cast<void>(co_await peer.sendText(R"({"type":"pong"})"));
                                                   }
                                               }
                                           },
                                           std::move(policy));
                                   co_return;
                               });
                    installResultRoute(router, posts);
                });

        // 轮询间隔远大于心跳：启动时那一轮取不到命令，之后只能由心跳补拉取到
        const auto       configurationPath = directory.write("agent.yaml", updateHealthConfiguration(server.port(), true, 600));
        LoggingSinkReset loggingReset;
        const auto       configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        std::atomic<int> exitCode{-1};
        std::thread      runner([&application, &exitCode] { exitCode.store(application.run(), std::memory_order_release); });

        const bool executed = waitFor(std::chrono::seconds{20}, [&posts] { return posts->posts.load(std::memory_order_acquire) >= 1; });
        application.requestStop();
        const bool stopped = waitFor(std::chrono::seconds{5}, [&exitCode] { return exitCode.load(std::memory_order_acquire) != -1; });
        runner.join();

        ASSERT_GT(hellos->load(std::memory_order_acquire), 0) << "WebSocket 通道未完成 hello";
        ASSERT_TRUE(executed) << "心跳补拉未在时限内执行通道建立后出现的命令";
        ASSERT_TRUE(stopped) << "停止请求未在时限内结束 run()";
        EXPECT_EQ(0, exitCode.load(std::memory_order_acquire));
        EXPECT_NE(std::string::npos, posts->pathAt(0).find("/1502/"));
    }

    TEST(AgentApplication, StopRequestedDuringFailingRegistrationReturnsPromptly)
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
                                    const int attempt = attempts->fetch_add(1, std::memory_order_acq_rel) + 1;
                                    response.setStatus(attempt == 1 ? 500 : 200);
                                    response.setBody(attempt == 1 ? R"({"code":500,"message":"暂时不可用","data":null,"traceId":"trace-register"})"
                                                                  : R"({"code":200,"message":"ok","data":{"id":77},"traceId":"trace-register"})");
                                    co_return;
                                });
                    router.post("/api/agent/heartbeat",
                                [](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
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

        const auto       configurationPath = directory.write("agent.yaml", updateHealthConfiguration(server.port()));
        LoggingSinkReset loggingReset;
        const auto       configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        std::atomic<int> exitCode{-1};
        std::thread      runner([&application, &exitCode] { exitCode.store(application.run(), std::memory_order_release); });

        ASSERT_TRUE(waitFor(std::chrono::seconds{8}, [&attempts] { return attempts->load(std::memory_order_acquire) >= 1; })) << "注册请求未发出";
        const auto stopStarted = std::chrono::steady_clock::now();
        application.requestStop();
        const bool stopped = waitFor(std::chrono::seconds{6}, [&exitCode] { return exitCode.load(std::memory_order_acquire) != -1; });
        const auto elapsed = std::chrono::steady_clock::now() - stopStarted;
        runner.join();

        ASSERT_TRUE(stopped) << "注册阶段的停止请求未在时限内结束 run()";
        EXPECT_EQ(0, exitCode.load(std::memory_order_acquire)) << "注册阶段收到停止请求应按服务停止返回 0";
        EXPECT_LE(elapsed, std::chrono::seconds{2}) << "注册退避必须按一秒切片检查停止标记";
    }

    TEST(AgentApplication, WritesHealthMarkerFromExpectedVersionAfterFirstHeartbeat)
    {
        TestSupport::TemporaryDirectory directory;
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        auto                       heartbeats = std::make_shared<std::atomic<int>>(0);
        TestSupport::ServerHarness server([heartbeats](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &) { registerUpdateHealthRoutes(router, heartbeats); });
        const auto                 configurationPath = directory.write("agent.yaml", updateHealthConfiguration(server.port()));

        const std::filesystem::path expectedMarker = directory.path() / "state" / "update-expected";
        const std::filesystem::path healthMarker   = directory.path() / "state" / "update-health";
        std::error_code             error;
        std::filesystem::create_directories(expectedMarker.parent_path(), error);
        ASSERT_FALSE(error) << error.message();
        std::ofstream(expectedMarker, std::ios::binary | std::ios::trunc) << kAgentVersion << "\n";
        ASSERT_TRUE(std::filesystem::exists(expectedMarker));

        LoggingSinkReset loggingReset;
        const auto       configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        std::atomic<int> exitCode{-1};
        std::thread      runner([&application, &exitCode] { exitCode.store(application.run(), std::memory_order_release); });

        const bool marked          = waitFor(std::chrono::seconds{10}, [&healthMarker] { return std::filesystem::exists(healthMarker); });
        const bool expectedCleared = waitFor(std::chrono::seconds{5}, [&expectedMarker] { return !std::filesystem::exists(expectedMarker); });
        application.requestStop();
        const bool stopped = waitFor(std::chrono::seconds{5}, [&exitCode] { return exitCode.load(std::memory_order_acquire) != -1; });
        runner.join();

        ASSERT_TRUE(marked) << "手工升级的 update-expected 未触发健康标记";
        ASSERT_TRUE(expectedCleared) << "确认健康后必须删除 update-expected";
        ASSERT_TRUE(stopped) << "停止请求未在时限内结束 run()";
        EXPECT_EQ(0, exitCode.load(std::memory_order_acquire));
        EXPECT_EQ(std::string{kAgentVersion}, readTextFile(healthMarker));

        auto state = AgentState::open(directory.path() / "state" / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();
        EXPECT_TRUE(state->takeResults(10).empty()) << "手工升级没有待上报的命令结果";
    }

    TEST(AgentApplication, KeepsExpectedVersionMarkerWhenItDiffersFromRunningVersion)
    {
        TestSupport::TemporaryDirectory directory;
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        auto                       heartbeats = std::make_shared<std::atomic<int>>(0);
        TestSupport::ServerHarness server([heartbeats](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &) { registerUpdateHealthRoutes(router, heartbeats); });
        const auto                 configurationPath = directory.write("agent.yaml", updateHealthConfiguration(server.port()));

        const std::filesystem::path expectedMarker = directory.path() / "state" / "update-expected";
        const std::filesystem::path healthMarker   = directory.path() / "state" / "update-health";
        std::error_code             error;
        std::filesystem::create_directories(expectedMarker.parent_path(), error);
        ASSERT_FALSE(error) << error.message();
        std::ofstream(expectedMarker, std::ios::binary | std::ios::trunc) << "9.9.9\n";

        LoggingSinkReset loggingReset;
        const auto       configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        std::atomic<int> exitCode{-1};
        std::thread      runner([&application, &exitCode] { exitCode.store(application.run(), std::memory_order_release); });

        const bool heartbeat = waitFor(std::chrono::seconds{10}, [&heartbeats] { return heartbeats->load(std::memory_order_acquire) >= 1; });
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        application.requestStop();
        const bool stopped = waitFor(std::chrono::seconds{5}, [&exitCode] { return exitCode.load(std::memory_order_acquire) != -1; });
        runner.join();

        ASSERT_TRUE(heartbeat) << "Agent 未在时限内完成首次心跳";
        ASSERT_TRUE(stopped) << "停止请求未在时限内结束 run()";
        EXPECT_EQ(0, exitCode.load(std::memory_order_acquire));
        EXPECT_FALSE(std::filesystem::exists(healthMarker)) << "版本不符不得写健康标记";
        ASSERT_TRUE(std::filesystem::exists(expectedMarker)) << "版本不符必须保留标记供安装器回滚";
        EXPECT_EQ(0U, readTextFile(expectedMarker).find("9.9.9"));
    }

    namespace
    {
        /**
         * @brief 把时刻写成与身份一致的 UTC 文本
         * @param point 目标时刻
         * @return std::string 形如 2027-01-01T00:00:00Z
         */
        [[nodiscard]] std::string utcTextAt(const std::chrono::system_clock::time_point point)
        {
            const std::time_t seconds = std::chrono::system_clock::to_time_t(point);
            tm                utc{};
#ifdef _WIN32
            gmtime_s(&utc, &seconds);
#else
            gmtime_r(&seconds, &utc);
#endif
            char buffer[32]{};
            std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
            return std::string{buffer};
        }

        /**
         * @brief 续期循环各动作的调用计数与等待记录
         */
        struct RenewalProbe
        {
            std::atomic<int>                       generate{0};        ///< 生成密钥与 CSR 的次数
            std::atomic<int>                       renew{0};           ///< 向服务端续期的次数
            std::atomic<int>                       install{0};         ///< 安装新证书的次数
            std::atomic<int>                       reloadHttp{0};      ///< 重载 HTTP 身份的次数
            std::atomic<int>                       reloadWebSocket{0}; ///< 重载 WebSocket 身份的次数
            std::atomic<int>                       commit{0};          ///< 两条通道都重载后清理备份的次数
            std::mutex                             mutex;              ///< 保护 waits
            std::vector<std::chrono::milliseconds> waits;              ///< 每次等待前收到的计划时长

            /**
             * @brief 记录一次计划等待
             * @param duration 计划时长
             */
            void recordWait(const std::chrono::milliseconds duration)
            {
                const std::lock_guard lock(mutex);
                waits.push_back(duration);
            }

            /**
             * @brief 取等待时长副本
             * @return std::vector<std::chrono::milliseconds> 副本
             */
            [[nodiscard]] std::vector<std::chrono::milliseconds> waitDurations()
            {
                const std::lock_guard lock(mutex);
                return waits;
            }
        };

        /**
         * @brief 打开用例用的状态库
         * @param directory 临时目录
         * @return AgentState 已就绪的状态库
         */
        [[nodiscard]] AgentState openRenewalState(const TestSupport::TemporaryDirectory &directory)
        {
            auto state = AgentState::open(directory.path() / "agent.db");
            if (!state.has_value())
            {
                throw std::runtime_error(state.error());
            }
            return std::move(*state);
        }
    } // namespace

    TEST(AgentApplication, RenewalRunsWhenCertificateIsDueAndUpdatesState)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = openRenewalState(directory);
        state.set("cert_fingerprint", "old-fingerprint");

        AgentConfiguration configuration;
        configuration.certificateRenewBeforeDays     = 7;
        configuration.certificateRenewalCheckSeconds = 3600;

        AgentLoop         agentLoop;
        std::atomic<bool> stop{false};
        RenewalProbe      probe;

        CertificateRenewalDependencies dependencies;
        dependencies.expiresAt       = utcTextAt(std::chrono::system_clock::now() + std::chrono::days{5});
        dependencies.generateRequest = [&probe]
        {
            probe.generate.fetch_add(1, std::memory_order_acq_rel);
            return std::expected<CertificateRequestMaterial, std::string>{CertificateRequestMaterial{"key-pem", "csr-pem"}};
        };
        dependencies.renew = [&probe](const std::int64_t nodeId, std::string csrPem) -> AsynGyanis::Core::Task<std::expected<RenewedCertificate, CertificateRenewalFailure>>
        {
            probe.renew.fetch_add(1, std::memory_order_acq_rel);
            if (nodeId != 4242 || csrPem != "csr-pem")
            {
                co_return std::unexpected(CertificateRenewalFailure{"请求参数不符合预期", false, false, 0});
            }
            co_return RenewedCertificate{"cert-pem", std::string(64U, 'b'), "2026-10-25 08:00:00", "2027-10-25 08:00:00", "0a"};
        };
        dependencies.install = [&probe](const std::string_view certificatePem)
        {
            probe.install.fetch_add(1, std::memory_order_acq_rel);
            if (certificatePem != "cert-pem")
            {
                return std::expected<AgentIdentity, std::string>{std::unexpected("证书记录不符合预期")};
            }
            return std::expected<AgentIdentity, std::string>{AgentIdentity{std::string(64U, 'b'), "2027-10-25T08:00:00Z"}};
        };
        dependencies.reloadHttpIdentity = [&probe]
        {
            probe.reloadHttp.fetch_add(1, std::memory_order_acq_rel);
            return true;
        };
        dependencies.reloadWebSocketIdentity = [&probe, &stop]
        {
            probe.reloadWebSocket.fetch_add(1, std::memory_order_acq_rel);
            stop.store(true, std::memory_order_release);
            return true;
        };
        dependencies.checkInterval = std::chrono::milliseconds{25};
        dependencies.waitObserver  = [&probe](const std::chrono::milliseconds duration) { probe.recordWait(duration); };

        const bool completed =
                agentLoop.run(AgentApplication::runCertificateRenewal(agentLoop.loop(), state, 4242, configuration, dependencies, stop), std::chrono::seconds{10}, "续期循环");
        ASSERT_TRUE(completed) << "续期循环未在时限内退出";

        EXPECT_EQ(1, probe.generate.load(std::memory_order_acquire));
        EXPECT_EQ(1, probe.renew.load(std::memory_order_acquire));
        EXPECT_EQ(1, probe.install.load(std::memory_order_acquire));
        EXPECT_EQ(1, probe.reloadHttp.load(std::memory_order_acquire));
        EXPECT_EQ(1, probe.reloadWebSocket.load(std::memory_order_acquire));
        EXPECT_EQ(std::string(64U, 'b'), state.get("cert_fingerprint").value_or(""));
        EXPECT_EQ("2027-10-25T08:00:00Z", state.get("cert_expiry").value_or(""));
    }

    TEST(AgentApplication, RenewalIsSkippedWhileCertificateIsBeyondWindow)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = openRenewalState(directory);

        AgentConfiguration configuration;
        configuration.certificateRenewBeforeDays     = 7;
        configuration.certificateRenewalCheckSeconds = 3600;

        AgentLoop         agentLoop;
        std::atomic<bool> stop{false};
        RenewalProbe      probe;
        std::atomic<int>  clockReads{0};
        const auto        fixedNow = std::chrono::system_clock::now();

        CertificateRenewalDependencies dependencies;
        dependencies.expiresAt = utcTextAt(fixedNow + std::chrono::days{30});
        dependencies.now       = [&clockReads, &stop, fixedNow]
        {
            if (clockReads.fetch_add(1, std::memory_order_acq_rel) + 1 >= 3)
            {
                stop.store(true, std::memory_order_release);
            }
            return fixedNow;
        };
        dependencies.generateRequest = [&probe]
        {
            probe.generate.fetch_add(1, std::memory_order_acq_rel);
            return std::expected<CertificateRequestMaterial, std::string>{CertificateRequestMaterial{"key-pem", "csr-pem"}};
        };
        dependencies.renew = [&probe](std::int64_t, std::string) -> AsynGyanis::Core::Task<std::expected<RenewedCertificate, CertificateRenewalFailure>>
        {
            probe.renew.fetch_add(1, std::memory_order_acq_rel);
            co_return std::unexpected(CertificateRenewalFailure{"不应发起续期", false, false, 0});
        };
        dependencies.checkInterval = std::chrono::milliseconds{10};
        dependencies.waitObserver  = [&probe](const std::chrono::milliseconds duration) { probe.recordWait(duration); };

        const bool completed =
                agentLoop.run(AgentApplication::runCertificateRenewal(agentLoop.loop(), state, 4242, configuration, dependencies, stop), std::chrono::seconds{10}, "续期循环");
        ASSERT_TRUE(completed);
        EXPECT_EQ(0, probe.generate.load(std::memory_order_acquire));
        EXPECT_EQ(0, probe.renew.load(std::memory_order_acquire));
        EXPECT_GE(probe.waitDurations().size(), 2U);
    }

    TEST(AgentApplication, RenewalRetriesWithConfiguredBackoff)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = openRenewalState(directory);

        AgentConfiguration configuration;
        configuration.certificateRenewBeforeDays     = 7;
        configuration.certificateRenewalCheckSeconds = 3600;

        AgentLoop         agentLoop;
        std::atomic<bool> stop{false};
        RenewalProbe      probe;

        CertificateRenewalDependencies dependencies;
        dependencies.expiresAt       = utcTextAt(std::chrono::system_clock::now() + std::chrono::days{1});
        dependencies.generateRequest = [] { return std::expected<CertificateRequestMaterial, std::string>{CertificateRequestMaterial{"key-pem", "csr-pem"}}; };
        dependencies.renew           = [&probe, &stop](std::int64_t, std::string) -> AsynGyanis::Core::Task<std::expected<RenewedCertificate, CertificateRenewalFailure>>
        {
            const int attempt = probe.renew.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (attempt >= 3)
            {
                stop.store(true, std::memory_order_release);
            }
            co_return std::unexpected(CertificateRenewalFailure{"证书续期返回 HTTP 429：同一节点两次签发至少间隔 60 秒", true, false, 429});
        };
        dependencies.install       = [](std::string_view) { return std::expected<AgentIdentity, std::string>{std::unexpected("重试阶段不应安装证书")}; };
        dependencies.checkInterval = std::chrono::milliseconds{10};
        dependencies.retryBackoff  = {std::chrono::milliseconds{30}, std::chrono::milliseconds{40}, std::chrono::milliseconds{50}};
        dependencies.waitObserver  = [&probe](const std::chrono::milliseconds duration) { probe.recordWait(duration); };

        const bool completed =
                agentLoop.run(AgentApplication::runCertificateRenewal(agentLoop.loop(), state, 4242, configuration, dependencies, stop), std::chrono::seconds{10}, "续期循环");
        ASSERT_TRUE(completed);
        EXPECT_EQ(3, probe.renew.load(std::memory_order_acquire));
        EXPECT_EQ(0, probe.install.load(std::memory_order_acquire));
        const auto waits = probe.waitDurations();
        ASSERT_EQ(2U, waits.size()) << "第三次失败后已请求停止，不应再等待";
        EXPECT_EQ(std::chrono::milliseconds{30}, waits[0]);
        EXPECT_EQ(std::chrono::milliseconds{40}, waits[1]);
    }

    TEST(AgentApplication, RenewalStopsOnForbiddenResponseWithoutInstalling)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = openRenewalState(directory);

        AgentConfiguration configuration;
        configuration.certificateRenewBeforeDays     = 7;
        configuration.certificateRenewalCheckSeconds = 3600;

        AgentLoop         agentLoop;
        std::atomic<bool> stop{false};
        RenewalProbe      probe;

        CertificateRenewalDependencies dependencies;
        dependencies.expiresAt       = utcTextAt(std::chrono::system_clock::now() + std::chrono::days{1});
        dependencies.generateRequest = [] { return std::expected<CertificateRequestMaterial, std::string>{CertificateRequestMaterial{"key-pem", "csr-pem"}}; };
        dependencies.renew           = [&probe](std::int64_t, std::string) -> AsynGyanis::Core::Task<std::expected<RenewedCertificate, CertificateRenewalFailure>>
        {
            probe.renew.fetch_add(1, std::memory_order_acq_rel);
            co_return std::unexpected(CertificateRenewalFailure{"证书续期被服务端拒绝（HTTP 403）：节点证书已吊销，请重新入场注册", false, true, 403});
        };
        dependencies.install = [&probe](std::string_view)
        {
            probe.install.fetch_add(1, std::memory_order_acq_rel);
            return std::expected<AgentIdentity, std::string>{std::unexpected("403 后不应安装证书")};
        };
        dependencies.waitObserver = [&probe](const std::chrono::milliseconds duration) { probe.recordWait(duration); };

        const bool completed =
                agentLoop.run(AgentApplication::runCertificateRenewal(agentLoop.loop(), state, 4242, configuration, dependencies, stop), std::chrono::seconds{10}, "续期循环");
        ASSERT_TRUE(completed) << "403 应按终止收口";
        EXPECT_EQ(1, probe.renew.load(std::memory_order_acquire));
        EXPECT_EQ(0, probe.install.load(std::memory_order_acquire));
        EXPECT_TRUE(probe.waitDurations().empty()) << "终止响应不应进入退避";
        EXPECT_FALSE(stop.load(std::memory_order_acquire));
    }

    TEST(AgentApplication, RenewalKeepsStateWhenHttpReloadFails)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = openRenewalState(directory);
        state.set("cert_fingerprint", "old-fingerprint");
        state.set("cert_expiry", "2026-10-01T00:00:00Z");

        AgentConfiguration configuration;
        configuration.certificateRenewBeforeDays     = 7;
        configuration.certificateRenewalCheckSeconds = 3600;

        AgentLoop         agentLoop;
        std::atomic<bool> stop{false};
        RenewalProbe      probe;

        CertificateRenewalDependencies dependencies;
        dependencies.expiresAt       = utcTextAt(std::chrono::system_clock::now() + std::chrono::days{1});
        dependencies.generateRequest = [] { return std::expected<CertificateRequestMaterial, std::string>{CertificateRequestMaterial{"key-pem", "csr-pem"}}; };
        dependencies.renew           = [&probe](std::int64_t, std::string) -> AsynGyanis::Core::Task<std::expected<RenewedCertificate, CertificateRenewalFailure>>
        {
            probe.renew.fetch_add(1, std::memory_order_acq_rel);
            co_return RenewedCertificate{"cert-pem", std::string(64U, 'b'), "2026-10-25 08:00:00", "2027-10-25 08:00:00", "0a"};
        };
        dependencies.install = [&probe](std::string_view)
        {
            probe.install.fetch_add(1, std::memory_order_acq_rel);
            return std::expected<AgentIdentity, std::string>{AgentIdentity{std::string(64U, 'b'), "2027-10-25T08:00:00Z"}};
        };
        dependencies.reloadHttpIdentity = [&probe, &stop]
        {
            probe.reloadHttp.fetch_add(1, std::memory_order_acq_rel);
            stop.store(true, std::memory_order_release);
            return false;
        };
        dependencies.reloadWebSocketIdentity = [&probe]
        {
            probe.reloadWebSocket.fetch_add(1, std::memory_order_acq_rel);
            return true;
        };

        const bool completed =
                agentLoop.run(AgentApplication::runCertificateRenewal(agentLoop.loop(), state, 4242, configuration, dependencies, stop), std::chrono::seconds{10}, "续期循环");
        ASSERT_TRUE(completed);
        EXPECT_EQ(1, probe.install.load(std::memory_order_acquire));
        EXPECT_EQ(1, probe.reloadHttp.load(std::memory_order_acquire));
        EXPECT_EQ(0, probe.reloadWebSocket.load(std::memory_order_acquire));
        EXPECT_EQ("old-fingerprint", state.get("cert_fingerprint").value_or(""));
        EXPECT_EQ("2026-10-01T00:00:00Z", state.get("cert_expiry").value_or(""));
    }

    TEST(AgentApplication, RenewalLoopExitsImmediatelyWhenStopWasRequested)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = openRenewalState(directory);

        AgentConfiguration configuration;
        AgentLoop          agentLoop;
        std::atomic<bool>  stop{true};
        RenewalProbe       probe;

        CertificateRenewalDependencies dependencies;
        dependencies.expiresAt       = utcTextAt(std::chrono::system_clock::now());
        dependencies.generateRequest = [&probe]
        {
            probe.generate.fetch_add(1, std::memory_order_acq_rel);
            return std::expected<CertificateRequestMaterial, std::string>{std::unexpected("已请求停止，不应生成 CSR")};
        };

        const bool completed =
                agentLoop.run(AgentApplication::runCertificateRenewal(agentLoop.loop(), state, 4242, configuration, dependencies, stop), std::chrono::seconds{5}, "续期循环");
        ASSERT_TRUE(completed);
        EXPECT_EQ(0, probe.generate.load(std::memory_order_acquire));
    }

    TEST(AgentApplication, RenewalRetriesWebSocketReloadAndLogsOnlyAfterBothChannels)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = openRenewalState(directory);

        auto sink = std::make_shared<CapturingLogSink>();
        AsynGyanis::Base::LoggerRegistry::instance().getRootLogger().clearSinks();
        AsynGyanis::Base::LoggerRegistry::instance().getRootLogger().addSink(sink);
        LoggingSinkReset loggingReset;

        AgentConfiguration configuration;
        configuration.certificateRenewBeforeDays     = 7;
        configuration.certificateRenewalCheckSeconds = 3600;

        AgentLoop         agentLoop;
        std::atomic<bool> stop{false};
        RenewalProbe      probe;

        CertificateRenewalDependencies dependencies;
        dependencies.expiresAt       = utcTextAt(std::chrono::system_clock::now() + std::chrono::days{1});
        dependencies.generateRequest = [&probe]
        {
            probe.generate.fetch_add(1, std::memory_order_acq_rel);
            return std::expected<CertificateRequestMaterial, std::string>{CertificateRequestMaterial{"key-pem", "csr-pem"}};
        };
        dependencies.renew = [&probe](std::int64_t, std::string) -> AsynGyanis::Core::Task<std::expected<RenewedCertificate, CertificateRenewalFailure>>
        {
            probe.renew.fetch_add(1, std::memory_order_acq_rel);
            co_return RenewedCertificate{"cert-pem", std::string(64U, 'b'), "2026-10-25 08:00:00", "2027-10-25 08:00:00", "0a"};
        };
        dependencies.install = [&probe](std::string_view)
        {
            probe.install.fetch_add(1, std::memory_order_acq_rel);
            return std::expected<AgentIdentity, std::string>{AgentIdentity{std::string(64U, 'b'), "2027-10-25T08:00:00Z"}};
        };
        dependencies.reloadHttpIdentity = [&probe]
        {
            probe.reloadHttp.fetch_add(1, std::memory_order_acq_rel);
            return true;
        };
        dependencies.reloadWebSocketIdentity = [&probe, &stop]
        {
            const int attempt = probe.reloadWebSocket.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (attempt >= 2)
            {
                stop.store(true, std::memory_order_release);
                return true;
            }
            return false;
        };
        dependencies.commitInstalledIdentity = [&probe] { probe.commit.fetch_add(1, std::memory_order_acq_rel); };
        // 常规检查间隔远大于复查间隔：重试若错用了检查间隔，下面的耗时断言会失败
        dependencies.checkInterval                = std::chrono::seconds{5};
        dependencies.websocketReloadRetryInterval = std::chrono::milliseconds{10};
        dependencies.waitObserver                 = [&probe](const std::chrono::milliseconds duration) { probe.recordWait(duration); };

        const auto started = std::chrono::steady_clock::now();
        const bool completed =
                agentLoop.run(AgentApplication::runCertificateRenewal(agentLoop.loop(), state, 4242, configuration, dependencies, stop), std::chrono::seconds{10}, "续期循环");
        const auto elapsed = std::chrono::steady_clock::now() - started;
        ASSERT_TRUE(completed) << "WebSocket 重载失败后应按复查间隔重试直到成功";
        EXPECT_LT(elapsed, std::chrono::seconds{2}) << "WebSocket 复查不能等到下一次常规检查";

        EXPECT_EQ(1, probe.generate.load(std::memory_order_acquire));
        EXPECT_EQ(1, probe.renew.load(std::memory_order_acquire));
        EXPECT_EQ(1, probe.install.load(std::memory_order_acquire));
        EXPECT_EQ(2, probe.reloadWebSocket.load(std::memory_order_acquire));
        EXPECT_EQ(1, probe.commit.load(std::memory_order_acquire)) << "两条通道都换好后才能清理备份";
        EXPECT_EQ(std::string(64U, 'b'), state.get("cert_fingerprint").value_or(""));
        EXPECT_EQ("2027-10-25T08:00:00Z", state.get("cert_expiry").value_or(""));

        EXPECT_TRUE(sink->contains(AsynGyanis::Base::LogLevel::Warn, "WebSocket 通道未能装载新证书")) << "WebSocket 失败必须有明确告警";
        EXPECT_TRUE(sink->contains(AsynGyanis::Base::LogLevel::Info, "WebSocket 通道已切换到新证书")) << "重试成功必须记一条成功日志";
        EXPECT_EQ(1U, sink->count("证书续期完成")) << "成功日志只能在两条通道都换好之后出现一次";
        for (const auto &message: sink->messages())
        {
            EXPECT_EQ(std::string::npos, message.find("key-pem")) << "日志不得包含私钥正文";
            EXPECT_EQ(std::string::npos, message.find("csr-pem")) << "日志不得包含 CSR 正文";
            EXPECT_EQ(std::string::npos, message.find("BEGIN")) << "日志不得包含 PEM 块";
        }
    }

    TEST(AgentApplication, RenewalDoesNotRepeatAfterSuccessOnNextTick)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = openRenewalState(directory);

        AgentConfiguration configuration;
        configuration.certificateRenewBeforeDays     = 7;
        configuration.certificateRenewalCheckSeconds = 3600;

        AgentLoop         agentLoop;
        std::atomic<bool> stop{false};
        RenewalProbe      probe;
        std::atomic<int>  clockReads{0};
        const auto        fixedNow = std::chrono::system_clock::now();

        CertificateRenewalDependencies dependencies;
        dependencies.expiresAt = utcTextAt(fixedNow + std::chrono::days{1});
        dependencies.now       = [&clockReads, &stop, fixedNow]
        {
            if (clockReads.fetch_add(1, std::memory_order_acq_rel) + 1 >= 2)
            {
                stop.store(true, std::memory_order_release);
            }
            return fixedNow;
        };
        dependencies.generateRequest = [&probe]
        {
            probe.generate.fetch_add(1, std::memory_order_acq_rel);
            return std::expected<CertificateRequestMaterial, std::string>{CertificateRequestMaterial{"key-pem", "csr-pem"}};
        };
        dependencies.renew = [&probe](std::int64_t, std::string) -> AsynGyanis::Core::Task<std::expected<RenewedCertificate, CertificateRenewalFailure>>
        {
            probe.renew.fetch_add(1, std::memory_order_acq_rel);
            co_return RenewedCertificate{"cert-pem", std::string(64U, 'b'), "2026-10-25 08:00:00", "2027-10-25 08:00:00", "0a"};
        };
        dependencies.install = [&probe](std::string_view)
        {
            probe.install.fetch_add(1, std::memory_order_acq_rel);
            return std::expected<AgentIdentity, std::string>{AgentIdentity{std::string(64U, 'b'), "2027-10-25T08:00:00Z"}};
        };
        dependencies.reloadHttpIdentity = [&probe]
        {
            probe.reloadHttp.fetch_add(1, std::memory_order_acq_rel);
            return true;
        };
        dependencies.reloadWebSocketIdentity = [&probe]
        {
            probe.reloadWebSocket.fetch_add(1, std::memory_order_acq_rel);
            return true;
        };
        dependencies.commitInstalledIdentity = [&probe] { probe.commit.fetch_add(1, std::memory_order_acq_rel); };
        dependencies.checkInterval           = std::chrono::milliseconds{10};
        dependencies.waitObserver            = [&probe](const std::chrono::milliseconds duration) { probe.recordWait(duration); };

        const bool completed =
                agentLoop.run(AgentApplication::runCertificateRenewal(agentLoop.loop(), state, 4242, configuration, dependencies, stop), std::chrono::seconds{10}, "续期循环");
        ASSERT_TRUE(completed);

        // 换回的新证书离到期还有一年，下一轮检查只能等待，不能再签发一次
        EXPECT_EQ(2, clockReads.load(std::memory_order_acquire));
        EXPECT_EQ(1, probe.generate.load(std::memory_order_acquire));
        EXPECT_EQ(1, probe.renew.load(std::memory_order_acquire));
        EXPECT_EQ(1, probe.install.load(std::memory_order_acquire));
        EXPECT_EQ(1, probe.reloadHttp.load(std::memory_order_acquire));
        EXPECT_EQ(1, probe.reloadWebSocket.load(std::memory_order_acquire));
        EXPECT_EQ(1, probe.commit.load(std::memory_order_acquire));
        EXPECT_FALSE(probe.waitDurations().empty()) << "成功后必须等待下一次检查";
    }

    TEST(AgentApplication, RenewalStopsDuringMultiSliceWait)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = openRenewalState(directory);

        AgentConfiguration configuration;
        configuration.certificateRenewBeforeDays     = 7;
        configuration.certificateRenewalCheckSeconds = 3600;

        AgentLoop         agentLoop;
        std::atomic<bool> stop{false};
        RenewalProbe      probe;

        std::mutex              mutex;
        std::condition_variable waiting;
        bool                    enteredWait = false;

        CertificateRenewalDependencies dependencies;
        dependencies.expiresAt     = utcTextAt(std::chrono::system_clock::now() + std::chrono::days{30});
        dependencies.checkInterval = std::chrono::seconds{5};
        dependencies.waitObserver  = [&probe, &mutex, &waiting, &enteredWait](const std::chrono::milliseconds duration)
        {
            probe.recordWait(duration);
            {
                const std::lock_guard lock(mutex);
                enteredWait = true;
            }
            waiting.notify_all();
        };

        // 停止请求必须落在等待进行中；观察点报出计划时长后才置位，五秒等待要靠一秒切片才能及时退出
        std::jthread stopper(
                [&mutex, &waiting, &enteredWait, &stop]
                {
                    std::unique_lock lock(mutex);
                    waiting.wait(lock, [&enteredWait] { return enteredWait; });
                    stop.store(true, std::memory_order_release);
                });

        const auto started = std::chrono::steady_clock::now();
        const bool completed =
                agentLoop.run(AgentApplication::runCertificateRenewal(agentLoop.loop(), state, 4242, configuration, dependencies, stop), std::chrono::seconds{20}, "续期循环");
        const auto elapsed = std::chrono::steady_clock::now() - started;

        ASSERT_TRUE(completed) << "多切片等待期间收到停止请求必须退出";
        EXPECT_LT(elapsed, std::chrono::seconds{4}) << "五秒等待未按一秒切片响应停止";
        EXPECT_EQ(0, probe.generate.load(std::memory_order_acquire));
        const auto waits = probe.waitDurations();
        ASSERT_FALSE(waits.empty());
        EXPECT_EQ(std::chrono::seconds{5}, waits.front());
    }

    TEST(AgentApplication, RenewalRetriesInvalidRequestWithLongerCap)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = openRenewalState(directory);

        auto sink = std::make_shared<CapturingLogSink>();
        AsynGyanis::Base::LoggerRegistry::instance().getRootLogger().clearSinks();
        AsynGyanis::Base::LoggerRegistry::instance().getRootLogger().addSink(sink);
        LoggingSinkReset loggingReset;

        AgentConfiguration configuration;
        configuration.certificateRenewBeforeDays     = 7;
        configuration.certificateRenewalCheckSeconds = 3600;

        AgentLoop         agentLoop;
        std::atomic<bool> stop{false};
        RenewalProbe      probe;

        CertificateRenewalDependencies dependencies;
        dependencies.expiresAt       = utcTextAt(std::chrono::system_clock::now() + std::chrono::days{1});
        dependencies.generateRequest = [] { return std::expected<CertificateRequestMaterial, std::string>{CertificateRequestMaterial{"key-pem", "csr-pem"}}; };
        dependencies.renew           = [&probe](std::int64_t, std::string) -> AsynGyanis::Core::Task<std::expected<RenewedCertificate, CertificateRenewalFailure>>
        {
            probe.renew.fetch_add(1, std::memory_order_acq_rel);
            co_return std::unexpected(CertificateRenewalFailure{"证书续期返回 HTTP 400：CSR 不合法", true, false, 400});
        };
        dependencies.install       = [](std::string_view) { return std::expected<AgentIdentity, std::string>{std::unexpected("400 后不应安装证书")}; };
        dependencies.checkInterval = std::chrono::milliseconds{10};
        dependencies.retryBackoff  = {std::chrono::hours{2}};
        dependencies.waitObserver  = [&probe, &stop](const std::chrono::milliseconds duration)
        {
            probe.recordWait(duration);
            stop.store(true, std::memory_order_release);
        };

        const bool completed =
                agentLoop.run(AgentApplication::runCertificateRenewal(agentLoop.loop(), state, 4242, configuration, dependencies, stop), std::chrono::seconds{10}, "续期循环");
        ASSERT_TRUE(completed) << "400 必须按退避重试而不是终止";

        EXPECT_EQ(1, probe.renew.load(std::memory_order_acquire));
        const auto waits = probe.waitDurations();
        ASSERT_EQ(1U, waits.size());
        EXPECT_EQ(std::chrono::hours{1}, waits.front()) << "400 的退避上限应放宽到一小时";
        EXPECT_TRUE(sink->contains(AsynGyanis::Base::LogLevel::Error, "HTTP 400")) << "400 必须按 ERROR 记录";
    }

    TEST(AgentApplication, RenewalSurvivesDependencyException)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = openRenewalState(directory);

        AgentConfiguration configuration;
        configuration.certificateRenewBeforeDays     = 7;
        configuration.certificateRenewalCheckSeconds = 3600;

        AgentLoop         agentLoop;
        std::atomic<bool> stop{false};
        RenewalProbe      probe;

        CertificateRenewalDependencies dependencies;
        dependencies.expiresAt       = utcTextAt(std::chrono::system_clock::now() + std::chrono::days{1});
        dependencies.generateRequest = [&probe]
        {
            if (probe.generate.fetch_add(1, std::memory_order_acq_rel) + 1 == 1)
            {
                throw std::runtime_error{"生成器临时故障"};
            }
            return std::expected<CertificateRequestMaterial, std::string>{CertificateRequestMaterial{"key-pem", "csr-pem"}};
        };
        dependencies.renew = [&probe, &stop](std::int64_t, std::string) -> AsynGyanis::Core::Task<std::expected<RenewedCertificate, CertificateRenewalFailure>>
        {
            const int attempt = probe.renew.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (attempt == 1)
            {
                // 协程体内抛出的异常会沿 co_await 回到续期循环，必须由循环层兜住
                throw std::runtime_error{"续期请求临时故障"};
            }
            stop.store(true, std::memory_order_release);
            co_return RenewedCertificate{"cert-pem", std::string(64U, 'b'), "2026-10-25 08:00:00", "2027-10-25 08:00:00", "0a"};
        };
        dependencies.install = [&probe](std::string_view)
        {
            probe.install.fetch_add(1, std::memory_order_acq_rel);
            return std::expected<AgentIdentity, std::string>{AgentIdentity{std::string(64U, 'b'), "2027-10-25T08:00:00Z"}};
        };
        dependencies.checkInterval = std::chrono::milliseconds{10};
        dependencies.retryBackoff  = {std::chrono::milliseconds{10}};

        const bool completed =
                agentLoop.run(AgentApplication::runCertificateRenewal(agentLoop.loop(), state, 4242, configuration, dependencies, stop), std::chrono::seconds{10}, "续期循环");
        ASSERT_TRUE(completed) << "依赖抛异常不能让续期协程直接结束";

        EXPECT_EQ(3, probe.generate.load(std::memory_order_acquire)) << "两次异常后的重试都要重新生成材料";
        EXPECT_EQ(2, probe.renew.load(std::memory_order_acquire)) << "续期协程异常后应重试一次";
        EXPECT_EQ(1, probe.install.load(std::memory_order_acquire));
    }
} // namespace EdgePulse::Agent
