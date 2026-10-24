#include "AgentClient.h"
#include "AgentConfiguration.h"
#include "AgentIdentity.h"
#include "AgentState.h"
#include "Executors/CommandExecutor.h"
#include "Executors/ConfigExecutor.h"
#include "Executors/DiagnosticsExecutor.h"
#include "Executors/RestartExecutor.h"
#include "ProcessRunner.h"
#include "TestSupport/TemporaryDirectory.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Runtime/Environment.h"
#include "Platform/FileSystem/FileSystem.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "TestSupport/HttpClientHarness.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef EDGEPULSE_AGENT_TEST_HOOK_PATH
#error "用例需要 EDGEPULSE_AGENT_TEST_HOOK_PATH 指向测试钩子可执行文件"
#endif

namespace EdgePulse::Agent
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;
        using AsynGyanis::Core::EventLoop;
        using AsynGyanis::Core::Task;

        /**
         * @brief 自持事件循环与协程帧的执行器
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

            EventLoop           m_loop;
            std::jthread        m_thread;
            std::vector<Task<>> m_frames;
        };

        /**
         * @brief 在用例离开作用域时先停掉 Agent 循环
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
            AgentLoop &m_loop;
        };

        /**
         * @brief 在用例存续期间设置环境变量，析构时恢复原值
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
            void assign(const std::string &value) const
            {
#if defined(_WIN32)
                _putenv_s(m_name.c_str(), value.c_str());
#else
                ::setenv(m_name.c_str(), value.c_str(), 1);
#endif
            }

            void remove() const
            {
#if defined(_WIN32)
                _putenv_s(m_name.c_str(), "");
#else
                ::unsetenv(m_name.c_str());
#endif
            }

            std::string                m_name;
            std::optional<std::string> m_previous;
        };

        [[nodiscard]] AgentConfiguration plaintextConfiguration(const std::uint16_t port)
        {
            AgentConfiguration configuration;
            configuration.baseUrl                = "http://127.0.0.1:" + std::to_string(port);
            configuration.allowPlaintextForTests = true;
            configuration.name                   = "edge-01";
            configuration.tenantCode             = "tenant-a";
            configuration.requestTimeoutSeconds  = 10;
            return configuration;
        }

        [[nodiscard]] AgentIdentity testIdentity()
        {
            AgentIdentity identity;
            identity.fingerprint = std::string(64, 'a');
            identity.expiresAt   = "2030-01-01T00:00:00Z";
            return identity;
        }

        [[nodiscard]] std::string readTextFile(const std::filesystem::path &path)
        {
            std::ifstream stream(path, std::ios::binary);
            return std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        }

        [[nodiscard]] std::string trimmedTrailingNewlines(std::string text)
        {
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
            {
                text.pop_back();
            }
            return text;
        }

        struct ConfigCapture
        {
            std::mutex               mutex;
            std::vector<std::string> reportBodies;
            std::atomic<int>         reportStatus{200};
        };

        void installConfigRoutes(AsynGyanis::Net::Router &router, const std::shared_ptr<ConfigCapture> &capture, const std::int64_t version, const ConfigValue &content)
        {
            router.get("/api/agent/nodes/:id/config",
                       [version, content](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> Task<>
                       {
                           const ConfigValue envelope =
                                   ConfigValue::object({{"code", 200}, {"message", "ok"}, {"data", ConfigValue::object({{"version", version}, {"content", content}, {"hash", "abc"}})}, {"traceId", "t"}});
                           response.setStatus(200);
                           response.setBody(envelope.dump());
                           co_return;
                       });
            router.post("/api/agent/nodes/:id/config/report",
                        [capture](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> Task<>
                        {
                            {
                                const std::lock_guard lock(capture->mutex);
                                capture->reportBodies.push_back(std::string{request.body()});
                            }
                            const int status = capture->reportStatus.load(std::memory_order_acquire);
                            response.setStatus(status);
                            response.setBody(status == 200 ? R"({"code":200,"message":"ok","data":{},"traceId":"t"})"
                                                           : R"({"code":500,"message":"暂时失败","data":null,"traceId":"t"})");
                            co_return;
                        });
        }

        [[nodiscard]] std::vector<std::string> reportBodies(const std::shared_ptr<ConfigCapture> &capture)
        {
            const std::lock_guard lock(capture->mutex);
            return capture->reportBodies;
        }

        [[nodiscard]] std::size_t lineCount(const std::filesystem::path &path)
        {
            std::ifstream     stream(path);
            std::size_t       lines = 0;
            std::string       line;
            while (std::getline(stream, line))
            {
                ++lines;
            }
            return lines;
        }
    } // namespace

    TEST(AgentExecutors, ConfigApplyWritesCanonicalFileRunsHookAndReportsApplied)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            capture = std::make_shared<ConfigCapture>();
        const ConfigValue               content = ConfigValue::object({{"nested", ConfigValue::object({{"z", true}})}, {"a", 1}});

        TestSupport::ServerHarness server([capture, content](AsynGyanis::Net::Router &router, EventLoop &) { installConfigRoutes(router, capture, 3, content); });

        AgentConfiguration configuration = plaintextConfiguration(server.port());
        configuration.configPath          = directory.path() / "config" / "node.json";
        configuration.configApplyHook     = std::filesystem::path{EDGEPULSE_AGENT_TEST_HOOK_PATH};
        const auto markerPath             = directory.path() / "hook-marker.txt";
        const ScopedEnvironmentVariable marker("EDGEPULSE_AGENT_TEST_HOOK_MARKER", markerPath.string());

        AgentLoop     loop;
        AgentClient   client(loop.loop(), configuration, testIdentity());
        ConfigExecutor executor(client, configuration, 77);
        LoopStopGuard stopGuard{loop};

        const auto outcome = loop.run(executor.execute(loop.loop(), AgentCommand{1, "config.apply", R"({"version":3})", 30}), std::chrono::seconds{20}, "配置应用");
        ASSERT_TRUE(outcome.has_value());
        EXPECT_TRUE(outcome->succeeded) << outcome->message;
        EXPECT_TRUE(executor.supports("config.apply"));

        const auto written = AsynGyanis::Base::parseConfigValue(readTextFile(configuration.configPath));
        ASSERT_TRUE(written.has_value());
        EXPECT_EQ(content, *written);

        const std::string markerText = readTextFile(markerPath);
        EXPECT_NE(std::string::npos, markerText.find(AsynGyanis::Platform::FileSystem::utf8FromPath(configuration.configPath)));
        EXPECT_EQ(1U, lineCount(markerPath)) << "钩子必须只收到配置路径一个参数";

        const auto bodies = reportBodies(capture);
        ASSERT_EQ(1U, bodies.size());
        const auto report = AsynGyanis::Base::parseConfigValue(bodies.front());
        ASSERT_TRUE(report.has_value()) << bodies.front();
        EXPECT_EQ(3, report->value("version", 0));
        EXPECT_EQ("applied", report->value("status", std::string{}));
        EXPECT_TRUE(report->at("content").is_object());
        EXPECT_EQ(content, report->at("content"));
    }

    TEST(AgentExecutors, ConfigApplyRejectsMalformedPayloadWithoutWriting)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            capture = std::make_shared<ConfigCapture>();
        TestSupport::ServerHarness      server([capture](AsynGyanis::Net::Router &router, EventLoop &) { installConfigRoutes(router, capture, 3, ConfigValue::object()); });

        AgentConfiguration configuration = plaintextConfiguration(server.port());
        configuration.configPath          = directory.path() / "node.json";
        static_cast<void>(directory.write("node.json", "old"));

        AgentLoop     loop;
        AgentClient   client(loop.loop(), configuration, testIdentity());
        ConfigExecutor executor(client, configuration, 77);
        LoopStopGuard stopGuard{loop};

        for (const std::string &payload: {std::string{"not-json"}, std::string{"{}"}, std::string{R"({"version":-1})"}, std::string{R"({"version":"3"})"}})
        {
            const auto outcome = loop.run(executor.execute(loop.loop(), AgentCommand{1, "config.apply", payload, 30}), std::chrono::seconds{20}, "畸形命令");
            ASSERT_TRUE(outcome.has_value()) << payload;
            EXPECT_FALSE(outcome->succeeded) << payload;
            EXPECT_FALSE(outcome->message.empty()) << payload;
            EXPECT_EQ("old", readTextFile(configuration.configPath)) << payload;
        }
        EXPECT_TRUE(reportBodies(capture).empty()) << "正文不合法时没有可上报的版本";
    }

    TEST(AgentExecutors, ConfigApplyRejectsNonObjectOversizeAndVersionMismatch)
    {
        struct Case
        {
            std::int64_t version;
            ConfigValue  content;
            const char  *payload;
        };
        const std::vector<Case> cases{
                {3, ConfigValue::array({1, 2}), R"({"version":3})"},
                {3, ConfigValue::object({{"blob", std::string(4096, 'x')}}), R"({"version":3})"},
                {2, ConfigValue::object({{"a", 1}}), R"({"version":3})"},
        };

        for (const Case &testCase: cases)
        {
            TestSupport::TemporaryDirectory directory;
            auto                            capture = std::make_shared<ConfigCapture>();
            TestSupport::ServerHarness      server([capture, &testCase](AsynGyanis::Net::Router &router, EventLoop &) { installConfigRoutes(router, capture, testCase.version, testCase.content); });

            AgentConfiguration configuration = plaintextConfiguration(server.port());
            configuration.configPath          = directory.path() / "node.json";
            configuration.maximumConfigBytes  = 1024;
            static_cast<void>(directory.write("node.json", "old"));

            AgentLoop     loop;
            AgentClient   client(loop.loop(), configuration, testIdentity());
            ConfigExecutor executor(client, configuration, 77);
            LoopStopGuard stopGuard{loop};

            const auto outcome = loop.run(executor.execute(loop.loop(), AgentCommand{1, "config.apply", testCase.payload, 30}), std::chrono::seconds{20}, "拒绝用例");
            ASSERT_TRUE(outcome.has_value());
            EXPECT_FALSE(outcome->succeeded);
            EXPECT_FALSE(outcome->message.empty());
            EXPECT_EQ("old", readTextFile(configuration.configPath));

            const auto bodies = reportBodies(capture);
            ASSERT_EQ(1U, bodies.size());
            const auto report = AsynGyanis::Base::parseConfigValue(bodies.front());
            ASSERT_TRUE(report.has_value());
            EXPECT_EQ("failed", report->value("status", std::string{}));
        }
    }

    TEST(AgentExecutors, ConfigApplyWriteFailureLeavesTargetUntouched)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            capture = std::make_shared<ConfigCapture>();
        TestSupport::ServerHarness      server([capture](AsynGyanis::Net::Router &router, EventLoop &) { installConfigRoutes(router, capture, 3, ConfigValue::object({{"a", 1}})); });

        const auto blockingFile = directory.write("blocked", "not a directory");
        AgentConfiguration configuration = plaintextConfiguration(server.port());
        configuration.configPath          = blockingFile / "node.json";

        AgentLoop     loop;
        AgentClient   client(loop.loop(), configuration, testIdentity());
        ConfigExecutor executor(client, configuration, 77);
        LoopStopGuard stopGuard{loop};

        const auto outcome = loop.run(executor.execute(loop.loop(), AgentCommand{1, "config.apply", R"({"version":3})", 30}), std::chrono::seconds{20}, "写入失败");
        ASSERT_TRUE(outcome.has_value());
        EXPECT_FALSE(outcome->succeeded);
        EXPECT_FALSE(outcome->message.empty());
        ASSERT_EQ(1U, reportBodies(capture).size());
        EXPECT_EQ("failed", AsynGyanis::Base::parseConfigValue(reportBodies(capture).front())->value("status", std::string{}));
    }

    TEST(AgentExecutors, ConfigApplyHookFailureRestoresOldFileByteIdentically)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            capture = std::make_shared<ConfigCapture>();
        TestSupport::ServerHarness      server([capture](AsynGyanis::Net::Router &router, EventLoop &) { installConfigRoutes(router, capture, 3, ConfigValue::object({{"new", true}})); });

        AgentConfiguration configuration = plaintextConfiguration(server.port());
        configuration.configPath          = directory.path() / "node.json";
        configuration.configApplyHook     = std::filesystem::path{EDGEPULSE_AGENT_TEST_HOOK_PATH};
        const std::string oldContent      = "old\r\ncontent\x01";
        static_cast<void>(directory.write("node.json", oldContent));
        const auto markerPath = directory.path() / "hook-marker.txt";
        const ScopedEnvironmentVariable marker("EDGEPULSE_AGENT_TEST_HOOK_MARKER", markerPath.string());
        const ScopedEnvironmentVariable exitCode("EDGEPULSE_AGENT_TEST_HOOK_EXIT", "1");

        AgentLoop     loop;
        AgentClient   client(loop.loop(), configuration, testIdentity());
        ConfigExecutor executor(client, configuration, 77);
        LoopStopGuard stopGuard{loop};

        const auto outcome = loop.run(executor.execute(loop.loop(), AgentCommand{1, "config.apply", R"({"version":3})", 30}), std::chrono::seconds{20}, "钩子失败");
        ASSERT_TRUE(outcome.has_value());
        EXPECT_FALSE(outcome->succeeded);
        EXPECT_NE(std::string::npos, outcome->message.find("钩子"));
        EXPECT_EQ(oldContent, readTextFile(configuration.configPath));
        EXPECT_FALSE(std::filesystem::exists(configuration.configPath.string() + ".backup"));

        const auto bodies = reportBodies(capture);
        ASSERT_EQ(1U, bodies.size());
        const auto report = AsynGyanis::Base::parseConfigValue(bodies.front());
        ASSERT_TRUE(report.has_value());
        EXPECT_EQ("failed", report->value("status", std::string{}));
        EXPECT_EQ(3, report->value("version", 0));
    }

    TEST(AgentExecutors, ConfigApplyHookFailureKeepsBackupWhenRestoreFails)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            capture = std::make_shared<ConfigCapture>();
        TestSupport::ServerHarness      server([capture](AsynGyanis::Net::Router &router, EventLoop &) { installConfigRoutes(router, capture, 3, ConfigValue::object({{"new", true}})); });

        AgentConfiguration configuration = plaintextConfiguration(server.port());
        configuration.configPath          = directory.path() / "node.json";
        configuration.configApplyHook     = std::filesystem::path{EDGEPULSE_AGENT_TEST_HOOK_PATH};
        const std::string oldContent      = "old\r\ncontent\x01";
        static_cast<void>(directory.write("node.json", oldContent));
        const auto markerPath = directory.path() / "hook-marker.txt";
        const ScopedEnvironmentVariable marker("EDGEPULSE_AGENT_TEST_HOOK_MARKER", markerPath.string());
        const ScopedEnvironmentVariable exitCode("EDGEPULSE_AGENT_TEST_HOOK_EXIT", "1");
        const ScopedEnvironmentVariable occupy("EDGEPULSE_AGENT_TEST_HOOK_REPLACE_WITH_DIRECTORY", "1");

        AgentLoop     loop;
        AgentClient   client(loop.loop(), configuration, testIdentity());
        ConfigExecutor executor(client, configuration, 77);
        LoopStopGuard stopGuard{loop};

        const auto outcome = loop.run(executor.execute(loop.loop(), AgentCommand{1, "config.apply", R"({"version":3})", 30}), std::chrono::seconds{20}, "恢复失败");
        ASSERT_TRUE(outcome.has_value());
        EXPECT_FALSE(outcome->succeeded);
        EXPECT_NE(std::string::npos, outcome->message.find("钩子"));

        std::filesystem::path backupPath = configuration.configPath;
        backupPath += ".backup";
        ASSERT_TRUE(std::filesystem::exists(backupPath)) << "恢复旧配置失败时不能删除唯一副本";
        EXPECT_EQ(oldContent, readTextFile(backupPath));
        EXPECT_NE(std::string::npos, outcome->message.find(AsynGyanis::Platform::FileSystem::utf8FromPath(backupPath)))
                << "失败原因要给出备份路径：" << outcome->message;

        const auto bodies = reportBodies(capture);
        ASSERT_EQ(1U, bodies.size());
        const auto report = AsynGyanis::Base::parseConfigValue(bodies.front());
        ASSERT_TRUE(report.has_value());
        EXPECT_EQ("failed", report->value("status", std::string{}));
    }

    TEST(AgentExecutors, DiagnosticsReportsBoundedStateMetricsAndLogTail)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = AgentState::open(directory.path() / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();
        state->set("desired_config_version", "5");
        state->enqueueTelemetry(R"({"m":1})");
        state->enqueueTelemetry(R"({"m":2})");
        state->enqueueResult(11, R"({"status":"succeeded"})");

        const std::string      logText = std::string(300, 'a') + "TAIL-END";
        const auto             logPath = directory.write("agent.log", logText);
        AgentConfiguration     configuration = plaintextConfiguration(1);
        configuration.logFile                = logPath;
        configuration.diagnosticsLogTailBytes = 64;
        std::atomic<int> calls{0};
        DiagnosticsExecutor executor(*state, configuration, "9.9.9",
                                     [&calls]()
                                     {
                                         calls.fetch_add(1, std::memory_order_release);
                                         return std::string{R"([{"name":"cpu.usage","value":12.5,"unit":"%"}])"};
                                     });

        AgentLoop     loop;
        LoopStopGuard stopGuard{loop};
        const auto    outcome = loop.run(executor.execute(loop.loop(), AgentCommand{1, "diagnostics.collect", "{}", 30}), std::chrono::seconds{20}, "诊断采集");
        ASSERT_TRUE(outcome.has_value());
        EXPECT_TRUE(outcome->succeeded) << outcome->message;
        EXPECT_EQ(1, calls.load(std::memory_order_acquire));

        const auto diagnostics = AsynGyanis::Base::parseConfigValue(outcome->result);
        ASSERT_TRUE(diagnostics.has_value()) << outcome->result;
        EXPECT_EQ("9.9.9", diagnostics->value("agentVersion", std::string{}));
        EXPECT_EQ("edge-01", diagnostics->value("nodeName", std::string{}));
        EXPECT_EQ(5, diagnostics->value("configVersion", 0));
        EXPECT_EQ(2, diagnostics->at("queueDepth").value("telemetry", 0));
        EXPECT_EQ(1, diagnostics->at("queueDepth").value("results", 0));
        ASSERT_EQ(1U, diagnostics->at("metrics").size());
        EXPECT_EQ("cpu.usage", diagnostics->at("metrics")[0].value("name", std::string{}));
        const std::string tail = diagnostics->value("logTail", std::string{});
        EXPECT_LE(tail.size(), 64U);
        EXPECT_NE(std::string::npos, tail.find("TAIL-END"));
        EXPECT_LE(outcome->result.size(), 256U * 1024U);

        state->enqueueTelemetry(R"({"m":3})");
        const auto refreshedOutcome = loop.run(executor.execute(loop.loop(), AgentCommand{2, "diagnostics.collect", "{}", 30}), std::chrono::seconds{20}, "再次诊断");
        ASSERT_TRUE(refreshedOutcome.has_value());
        ASSERT_TRUE(refreshedOutcome->succeeded) << refreshedOutcome->message;
        EXPECT_EQ(2, calls.load(std::memory_order_acquire));
        const auto refreshed = AsynGyanis::Base::parseConfigValue(refreshedOutcome->result);
        ASSERT_TRUE(refreshed.has_value()) << refreshedOutcome->result;
        EXPECT_EQ(5, refreshed->value("configVersion", 0));
        EXPECT_EQ(3, refreshed->at("queueDepth").value("telemetry", 0)) << "队列深度必须在每次采集时从状态库重新读取";
        EXPECT_EQ(1, refreshed->at("queueDepth").value("results", 0));
    }

    TEST(AgentExecutors, DiagnosticsDegradesWhenProvidersAndLogAreMissing)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = AgentState::open(directory.path() / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();
        AgentConfiguration configuration = plaintextConfiguration(1);
        configuration.logFile            = directory.path() / "missing.log";
        DiagnosticsExecutor executor(*state, configuration, "1.0.0", {});

        AgentLoop     loop;
        LoopStopGuard stopGuard{loop};
        const auto    outcome = loop.run(executor.execute(loop.loop(), AgentCommand{1, "diagnostics.collect", "{}", 30}), std::chrono::seconds{20}, "缺失提供者");
        ASSERT_TRUE(outcome.has_value());
        EXPECT_TRUE(outcome->succeeded) << outcome->message;
        const auto diagnostics = AsynGyanis::Base::parseConfigValue(outcome->result);
        ASSERT_TRUE(diagnostics.has_value());
        EXPECT_TRUE(diagnostics->at("metrics").is_array());
        EXPECT_TRUE(diagnostics->at("metrics").empty());
        EXPECT_EQ("", diagnostics->value("logTail", std::string{}));
    }

    TEST(AgentExecutors, DiagnosticsOutputStaysUnderLimitWithLargeLogTail)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = AgentState::open(directory.path() / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();
        const auto             logPath = directory.write("agent.log", std::string(400U * 1024U, 'y'));
        AgentConfiguration     configuration = plaintextConfiguration(1);
        configuration.logFile                = logPath;
        configuration.diagnosticsLogTailBytes = 400U * 1024U;
        DiagnosticsExecutor executor(*state, configuration, "1.0.0", [] { return std::string{"[]"}; });

        AgentLoop     loop;
        LoopStopGuard stopGuard{loop};
        const auto    outcome = loop.run(executor.execute(loop.loop(), AgentCommand{1, "diagnostics.collect", "{}", 30}), std::chrono::seconds{20}, "大日志尾");
        ASSERT_TRUE(outcome.has_value());
        EXPECT_TRUE(outcome->succeeded);
        EXPECT_LE(outcome->result.size(), 256U * 1024U);
        const auto diagnostics = AsynGyanis::Base::parseConfigValue(outcome->result);
        ASSERT_TRUE(diagnostics.has_value());
        EXPECT_LE(diagnostics->value("logTail", std::string{}).size(), 256U * 1024U);
    }

    TEST(AgentExecutors, RestartInvokesInjectedCallbackOnly)
    {
        std::atomic<bool> restarted{false};
        RestartExecutor   executor([&restarted] { restarted.store(true, std::memory_order_release); });
        EXPECT_TRUE(executor.supports("agent.restart"));

        AgentLoop     loop;
        LoopStopGuard stopGuard{loop};
        const auto    outcome = loop.run(executor.execute(loop.loop(), AgentCommand{1, "agent.restart", "{}", 30}), std::chrono::seconds{10}, "重启命令");
        ASSERT_TRUE(outcome.has_value());
        EXPECT_TRUE(outcome->succeeded);
        EXPECT_TRUE(restarted.load(std::memory_order_acquire));
    }

    TEST(AgentExecutors, ProcessRunnerSucceedsAndPreservesArgumentsWithoutShell)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      marker = directory.path() / "process-marker.txt";
        const ScopedEnvironmentVariable markerEnv("EDGEPULSE_AGENT_TEST_HOOK_MARKER", marker.string());
        const std::string               injected = "a & echo injected";
        const ProcessRunResult          result = runProcess(std::filesystem::path{EDGEPULSE_AGENT_TEST_HOOK_PATH}, {injected}, std::chrono::seconds{10});
        EXPECT_TRUE(result.succeeded()) << result.failure;
        EXPECT_FALSE(result.timedOut);
        EXPECT_EQ(0, result.exitCode);
        EXPECT_EQ(injected + "\n", readTextFile(marker)) << "参数必须原样交给可执行文件，不能经 shell 展开";
    }

    TEST(AgentExecutors, ProcessRunnerReportsNonzeroExit)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      marker = directory.path() / "process-marker.txt";
        const ScopedEnvironmentVariable markerEnv("EDGEPULSE_AGENT_TEST_HOOK_MARKER", marker.string());
        const ScopedEnvironmentVariable exitEnv("EDGEPULSE_AGENT_TEST_HOOK_EXIT", "1");
        const ProcessRunResult          result = runProcess(std::filesystem::path{EDGEPULSE_AGENT_TEST_HOOK_PATH}, {}, std::chrono::seconds{10});
        EXPECT_FALSE(result.succeeded());
        EXPECT_EQ(1, result.exitCode);
        EXPECT_NE(std::string::npos, result.failure.find("1"));
    }

    TEST(AgentExecutors, ProcessRunnerKillsOnTimeout)
    {
        const auto             started = std::chrono::steady_clock::now();
        const ProcessRunResult result  = runProcess(std::filesystem::path{EDGEPULSE_AGENT_TEST_HOOK_PATH}, {"--sleep", "5000"}, std::chrono::milliseconds{100});
        const auto             elapsed = std::chrono::steady_clock::now() - started;
        EXPECT_TRUE(result.timedOut);
        EXPECT_FALSE(result.succeeded());
        EXPECT_LT(elapsed, std::chrono::seconds{3});
    }

    TEST(AgentExecutors, ProcessRunnerCapsOutputAt64KiB)
    {
        const ProcessRunResult result = runProcess(std::filesystem::path{EDGEPULSE_AGENT_TEST_HOOK_PATH}, {"--output", "200000"}, std::chrono::seconds{20});
        EXPECT_TRUE(result.succeeded()) << result.failure;
        EXPECT_TRUE(result.outputTruncated);
        EXPECT_EQ(64U * 1024U, result.output.size());
    }

    TEST(AgentExecutors, ProcessRunnerRunsCmdScriptWithArgumentAndExitCode)
    {
#if !defined(_WIN32)
        GTEST_SKIP() << "cmd.exe 仅在 Windows 上可用";
#else
        TestSupport::TemporaryDirectory directory;
        const auto                      marker = directory.path() / "cmd-marker.txt";
        const ScopedEnvironmentVariable markerEnv("EDGEPULSE_AGENT_TEST_SCRIPT_MARKER", marker.string());
        const auto                      script = directory.write("echo-argument.cmd",
                                                                  "@echo off\r\n"
                                                                  "> \"%EDGEPULSE_AGENT_TEST_SCRIPT_MARKER%\" echo %~1\r\n"
                                                                  "exit /b 7\r\n");

        const ProcessRunResult result = runProcess(script, {"script-argument"}, std::chrono::seconds{20});
        EXPECT_TRUE(result.started) << result.failure;
        EXPECT_FALSE(result.succeeded());
        EXPECT_FALSE(result.timedOut);
        EXPECT_EQ(7, result.exitCode);
        EXPECT_EQ("script-argument", trimmedTrailingNewlines(readTextFile(marker)));
#endif
    }

    TEST(AgentExecutors, ProcessRunnerRunsPowerShellScript)
    {
#if !defined(_WIN32)
        GTEST_SKIP() << "powershell.exe 仅在 Windows 上可用";
#else
        TestSupport::TemporaryDirectory directory;
        const auto marker = directory.path() / "ps-marker.txt";
        const auto script = directory.write("write-marker.ps1",
                                            "[System.IO.File]::WriteAllText($args[0], 'powershell-ok')\r\n"
                                            "exit 0\r\n");

        const ProcessRunResult result = runProcess(script, {marker.string()}, std::chrono::seconds{30});
        if (!result.started)
        {
            GTEST_SKIP() << "powershell.exe 不可用：" << result.failure;
        }
        EXPECT_TRUE(result.succeeded()) << result.failure;
        EXPECT_EQ(0, result.exitCode);
        EXPECT_EQ("powershell-ok", readTextFile(marker));
#endif
    }

    TEST(AgentExecutors, ProcessRunnerRejectsPercentArgumentForCmdScript)
    {
#if !defined(_WIN32)
        GTEST_SKIP() << "cmd.exe 仅在 Windows 上可用";
#else
        TestSupport::TemporaryDirectory directory;
        const auto                      marker = directory.path() / "cmd-marker.txt";
        const ScopedEnvironmentVariable markerEnv("EDGEPULSE_AGENT_TEST_SCRIPT_MARKER", marker.string());
        const auto                      script = directory.write("echo-argument.cmd",
                                                                  "@echo off\r\n"
                                                                  "> \"%EDGEPULSE_AGENT_TEST_SCRIPT_MARKER%\" echo %~1\r\n"
                                                                  "exit /b 0\r\n");

        const ProcessRunResult result = runProcess(script, {"%PATH%"}, std::chrono::seconds{20});
        EXPECT_FALSE(result.started);
        EXPECT_FALSE(result.succeeded());
        EXPECT_NE(std::string::npos, result.failure.find("拒绝")) << result.failure;
        EXPECT_FALSE(std::filesystem::exists(marker)) << "参数在启动子进程之前就必须被拒绝";
#endif
    }
} // namespace EdgePulse::Agent
