#include "AgentApplication.h"

#include "AgentClient.h"
#include "AgentIdentity.h"
#include "AgentState.h"
#include "AgentVersion.h"
#include "AgentWebSocket.h"
#include "CommandDispatcher.h"
#include "Executors/ConfigExecutor.h"
#include "Executors/DiagnosticsExecutor.h"
#include "Executors/FirmwareExecutor.h"
#include "Executors/RestartExecutor.h"
#include "Executors/TelemetryPublisher.h"
#include "Executors/UpdateExecutor.h"
#include "ProcessRunner.h"
#include "TelemetryCollector.h"

#include "Platform/FileSystem/AtomicFileWriter.h"
#include "Platform/FileSystem/FileSystem.h"
#include "Platform/Platform.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogLevel.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/ConsoleSink.h"
#include "Base/Log/Sinks/RollingFileSink.h"
#include "Base/Runtime/Environment.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Process/GracefulShutdown.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <ctime>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !ASYN_PLATFORM_WIN32
#include <unistd.h>
#endif

namespace EdgePulse::Agent
{
    namespace
    {
        using RegistrationOutcome = std::expected<std::int64_t, RegistrationFailure>;

        constexpr int kMaximumConflictResponses = 3; ///< 单次启动允许的 409 响应次数，达到后按永久名字冲突停止

        constexpr int kMinimumHeartbeatSeconds      = 5;   ///< 心跳间隔下限
        constexpr int kMaximumHeartbeatSeconds      = 600; ///< 心跳间隔上限
        constexpr int kHeartbeatFailureWarningCount = 3;   ///< 连续失败达到该次数时额外告警一次
        constexpr int kWebSocketFailureAlertCount   = 3;   ///< WebSocket 连续失败达到该次数时额外告警一次

        /**
         * @brief 取当前 UTC 时刻的秒级文本
         * @return std::string 形如 2026-10-24T08:00:00Z
         */
        [[nodiscard]] std::string utcNowText()
        {
            const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
            return std::format("{:%Y-%m-%dT%H:%M:%S}Z", now);
        }

        /**
         * @brief 解析形如 2026-10-24T08:00:00Z 的 UTC 文本
         * @param text 待解析文本
         * @return 成功返回对应时刻；格式不符或字段越界时为空
         */
        [[nodiscard]] std::optional<std::chrono::system_clock::time_point> parseUtcTimestamp(const std::string &text)
        {
            // 固定形如 2026-10-24T08:00:00Z；长度与分隔符先定死，再按位解析数字
            constexpr std::size_t kTimestampLength = 20;
            if (text.size() != kTimestampLength || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':' || text[19] != 'Z')
            {
                return std::nullopt;
            }
            const auto readInteger = [&text](const std::size_t offset, const std::size_t length, int &value) -> bool
            {
                const auto [end, error] = std::from_chars(text.data() + offset, text.data() + offset + length, value);
                return error == std::errc{} && end == text.data() + offset + length;
            };
            int year   = 0;
            int month  = 0;
            int day    = 0;
            int hour   = 0;
            int minute = 0;
            int second = 0;
            if (!readInteger(0, 4, year) || !readInteger(5, 2, month) || !readInteger(8, 2, day) || !readInteger(11, 2, hour) || !readInteger(14, 2, minute) ||
                !readInteger(17, 2, second))
            {
                return std::nullopt;
            }
            if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60)
            {
                return std::nullopt;
            }
            tm value{};
            value.tm_year = year - 1900;
            value.tm_mon  = month - 1;
            value.tm_mday = day;
            value.tm_hour = hour;
            value.tm_min  = minute;
            value.tm_sec  = second;
#ifdef _WIN32
            const std::time_t seconds = _mkgmtime(&value);
#else
            const std::time_t seconds = timegm(&value);
#endif
            if (seconds == static_cast<std::time_t>(-1))
            {
                return std::nullopt;
            }
            return std::chrono::system_clock::from_time_t(seconds);
        }

        /**
         * @brief 常驻运行期间在循环线程上共享的状态
         */
        struct RuntimeFlags
        {
            WebSocketFallbackState fallback;                     ///< WebSocket 与轮询的降级状态
            int                    heartbeatIntervalSeconds{30}; ///< 当前心跳间隔，服务端可下发新值
        };

        /**
         * @brief 确保目录存在
         * @param directory 目标目录
         * @param error 失败时写入中文原因
         * @return bool 创建成功或已存在返回 true
         */
        [[nodiscard]] bool ensureDirectory(const std::filesystem::path &directory, std::string &error)
        {
            std::error_code filesystemError;
            std::filesystem::create_directories(directory, filesystemError);
            if (filesystemError)
            {
                error = "无法创建目录 " + directory.string() + "：" + filesystemError.message();
                return false;
            }
            return true;
        }

        /**
         * @brief 装配根日志器：控制台加按大小滚动的文件输出
         * @param configuration 已校验的配置
         * @throws std::exception 文件 Sink 打不开日志路径时由构造抛出，交给 run() 的边界捕获并拒绝启动
         * @details Agent 配置里的日志段是部署形态（文件、等级、大小与备份份数），与引擎日志加载器
         *          要求的树形段落不同，因此直接复用引擎的 Sink 与等级工具完成装配。
         */
        void installLogging(const AgentConfiguration &configuration)
        {
            auto &rootLogger = AsynGyanis::Base::LoggerRegistry::instance().getRootLogger();
            rootLogger.setLevel(AsynGyanis::Base::logLevelFromString(configuration.logLevel));
            rootLogger.addSink(std::make_shared<AsynGyanis::Base::ConsoleSink>(false));

            std::filesystem::path logDirectory = configuration.logFile.parent_path();
            if (logDirectory.empty())
            {
                logDirectory = std::filesystem::current_path();
            }
            const auto maximumSizeBytes = static_cast<std::size_t>(configuration.logMaxSizeMb) * 1024U * 1024U;
            rootLogger.addSink(std::make_shared<AsynGyanis::Base::RollingFileSink>(configuration.logFile.filename(), logDirectory, AsynGyanis::Base::RollingPolicy::Size,
                                                                                   maximumSizeBytes, static_cast<std::size_t>(configuration.logMaxBackup), true));
        }

        /**
         * @brief 按环境变量名读取令牌
         * @param variableName 环境变量名
         * @return std::string 令牌文本；变量名为空、未设置或为空时返回空串
         * @note 返回值只用于本次注册请求，不写日志、不写状态库
         */
        [[nodiscard]] std::string tokenFromEnvironment(const std::string &variableName)
        {
            if (variableName.empty())
            {
                return {};
            }
            return Common::environmentValue(variableName).value_or("");
        }

        /**
         * @brief 在事件循环上执行一次注册并把结果交回等待方
         * @param client 注册客户端
         * @param token 注册或换绑令牌
         * @param agentVersion 当前版本
         * @param promise 结果通道
         * @return 协程任务
         */
        AsynGyanis::Core::Task<> awaitRegistration(AgentClient &client, std::string token, std::string agentVersion, std::shared_ptr<std::promise<RegistrationOutcome>> promise)
        {
            try
            {
                promise->set_value(co_await client.registerAgent(std::move(token), std::move(agentVersion)));
            } catch (const std::exception &failure)
            {
                promise->set_value(std::unexpected(RegistrationFailure{"注册协程异常：" + std::string{failure.what()}, false, false}));
            } catch (...)
            {
                promise->set_value(std::unexpected(RegistrationFailure{"注册协程异常：未知错误", false, false}));
            }
            co_return;
        }

        /**
         * @brief 按 5 秒、10 秒、30 秒、60 秒的退避反复注册，直到成功、被拒绝、409 次数用尽或收到停止请求
         * @param loop 事件循环
         * @param client 注册客户端
         * @param registrationToken 注册或换绑令牌
         * @param agentVersion 当前版本
         * @param requestTimeoutSeconds 单次请求时限
         * @param stopRequested 停止标记；等待请求与退避期间每秒检查一次
         * @return 成功返回节点 ID；403、其他不可重试的失败、409 达到上限或收到停止请求时返回空
         * @details 服务端用 409 同时表示永久名字冲突与并发注册冲突，客户端不解析原因文本，
         *          只在单次启动内累计到固定次数后按永久冲突终止。退避按一秒切片，
         *          SIGTERM 落在注册阶段时最多一秒就能退出，不会把服务停机拖过 SCM 的等待上限。
         */
        [[nodiscard]] std::optional<std::int64_t> registerWithBackoff(AsynGyanis::Core::EventLoop &loop, AgentClient &client, const std::string &registrationToken,
                                                                      const std::string &agentVersion, const int requestTimeoutSeconds, const std::atomic<bool> &stopRequested)
        {
            std::jthread                          loopThread([&loop] { loop.run(); });
            std::vector<AsynGyanis::Core::Task<>> pendingTasks;
            const std::array<int, 4>              delays{5, 10, 30, 60};
            const auto                            waitLimit         = std::chrono::seconds{requestTimeoutSeconds + 15};
            int                                   conflictResponses = 0;

            const auto stopLoop = [&loop, &loopThread]()
            {
                loop.stop();
                if (loopThread.joinable())
                {
                    loopThread.join();
                }
            };
            const auto stopRequestedNow = [&stopRequested]() { return stopRequested.load(std::memory_order_acquire); };

            for (std::size_t attempt = 0;; ++attempt)
            {
                if (stopRequestedNow())
                {
                    LOG_INFO("注册阶段收到停止请求，已放弃注册");
                    stopLoop();
                    return std::nullopt;
                }

                auto promise = std::make_shared<std::promise<RegistrationOutcome>>();
                auto future  = promise->get_future();
                auto task    = awaitRegistration(client, registrationToken, agentVersion, promise);
                loop.scheduler().scheduleRemote(task.handle());
                pendingTasks.push_back(std::move(task));

                const auto deadline = std::chrono::steady_clock::now() + waitLimit;
                for (;;)
                {
                    if (future.wait_for(std::chrono::milliseconds{200}) == std::future_status::ready)
                    {
                        break;
                    }
                    if (stopRequestedNow())
                    {
                        LOG_INFO("注册请求等待期间收到停止请求，已放弃注册");
                        stopLoop();
                        return std::nullopt;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        LOG_ERROR_FMT("注册请求在 {} 秒内没有返回，停止启动", waitLimit.count());
                        stopLoop();
                        return std::nullopt;
                    }
                }

                const RegistrationOutcome outcome = future.get();
                if (outcome.has_value())
                {
                    stopLoop();
                    return *outcome;
                }
                const RegistrationFailure &failure = outcome.error();
                if (failure.authorizationRejected)
                {
                    LOG_WARN_FMT("{}；请修正令牌或证书指纹后重启", failure.message);
                    stopLoop();
                    return std::nullopt;
                }
                if (!failure.retryable)
                {
                    LOG_ERROR_FMT("{}", failure.message);
                    stopLoop();
                    return std::nullopt;
                }
                if (failure.httpStatus == 409)
                {
                    ++conflictResponses;
                    if (conflictResponses >= kMaximumConflictResponses)
                    {
                        LOG_ERROR_FMT("注册已累计 {} 次返回 HTTP 409，节点名可能已被其他租户或已删除节点占用；请更换 agent.server.name，或联系管理员清理同名节点后重启",
                                      kMaximumConflictResponses);
                        stopLoop();
                        return std::nullopt;
                    }
                }
                const int delaySeconds = delays[std::min<std::size_t>(attempt, delays.size() - 1)];
                LOG_WARN_FMT("注册失败：{}，{} 秒后重试", failure.message, delaySeconds);
                for (int elapsed = 0; elapsed < delaySeconds; ++elapsed)
                {
                    if (stopRequestedNow())
                    {
                        LOG_INFO("注册退避期间收到停止请求，已放弃注册");
                        stopLoop();
                        return std::nullopt;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds{1});
                }
            }
        }

        /**
         * @brief 按服务端节奏发送心跳，并顺带补报命令结果
         * @param loop 事件循环
         * @param client 服务端客户端
         * @param state 本地状态库
         * @param dispatcher 命令分发器
         * @param nodeId 节点标识
         * @param agentVersion 当前版本
         * @param flags 循环线程共享状态
         * @param stopRequested 停止标记
         * @param firstHealthyHeartbeat 首次成功心跳后的确认动作，可为空
         * @return 协程任务
         * @note 心跳失败只记日志并继续下一轮，任何心跳失败都不会结束 Agent；
         *       每次成功心跳后先补拉一次命令再上报结果，确认动作只在第一次成功心跳后执行一次
         */
        AsynGyanis::Core::Task<> runHeartbeat(AsynGyanis::Core::EventLoop &loop, AgentClient &client, AgentState &state, CommandDispatcher &dispatcher, const std::int64_t nodeId,
                                              std::string agentVersion, RuntimeFlags &flags, const std::atomic<bool> &stopRequested,
                                              std::function<AsynGyanis::Core::Task<>()> firstHealthyHeartbeat)
        {
            int  consecutiveFailures = 0;
            bool healthCheckDone     = false;
            for (;;)
            {
                const auto reply = co_await client.heartbeat(nodeId, agentVersion);
                if (!reply.has_value())
                {
                    ++consecutiveFailures;
                    LOG_WARN_FMT("心跳失败（连续 {} 次）：{}", consecutiveFailures, reply.error());
                    if (consecutiveFailures == kHeartbeatFailureWarningCount)
                    {
                        LOG_WARN_FMT("心跳已连续失败 {} 次：WebSocket 与命令轮询继续运行，Agent 不会退出", consecutiveFailures);
                    }
                } else
                {
                    consecutiveFailures = 0;
                    if (reply->heartbeatIntervalSeconds >= kMinimumHeartbeatSeconds && reply->heartbeatIntervalSeconds <= kMaximumHeartbeatSeconds)
                    {
                        flags.heartbeatIntervalSeconds = reply->heartbeatIntervalSeconds;
                    } else
                    {
                        LOG_WARN_FMT("服务端下发的心跳间隔 {} 秒不在 {}..{} 内，继续使用 {} 秒", reply->heartbeatIntervalSeconds, kMinimumHeartbeatSeconds,
                                     kMaximumHeartbeatSeconds, flags.heartbeatIntervalSeconds);
                    }
                    if (reply->desiredConfigVersion >= 0)
                    {
                        // 值未变化时跳过写入，避免每次心跳都触发一次 SQLite 写事务
                        const std::string desired = std::to_string(reply->desiredConfigVersion);
                        const auto        current = state.get("desired_config_version");
                        if (!current.has_value() || *current != desired)
                        {
                            state.set("desired_config_version", desired);
                        }
                    } else
                    {
                        LOG_WARN("心跳响应没有可解析的 desiredConfigVersion，保留状态库里的旧值");
                    }
                    if (!healthCheckDone)
                    {
                        healthCheckDone = true;
                        if (firstHealthyHeartbeat)
                        {
                            co_await firstHealthyHeartbeat();
                        }
                    }
                    // hello 与通道登记之间存在空窗：即使 WebSocket 已连接，也每轮心跳补拉一次，
                    // 让空窗里创建的命令最迟在一个心跳周期内被执行；重复投递由分发器去重
                    co_await dispatcher.pollOnce();
                    const ResultFlush flushed = co_await dispatcher.flushResults();
                    if (flushed.failed)
                    {
                        LOG_WARN("命令结果补报未完成，将在下一轮心跳后重试");
                    }
                }

                if (stopRequested.load(std::memory_order_acquire))
                {
                    co_return;
                }
                AsynGyanis::Core::Timer timer(loop);
                co_await timer.waitFor(std::chrono::seconds{flags.heartbeatIntervalSeconds});
            }
        }

        /**
         * @brief 维持 WebSocket 命令通道：断线按 1、2、5、15、30、60 秒退避重连
         * @param loop 事件循环
         * @param socket 命令长连接
         * @param dispatcher 命令分发器
         * @param nodeId 节点标识
         * @param agentVersion 当前版本
         * @param flags 循环线程共享状态
         * @param stopRequested 停止标记
         * @return 协程任务
         * @note 轮询在启动与任何断开期间运行，只有 hello 确认的连接会暂停它；
         *       连续失败达到 3 次时额外告警一次，重连退避仍按失败次数增长
         */
        AsynGyanis::Core::Task<> runWebSocket(AsynGyanis::Core::EventLoop &loop, AgentWebSocket &socket, CommandDispatcher &dispatcher, const std::int64_t nodeId,
                                              std::string agentVersion, RuntimeFlags &flags, const std::atomic<bool> &stopRequested)
        {
            const AgentWebSocket::ConnectionObserver observer = [&flags](const bool connected)
            {
                if (connected)
                {
                    flags.fallback.recordConnected();
                    LOG_INFO("WebSocket 命令通道已建立，命令轮询暂停");
                } else
                {
                    flags.fallback.recordDisconnected();
                    LOG_INFO("WebSocket 命令通道已断开，命令轮询恢复");
                }
            };

            for (;;)
            {
                if (stopRequested.load(std::memory_order_acquire))
                {
                    co_return;
                }

                const auto outcome = co_await socket.serve(dispatcher, nodeId, agentVersion, observer);
                if (outcome.has_value())
                {
                    LOG_INFO("WebSocket 命令通道已关闭，将重新连接");
                    flags.fallback.recordDisconnected();
                } else
                {
                    flags.fallback.recordConnectFailure();
                    LOG_WARN_FMT("WebSocket 命令通道不可用（连续 {} 次）：{}", flags.fallback.consecutiveFailures(), outcome.error());
                    if (flags.fallback.consecutiveFailures() == kWebSocketFailureAlertCount)
                    {
                        LOG_WARN("WebSocket 已连续失败 3 次，命令轮询继续兜底");
                    }
                }

                if (stopRequested.load(std::memory_order_acquire))
                {
                    co_return;
                }
                AsynGyanis::Core::Timer timer(loop);
                co_await timer.waitFor(std::chrono::seconds{webSocketReconnectBackoffSeconds(flags.fallback.consecutiveFailures())});
            }
        }

        /**
         * @brief 按固定间隔轮询命令，WebSocket 可用期间暂停
         * @param loop 事件循环
         * @param dispatcher 命令分发器
         * @param intervalSeconds 轮询间隔
         * @param flags 循环线程共享状态
         * @param stopRequested 停止标记
         * @return 协程任务
         */
        AsynGyanis::Core::Task<> runPolling(AsynGyanis::Core::EventLoop &loop, CommandDispatcher &dispatcher, const int intervalSeconds, RuntimeFlags &flags,
                                            const std::atomic<bool> &stopRequested)
        {
            for (;;)
            {
                if (stopRequested.load(std::memory_order_acquire))
                {
                    co_return;
                }
                if (flags.fallback.shouldPoll())
                {
                    co_await dispatcher.pollOnce();
                    const ResultFlush flushed = co_await dispatcher.flushResults();
                    if (flushed.failed)
                    {
                        LOG_WARN("命令结果上报未完成，将在下一轮轮询后重试");
                    }
                }
                AsynGyanis::Core::Timer timer(loop);
                co_await timer.waitFor(std::chrono::seconds{intervalSeconds > 0 ? intervalSeconds : 15});
            }
        }

        /**
         * @brief 取当前进程标识，用于拼出唯一的 systemd 重启单元名
         * @return std::uint64_t 进程标识
         */
        [[nodiscard]] std::uint64_t currentProcessIdentifier()
        {
#if ASYN_PLATFORM_WIN32
            return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
            return static_cast<std::uint64_t>(::getpid());
#endif
        }

        /**
         * @brief 通过系统服务管理器重启 Agent，成功启动后请求当前进程停止
         * @param loop 事件循环
         * @param requestStop 请求当前进程停止的回调
         * @return 协程任务
         * @details 启动器在阻塞执行器上脱离运行，避免 systemd-run 或 powershell 的启动耗时占住循环。
         *          Linux 用 transient 单元延迟执行 systemctl restart，Windows 用 Restart-Service；
         *          启动失败只记 ERROR 并保持进程运行，因为命令结果已经入队，不能假装重启成功。
         *          Windows 安装器固定把服务注册为 EdgePulseAgent，自定义服务名不支持本命令。
         */
        AsynGyanis::Core::Task<> restartAgentService(AsynGyanis::Core::EventLoop &loop, const std::function<void()> &requestStop)
        {
            std::expected<void, std::string> launched;
            try
            {
                launched = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::expected<void, std::string>>(
                        loop,
                        []() -> std::expected<void, std::string>
                        {
#if ASYN_PLATFORM_WIN32
                            return launchDetached("powershell.exe", {"-NoProfile", "-Command", "Restart-Service -Name 'EdgePulseAgent' -Force"});
#else
                            const std::string unit = "--unit=edgepulse-agent-restart-" + std::to_string(currentProcessIdentifier());
                            return launchDetached("systemd-run", {"--collect", unit, "--property=Type=exec", "systemctl", "restart", "edgepulse-agent.service"});
#endif
                        });
            } catch (const std::exception &failure)
            {
                launched = std::unexpected("提交服务重启任务失败：" + std::string{failure.what()});
            } catch (...)
            {
                launched = std::unexpected("提交服务重启任务失败：未知异常");
            }
            if (!launched.has_value())
            {
                LOG_ERROR_FMT("agent.restart 启动服务重启失败：{}", launched.error());
                co_return;
            }

            LOG_INFO("agent.restart 已启动服务重启，当前进程即将退出");
            if (requestStop)
            {
                try
                {
                    requestStop();
                } catch (const std::exception &failure)
                {
                    LOG_WARN_FMT("agent.restart 请求当前进程停止失败：{}", failure.what());
                } catch (...)
                {
                    LOG_WARN("agent.restart 请求当前进程停止失败：未知异常");
                }
            }
            co_return;
        }

        /**
         * @brief 按间隔补报离线遥测并采集新一轮样本
         * @param loop 事件循环
         * @param collector 采集器，采集动作投到阻塞执行器上执行
         * @param publisher 遥测上报器
         * @param nodeId 节点标识
         * @param intervalSeconds 采集间隔，单位秒
         * @param stopRequested 停止标记
         * @return 协程任务
         * @note 每轮先补报既有队列，再等待间隔后采集并直报；任何一次失败都不结束协程，也不阻塞心跳与命令
         */
        AsynGyanis::Core::Task<> runTelemetry(AsynGyanis::Core::EventLoop &loop, std::shared_ptr<TelemetryCollector> collector, TelemetryPublisher &publisher,
                                              const std::int64_t nodeId, const int intervalSeconds, const std::atomic<bool> &stopRequested)
        {
            for (;;)
            {
                if (stopRequested.load(std::memory_order_acquire))
                {
                    co_return;
                }
                co_await publisher.flush(loop, nodeId);
                if (stopRequested.load(std::memory_order_acquire))
                {
                    co_return;
                }

                AsynGyanis::Core::Timer timer(loop);
                co_await timer.waitFor(std::chrono::seconds{intervalSeconds > 0 ? intervalSeconds : 30});
                if (stopRequested.load(std::memory_order_acquire))
                {
                    co_return;
                }

                std::vector<TelemetrySample> samples;
                try
                {
                    samples = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::vector<TelemetrySample>>(loop, [collector] { return collector->collect(); });
                } catch (const std::exception &failure)
                {
                    LOG_WARN_FMT("遥测采集任务提交失败，本轮跳过：{}", failure.what());
                    continue;
                } catch (...)
                {
                    LOG_WARN("遥测采集任务提交失败：未知异常，本轮跳过");
                    continue;
                }
                if (!samples.empty())
                {
                    co_await publisher.publish(nodeId, samples);
                }
            }
        }

        /**
         * @brief 在阻塞执行器上检查升级标记是否存在
         * @param loop 事件循环
         * @param path 标记路径
         * @return 协程任务，结果为 true 表示标记存在；失败返回中文原因
         * @note 状态目录可能位于慢盘，存在检查不能占住事件循环
         */
        AsynGyanis::Core::Task<std::expected<bool, std::string>> markerExists(AsynGyanis::Core::EventLoop &loop, std::filesystem::path path)
        {
            std::expected<bool, std::string> found;
            try
            {
                found = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::expected<bool, std::string>>(
                        loop,
                        [path]() -> std::expected<bool, std::string>
                        {
                            std::string     pathText = AsynGyanis::Platform::FileSystem::utf8FromPath(path);
                            std::error_code filesystemError;
                            const bool      exists = std::filesystem::exists(path, filesystemError);
                            if (filesystemError)
                            {
                                return std::unexpected("检查升级标记 " + pathText + " 失败：" + filesystemError.message());
                            }
                            return exists;
                        });
            } catch (const std::exception &failure)
            {
                found = std::unexpected("提交升级标记检查任务失败：" + std::string{failure.what()});
            } catch (...)
            {
                found = std::unexpected("提交升级标记检查任务失败：未知异常");
            }
            co_return found;
        }

        /**
         * @brief 在阻塞执行器上删除升级标记，文件不存在视为成功
         * @param loop 事件循环
         * @param path 标记路径
         * @return 协程任务；删除失败时返回中文原因
         */
        AsynGyanis::Core::Task<std::expected<void, std::string>> removeMarker(AsynGyanis::Core::EventLoop &loop, std::filesystem::path path)
        {
            std::expected<void, std::string> removed;
            try
            {
                removed = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::expected<void, std::string>>(
                        loop,
                        [path]() -> std::expected<void, std::string>
                        {
                            std::error_code filesystemError;
                            std::filesystem::remove(path, filesystemError);
                            if (filesystemError)
                            {
                                return std::unexpected("删除升级标记 " + AsynGyanis::Platform::FileSystem::utf8FromPath(path) + " 失败：" + filesystemError.message());
                            }
                            return {};
                        });
            } catch (const std::exception &failure)
            {
                removed = std::unexpected("提交升级标记删除任务失败：" + std::string{failure.what()});
            } catch (...)
            {
                removed = std::unexpected("提交升级标记删除任务失败：未知异常");
            }
            co_return removed;
        }

        /**
         * @brief 在阻塞执行器上读取升级标记文本
         * @param loop 事件循环
         * @param path 标记路径
         * @return 协程任务；标记不存在时返回空 optional，读取失败返回中文原因
         * @note 内容去掉首尾空白；只用于版本这类单行文本
         */
        AsynGyanis::Core::Task<std::expected<std::optional<std::string>, std::string>> readMarker(AsynGyanis::Core::EventLoop &loop, std::filesystem::path path)
        {
            std::expected<std::optional<std::string>, std::string> read;
            try
            {
                read = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::expected<std::optional<std::string>, std::string>>(
                        loop,
                        [path]() -> std::expected<std::optional<std::string>, std::string>
                        {
                            const std::string pathText = AsynGyanis::Platform::FileSystem::utf8FromPath(path);
                            std::error_code   filesystemError;
                            if (!std::filesystem::exists(path, filesystemError))
                            {
                                if (filesystemError)
                                {
                                    return std::unexpected("检查升级标记 " + pathText + " 失败：" + filesystemError.message());
                                }
                                return std::optional<std::string>{};
                            }
                            std::ifstream stream(path, std::ios::binary);
                            if (!stream)
                            {
                                return std::unexpected("读取升级标记 " + pathText + " 失败");
                            }
                            std::string content{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
                            if (stream.bad())
                            {
                                return std::unexpected("读取升级标记 " + pathText + " 失败");
                            }
                            const auto isSpace = [](const char character) { return character == ' ' || character == '\t' || character == '\r' || character == '\n'; };
                            while (!content.empty() && isSpace(content.back()))
                            {
                                content.pop_back();
                            }
                            std::size_t begin = 0;
                            while (begin < content.size() && isSpace(content[begin]))
                            {
                                ++begin;
                            }
                            return std::optional<std::string>{content.substr(begin)};
                        });
            } catch (const std::exception &failure)
            {
                read = std::unexpected("提交升级标记读取任务失败：" + std::string{failure.what()});
            } catch (...)
            {
                read = std::unexpected("提交升级标记读取任务失败：未知异常");
            }
            co_return read;
        }

        /**
         * @brief 在阻塞执行器上写入升级健康标记
         * @param loop 事件循环
         * @param path 健康标记路径
         * @param agentVersion 当前运行版本
         * @return 协程任务；失败返回中文原因
         */
        AsynGyanis::Core::Task<std::expected<void, std::string>> writeHealthMarker(AsynGyanis::Core::EventLoop &loop, std::filesystem::path path, std::string agentVersion)
        {
            std::expected<void, std::string> written;
            try
            {
                written = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::expected<void, std::string>>(
                        loop,
                        [path, agentVersion = std::move(agentVersion)]() -> std::expected<void, std::string>
                        {
                            std::string writeError;
                            if (!AsynGyanis::Platform::AtomicFileWriter::writeText(path, agentVersion, std::nullopt, &writeError))
                            {
                                return std::unexpected("写入升级健康标记 " + AsynGyanis::Platform::FileSystem::utf8FromPath(path) + " 失败：" + writeError);
                            }
                            return {};
                        });
            } catch (const std::exception &failure)
            {
                written = std::unexpected("提交升级健康标记写入任务失败：" + std::string{failure.what()});
            } catch (...)
            {
                written = std::unexpected("提交升级健康标记写入任务失败：未知异常");
            }
            co_return written;
        }
    } // namespace

    AgentApplication::AgentApplication(AgentConfiguration configuration, const bool consoleMode) : m_configuration(std::move(configuration)), m_consoleMode(consoleMode)
    {
    }

    void AgentApplication::requestStop()
    {
        m_stopRequested.store(true, std::memory_order_release);
        const std::lock_guard lock(m_loopMutex);
        if (m_loop != nullptr)
        {
            m_loop->stop();
        }
    }

    AsynGyanis::Core::Task<> AgentApplication::confirmUpdateHealth(AsynGyanis::Core::EventLoop &loop, AgentState &state, std::string agentVersion)
    {
        const auto update = state.updateState();
        if (!update.has_value())
        {
            LOG_ERROR_FMT("读取升级状态失败，本次跳过升级健康确认：{}", update.error());
            co_return;
        }

        const std::filesystem::path healthMarker   = m_configuration.stateDirectory / "update-health";
        const std::filesystem::path rollbackMarker = m_configuration.stateDirectory / "update-rollback";
        const std::filesystem::path expectedMarker = m_configuration.stateDirectory / "update-expected";

        // 期望版本标记由安装器在启动新版本之前写入，手工 --upgrade 没有待上报结果时也靠它确认健康
        std::optional<std::string> expectedVersion;
        const auto                 expectedRead = co_await readMarker(loop, expectedMarker);
        if (expectedRead.has_value())
        {
            expectedVersion = *expectedRead;
        } else
        {
            // 读不出标记不能连待上报的升级结果一起丢掉，否则每次启动都会重复处理同一行
            LOG_ERROR_FMT("{}，本次按没有期望版本标记处理", expectedRead.error());
        }

        if (update->pendingResult.empty())
        {
            // 没有待上报结果时回滚标记只可能是上一次升级清理失败的残留，已无法补报，删掉免得下次启动重复判断
            const auto removed = co_await removeMarker(loop, rollbackMarker);
            if (!removed.has_value())
            {
                LOG_WARN_FMT("清理残留的升级回滚标记失败：{}", removed.error());
            }

            if (!expectedVersion.has_value())
            {
                co_return;
            }
            if (*expectedVersion != agentVersion)
            {
                // 标记留给安装器：版本不符说明新二进制没有真正跑起来，安装器会据此回滚
                LOG_WARN_FMT("升级期望版本 {} 与当前运行版本 {} 不一致，保留 update-expected 等待安装器回滚", *expectedVersion, agentVersion);
                co_return;
            }

            const auto written = co_await writeHealthMarker(loop, healthMarker, agentVersion);
            if (!written.has_value())
            {
                LOG_ERROR_FMT("{}", written.error());
                co_return;
            }
            const auto deleted = co_await removeMarker(loop, expectedMarker);
            if (!deleted.has_value())
            {
                LOG_WARN_FMT("{}", deleted.error());
            }
            LOG_INFO_FMT("手工升级健康标记已写入，版本 {}", agentVersion);
            co_return;
        }

        const auto rollbackFound = co_await markerExists(loop, rollbackMarker);
        if (!rollbackFound.has_value())
        {
            LOG_ERROR_FMT("{}，保留待上报结果等待下次启动", rollbackFound.error());
            co_return;
        }
        const bool rollbackPending = *rollbackFound;

        const auto   parsed    = AsynGyanis::Base::parseConfigValue(update->pendingResult);
        std::int64_t commandId = 0;
        if (parsed.has_value() && parsed->is_object())
        {
            const auto iterator = parsed->find("commandId");
            if (iterator != parsed->end())
            {
                const auto value = AsynGyanis::Base::configValueAs<std::int64_t>(*iterator);
                if (value.has_value())
                {
                    commandId = *value;
                }
            }
        }
        if (commandId <= 0)
        {
            LOG_ERROR_FMT("升级待上报结果缺少可用的 commandId，已清除：{}", update->pendingResult);
            UpdateStateRecord clearedRecord = *update;
            clearedRecord.healthyAt.clear();
            clearedRecord.pendingResult.clear();
            const auto cleared = state.setUpdateState(clearedRecord);
            if (!cleared.has_value())
            {
                LOG_ERROR_FMT("清除损坏的升级待上报结果失败：{}", cleared.error());
                co_return;
            }
            if (rollbackPending)
            {
                const auto removed = co_await removeMarker(loop, rollbackMarker);
                if (!removed.has_value())
                {
                    LOG_ERROR_FMT("{}", removed.error());
                }
            }
            co_return;
        }

        // 目标版本与当前运行版本不一致说明二进制没有被替换；写健康标记会把旧版本报成升级成功
        const bool versionMismatch = !update->version.empty() && update->version != agentVersion;
        if (rollbackPending || versionMismatch)
        {
            std::string message;
            if (rollbackPending)
            {
                message = "升级未通过健康检查，已回滚到旧版本";
            } else
            {
                message = "升级目标版本 " + update->version + " 与当前运行版本 " + agentVersion + " 不一致";
            }
            const AsynGyanis::Base::ConfigValue payload = AsynGyanis::Base::ConfigValue::object({{"status", "failed"}, {"result", "{}"}, {"message", message}});
            // 安装器启动时已为同一命令入队过一条成功结果，直接入队会被唯一索引丢弃，必须整行替换
            const auto replaced = state.replaceResult(commandId, payload.dump());
            if (!replaced.has_value())
            {
                LOG_ERROR_FMT("升级失败结果替换入队失败，保留待上报结果等待下次启动：{}", replaced.error());
                co_return;
            }

            UpdateStateRecord record = *update;
            record.healthyAt.clear();
            record.pendingResult.clear();
            const auto cleared = state.setUpdateState(record);
            if (!cleared.has_value())
            {
                LOG_ERROR_FMT("清除升级待上报结果失败，保留回滚标记等待下次启动重新上报：{}", cleared.error());
                co_return;
            }

            if (rollbackPending)
            {
                const auto removed = co_await removeMarker(loop, rollbackMarker);
                if (!removed.has_value())
                {
                    LOG_ERROR_FMT("{}", removed.error());
                }
                LOG_WARN_FMT("升级未通过健康检查，命令 {} 已按失败上报并恢复旧版本状态", commandId);
            } else
            {
                LOG_WARN_FMT("升级目标版本 {} 与当前运行版本 {} 不一致，命令 {} 已按失败上报", update->version, agentVersion, commandId);
            }
            co_return;
        }

        const auto written = co_await writeHealthMarker(loop, healthMarker, agentVersion);
        if (!written.has_value())
        {
            LOG_ERROR_FMT("{}", written.error());
            co_return;
        }

        const AsynGyanis::Base::ConfigValue detail   = AsynGyanis::Base::ConfigValue::object({{"version", agentVersion}});
        const AsynGyanis::Base::ConfigValue payload  = AsynGyanis::Base::ConfigValue::object({{"status", "succeeded"}, {"result", detail.dump()}, {"message", "升级成功"}});
        const auto                          replaced = state.replaceResult(commandId, payload.dump());
        if (!replaced.has_value())
        {
            LOG_ERROR_FMT("升级成功结果替换入队失败，保留待上报结果等待下次启动：{}", replaced.error());
            co_return;
        }

        UpdateStateRecord record = *update;
        record.healthyAt         = utcNowText();
        record.pendingResult.clear();
        const auto saved = state.setUpdateState(record);
        if (!saved.has_value())
        {
            LOG_ERROR_FMT("升级健康标记已写入，但记录确认时刻或清除待上报结果失败：{}", saved.error());
        }
        if (expectedVersion.has_value())
        {
            if (*expectedVersion == agentVersion)
            {
                const auto deleted = co_await removeMarker(loop, expectedMarker);
                if (!deleted.has_value())
                {
                    LOG_WARN_FMT("{}", deleted.error());
                }
            } else
            {
                LOG_WARN_FMT("升级期望版本 {} 与当前运行版本 {} 不一致，保留 update-expected 等待安装器回滚", *expectedVersion, agentVersion);
            }
        }
        LOG_INFO_FMT("升级健康标记已写入，版本 {}，命令 {} 已按成功上报", agentVersion, commandId);
        co_return;
    }

    AsynGyanis::Core::Task<> AgentApplication::runCertificateRenewal(AsynGyanis::Core::EventLoop &loop, AgentState &state, const std::int64_t nodeId,
                                                                     const AgentConfiguration &configuration, CertificateRenewalDependencies dependencies,
                                                                     const std::atomic<bool> &stopRequested)
    {
        const auto nowProvider   = dependencies.now ? dependencies.now : []() { return std::chrono::system_clock::now(); };
        const auto checkInterval = dependencies.checkInterval.count() > 0
                                           ? dependencies.checkInterval
                                           : std::chrono::milliseconds{std::chrono::seconds{std::clamp(configuration.certificateRenewalCheckSeconds, 60, 86400)}};
        const auto renewBefore   = std::chrono::days{std::clamp(configuration.certificateRenewBeforeDays, 1, 90)};
        const std::vector<std::chrono::milliseconds>  defaultBackoff{std::chrono::seconds{5}, std::chrono::seconds{10}, std::chrono::seconds{30}, std::chrono::seconds{60},
                                                                     std::chrono::seconds{300}};
        const std::vector<std::chrono::milliseconds> &backoff = dependencies.retryBackoff.empty() ? defaultBackoff : dependencies.retryBackoff;

        std::string currentExpiresAt = dependencies.expiresAt;
        std::size_t retryIndex       = 0;
        const auto  nextFailureDelay = [&backoff, &retryIndex]()
        {
            const auto delay = backoff[std::min(retryIndex, backoff.size() - 1)];
            if (retryIndex + 1 < backoff.size())
            {
                ++retryIndex;
            }
            return delay;
        };

        // 等待按一秒切片：停止标记置位后最迟一秒退出，不把小时级的检查间隔变成停机等待
        const auto waitFor = [&loop, &dependencies, &stopRequested](const std::chrono::milliseconds duration) -> AsynGyanis::Core::Task<bool>
        {
            if (stopRequested.load(std::memory_order_acquire))
            {
                co_return false;
            }
            if (dependencies.waitObserver)
            {
                dependencies.waitObserver(duration);
            }
            auto remaining = duration;
            while (remaining > std::chrono::milliseconds::zero())
            {
                if (stopRequested.load(std::memory_order_acquire))
                {
                    co_return false;
                }
                const auto              slice = std::min(remaining, std::chrono::milliseconds{1000});
                AsynGyanis::Core::Timer timer(loop);
                co_await timer.waitFor(slice);
                remaining -= slice;
            }
            co_return !stopRequested.load(std::memory_order_acquire);
        };

        for (;;)
        {
            if (stopRequested.load(std::memory_order_acquire))
            {
                co_return;
            }

            const auto expiry = parseUtcTimestamp(currentExpiresAt);
            if (!expiry.has_value())
            {
                LOG_ERROR_FMT("证书到期时间无法解析（{}），本轮跳过自动续期；请检查证书文件", currentExpiresAt);
                if (!co_await waitFor(checkInterval))
                {
                    co_return;
                }
                continue;
            }
            if (nowProvider() < *expiry - renewBefore)
            {
                if (!co_await waitFor(checkInterval))
                {
                    co_return;
                }
                continue;
            }

            LOG_INFO_FMT("证书将在 {} 到期，开始自动续期", currentExpiresAt);
            if (!dependencies.generateRequest || !dependencies.renew || !dependencies.install)
            {
                LOG_ERROR("自动续期缺少生成、续期或安装入口，本循环停止");
                co_return;
            }

            const auto material = dependencies.generateRequest();
            if (!material)
            {
                const auto delay = nextFailureDelay();
                LOG_WARN_FMT("生成续期密钥与 CSR 失败：{}；{} 毫秒后重试", material.error(), delay.count());
                if (!co_await waitFor(delay))
                {
                    co_return;
                }
                continue;
            }

            const auto renewed = co_await dependencies.renew(nodeId, material->csrPem);
            if (!renewed)
            {
                const CertificateRenewalFailure &failure = renewed.error();
                if (failure.authorizationRejected)
                {
                    LOG_ERROR_FMT("自动续期被服务端拒绝：{}；请重新入场注册并重启 Agent，自动续期已停止", failure.message);
                    co_return;
                }
                if (!failure.retryable)
                {
                    LOG_ERROR_FMT("自动续期失败且不可重试：{}；自动续期已停止", failure.message);
                    co_return;
                }
                const auto delay = nextFailureDelay();
                LOG_WARN_FMT("自动续期失败：{}；{} 毫秒后重试", failure.message, delay.count());
                if (!co_await waitFor(delay))
                {
                    co_return;
                }
                continue;
            }

            const auto installed = dependencies.install(renewed->certificatePem);
            if (!installed)
            {
                const auto delay = nextFailureDelay();
                LOG_WARN_FMT("安装新证书失败，旧证书继续使用：{}；{} 毫秒后重试", installed.error(), delay.count());
                if (!co_await waitFor(delay))
                {
                    co_return;
                }
                continue;
            }

            // HTTP 身份没换过去说明文件已经回退，本轮按失败处理；WebSocket 的换代失败不影响
            // 已建立的连接（服务端在重叠窗口内仍接受旧证书），下一次重连自然用新证书
            if (dependencies.reloadHttpIdentity && !dependencies.reloadHttpIdentity())
            {
                const auto delay = nextFailureDelay();
                LOG_WARN_FMT("HTTP 通道重载新证书失败，已回退旧身份；{} 毫秒后重试", delay.count());
                if (!co_await waitFor(delay))
                {
                    co_return;
                }
                continue;
            }
            if (dependencies.reloadWebSocketIdentity && !dependencies.reloadWebSocketIdentity())
            {
                LOG_WARN("WebSocket 通道未能装载新证书，现有连接继续使用旧证书，下一次重连时生效");
            }

            currentExpiresAt = installed->expiresAt;
            state.set("cert_fingerprint", installed->fingerprint);
            state.set("cert_expiry", installed->expiresAt);
            retryIndex = 0;
            LOG_INFO_FMT("证书续期完成：新指纹 {}，到期 {}（UTC），HTTP 与 WebSocket 已切换到新身份", installed->fingerprint, installed->expiresAt);

            if (stopRequested.load(std::memory_order_acquire))
            {
                co_return;
            }
            if (!co_await waitFor(checkInterval))
            {
                co_return;
            }
        }
    }

    int AgentApplication::run()
    {
        try
        {
            std::string error;
            if (!ensureDirectory(m_configuration.stateDirectory, error))
            {
                std::cerr << error << '\n';
                return 1;
            }
            if (!m_configuration.logFile.parent_path().empty() && !ensureDirectory(m_configuration.logFile.parent_path(), error))
            {
                std::cerr << error << '\n';
                return 1;
            }

            installLogging(m_configuration);
            LOG_INFO_FMT("Agent 已启动，节点名 {}", m_configuration.name);
            auto telemetryCollector = std::make_shared<TelemetryCollector>(m_configuration.telemetryDiskPath);

            // 注册阶段还没有常驻循环；先接管信号，SIGTERM 只置停止标记，由每秒一次的注册等待自行收口
            std::optional<AsynGyanis::Core::GracefulShutdown> shutdown;
            if (m_consoleMode)
            {
                shutdown.emplace();
                shutdown->onShutdown([this] { requestStop(); });
            }

            const auto identity = AgentIdentity::load(m_configuration);
            if (!identity.has_value())
            {
                LOG_ERROR_FMT("加载证书身份失败：{}", identity.error());
                return 1;
            }
            LOG_INFO_FMT("证书身份已加载：节点 {}，指纹 {}，到期 {}（UTC）", m_configuration.name, identity->fingerprint, identity->expiresAt);

            auto state = AgentState::open(m_configuration.stateDirectory / "agent.db");
            if (!state.has_value())
            {
                LOG_ERROR_FMT("{}", state.error());
                return 1;
            }

            const bool        rebindTokenSelected = state->get("node_id").has_value() && !m_configuration.rebindTokenEnv.empty();
            const std::string registrationToken   = tokenFromEnvironment(rebindTokenSelected ? m_configuration.rebindTokenEnv : m_configuration.enrollmentTokenEnv);
            if (rebindTokenSelected)
            {
                LOG_INFO_FMT("状态库已有节点标识，本次注册从换绑令牌环境变量读取令牌");
            }
            const std::string agentVersion{kAgentVersion};

            std::optional<std::int64_t> nodeId;
            {
                // 注册阶段的循环在注册结束后停止，而 EventLoop 的停止请求是粘性的，常驻段必须另起一条循环
                AsynGyanis::Core::EventLoop registrationLoop;
                AgentClient                 registrationClient(registrationLoop, m_configuration, *identity);
                nodeId = registerWithBackoff(registrationLoop, registrationClient, registrationToken, agentVersion, m_configuration.requestTimeoutSeconds, m_stopRequested);
            }
            if (!nodeId.has_value())
            {
                if (m_stopRequested.load(std::memory_order_acquire))
                {
                    LOG_INFO("注册完成前收到停止请求，Agent 按服务停止退出");
                    return 0;
                }
                return 1;
            }

            state->set("node_id", std::to_string(*nodeId));
            state->set("agent_version", agentVersion);
            state->set("cert_fingerprint", identity->fingerprint);
            state->set("cert_expiry", identity->expiresAt);
            LOG_INFO_FMT("注册成功，节点 {} 的 ID 为 {}，指纹与版本已写入状态库", m_configuration.name, *nodeId);

            AsynGyanis::Core::EventLoop                   loop;
            AgentClient                                   client(loop, m_configuration, *identity);
            std::vector<std::shared_ptr<CommandExecutor>> executors;
            executors.push_back(std::make_shared<ConfigExecutor>(client, m_configuration, *nodeId));
            executors.push_back(std::make_shared<FirmwareExecutor>(client, *state, m_configuration, *nodeId));
            // 安装器在另一个进程里替换二进制并等待健康标记，本进程确认它已启动后立即退出
            executors.push_back(std::make_shared<UpdateExecutor>(client, *state, m_configuration, UpdateExecutor::Launcher{}, [this] { requestStop(); }));
            // 诊断指标由遥测采集器提供，提供者本身在诊断工作线程上执行
            executors.push_back(std::make_shared<DiagnosticsExecutor>(*state, m_configuration, agentVersion, [telemetryCollector] { return telemetryCollector->metricsJson(); }));
            // 重启动作必须在命令结果入队并尝试上报之后执行，否则结果会随进程退出丢失；
            // 平台服务管理器（systemd 或 SCM）负责把进程重新拉起来
            executors.push_back(std::make_shared<RestartExecutor>(true));
            CommandDispatcher       dispatcher(client, *state, std::move(executors),
                                               [this, &loop]() -> AsynGyanis::Core::Task<> { co_await restartAgentService(loop, [this] { requestStop(); }); });
            AgentWebSocket          webSocket(loop, m_configuration);
            TelemetryPublisher      telemetryPublisher(client, *state);
            AgentCertificateManager certificateManager(m_configuration);

            RuntimeFlags flags;
            flags.heartbeatIntervalSeconds = m_configuration.heartbeatIntervalSeconds;

            if (m_consoleMode)
            {
                // 常驻段的收尾动作要跑在循环线程上；注册阶段的观察者在这里换成绑定循环的实例
                shutdown.reset();
                shutdown.emplace(loop);
                shutdown->onShutdown([this] { requestStop(); });
            }

            // 帧的寿命与常驻循环绑定：先停循环，再在 run() 返回前销毁这些帧
            std::vector<AsynGyanis::Core::Task<>> frames;
            frames.push_back(runHeartbeat(loop, client, *state, dispatcher, *nodeId, agentVersion, flags, m_stopRequested,
                                          [this, &loop, statePointer = &*state, agentVersion] { return confirmUpdateHealth(loop, *statePointer, agentVersion); }));
            if (m_configuration.websocketEnabled)
            {
                frames.push_back(runWebSocket(loop, webSocket, dispatcher, *nodeId, agentVersion, flags, m_stopRequested));
            } else
            {
                LOG_INFO("配置已关闭 WebSocket，命令通道立即使用轮询");
            }
            frames.push_back(runPolling(loop, dispatcher, m_configuration.commandPollIntervalSeconds, flags, m_stopRequested));
            frames.push_back(runTelemetry(loop, telemetryCollector, telemetryPublisher, *nodeId, m_configuration.telemetryIntervalSeconds, m_stopRequested));
            if (m_configuration.baseUrl.starts_with("https://"))
            {
                CertificateRenewalDependencies renewal;
                renewal.expiresAt          = identity->expiresAt;
                renewal.generateRequest    = [&certificateManager]() { return certificateManager.generateKeyAndCsr(); };
                renewal.renew              = [&client](std::int64_t renewingNodeId, std::string csrPem) { return client.renewCertificate(renewingNodeId, std::move(csrPem)); };
                renewal.install            = [&certificateManager](std::string_view certificatePem) { return certificateManager.install(certificatePem); };
                renewal.reloadHttpIdentity = [&client, &certificateManager]()
                { return client.reloadIdentityFromDisk([&certificateManager]() { return certificateManager.restorePrevious().has_value(); }); };
                renewal.reloadWebSocketIdentity = [&webSocket]() { return webSocket.reloadIdentityFromDisk(); };
                frames.push_back(runCertificateRenewal(loop, *state, *nodeId, m_configuration, std::move(renewal), m_stopRequested));
            } else
            {
                LOG_INFO("明文测试配置不启用证书自动续期");
            }
            // 循环尚未开始运行，此时入本地就绪队列；帧由上面那个容器持有到 run() 结束
            for (auto &frame: frames)
            {
                loop.scheduler().schedule(frame.handle());
            }

            {
                const std::lock_guard lock(m_loopMutex);
                m_loop = &loop;
            }
            if (!m_stopRequested.load(std::memory_order_acquire))
            {
                LOG_INFO_FMT("Agent 已进入常驻运行，心跳间隔 {} 秒，轮询间隔 {} 秒", flags.heartbeatIntervalSeconds, m_configuration.commandPollIntervalSeconds);
                loop.run();
            }
            const bool stoppedByFailure = loop.snapshot().stoppedByFailure;
            {
                const std::lock_guard lock(m_loopMutex);
                m_loop = nullptr;
            }
            if (stoppedByFailure)
            {
                // 非零退出让 systemd 的 Restart= 与 SCM 的失败恢复动作真正触发，而不是把崩溃当正常停机
                LOG_ERROR("Agent 事件循环因未捕获异常停止，以退出码 1 结束");
                return 1;
            }
            LOG_INFO("Agent 已停止");
            return 0;
        } catch (const std::exception &failure)
        {
            std::cerr << "Agent 启动失败：" << failure.what() << '\n';
            return 1;
        }
    }
} // namespace EdgePulse::Agent
