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
#include <chrono>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace EdgePulse::Agent
{
    namespace
    {
        using RegistrationOutcome = std::expected<std::int64_t, RegistrationFailure>;

        constexpr int kMaximumConflictResponses = 3; ///< 单次启动允许的 409 响应次数，达到后按永久名字冲突停止

        constexpr int kMinimumHeartbeatSeconds      = 5;   ///< 心跳间隔下限
        constexpr int kMaximumHeartbeatSeconds      = 600; ///< 心跳间隔上限
        constexpr int kHeartbeatFailureWarningCount = 3;   ///< 连续失败达到该次数时额外告警一次

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
         * @brief 常驻运行期间在循环线程上共享的状态
         */
        struct RuntimeFlags
        {
            WebSocketFallbackState fallback;               ///< WebSocket 与轮询的降级状态
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
         * @brief 按 5 秒、10 秒、30 秒、60 秒的退避反复注册，直到成功、被拒绝或 409 次数用尽
         * @param loop 事件循环
         * @param client 注册客户端
         * @param registrationToken 注册或换绑令牌
         * @param agentVersion 当前版本
         * @param requestTimeoutSeconds 单次请求时限
         * @return 成功返回节点 ID；403、其他不可重试的失败或 409 达到上限时返回空
         * @details 服务端用 409 同时表示永久名字冲突与并发注册冲突，客户端不解析原因文本，
         *          只在单次启动内累计到固定次数后按永久冲突终止。
         */
        [[nodiscard]] std::optional<std::int64_t> registerWithBackoff(AsynGyanis::Core::EventLoop &loop, AgentClient &client, const std::string &registrationToken,
                                                                      const std::string &agentVersion, const int requestTimeoutSeconds)
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

            for (std::size_t attempt = 0;; ++attempt)
            {
                auto promise = std::make_shared<std::promise<RegistrationOutcome>>();
                auto future  = promise->get_future();
                auto task    = awaitRegistration(client, registrationToken, agentVersion, promise);
                loop.scheduler().scheduleRemote(task.handle());
                pendingTasks.push_back(std::move(task));

                if (future.wait_for(waitLimit) != std::future_status::ready)
                {
                    LOG_ERROR_FMT("注册请求在 {} 秒内没有返回，停止启动", waitLimit.count());
                    stopLoop();
                    return std::nullopt;
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
                std::this_thread::sleep_for(std::chrono::seconds{delaySeconds});
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
         *       确认动作只在第一次成功心跳后执行一次，之后的心跳不再重复
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
         * @note 连续失败 3 次后启用轮询；通道确认可用期间轮询暂停
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
                } else
                {
                    const bool pollingBefore = flags.fallback.shouldPoll();
                    flags.fallback.recordConnectFailure();
                    LOG_WARN_FMT("WebSocket 命令通道不可用（连续 {} 次）：{}", flags.fallback.consecutiveFailures(), outcome.error());
                    if (!pollingBefore && flags.fallback.shouldPoll())
                    {
                        LOG_WARN("WebSocket 连续失败 3 次，已启用命令轮询兜底");
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

        const std::filesystem::path rollbackMarker = m_configuration.stateDirectory / "update-rollback";
        if (update->pendingResult.empty())
        {
            // 没有待上报结果时回滚标记只可能是上一次升级清理失败的残留，已无法补报，删掉免得下次启动重复判断
            const auto removed = co_await removeMarker(loop, rollbackMarker);
            if (!removed.has_value())
            {
                LOG_WARN_FMT("清理残留的升级回滚标记失败：{}", removed.error());
            }
            co_return;
        }

        const auto rollbackFound = co_await markerExists(loop, rollbackMarker);
        if (!rollbackFound.has_value())
        {
            LOG_ERROR_FMT("{}，保留待上报结果等待下次启动", rollbackFound.error());
            co_return;
        }
        const bool rollbackPending = *rollbackFound;

        const auto parsed = AsynGyanis::Base::parseConfigValue(update->pendingResult);
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
            const AsynGyanis::Base::ConfigValue payload = AsynGyanis::Base::ConfigValue::object(
                    {{"status", "failed"}, {"result", "{}"}, {"message", message}});
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

        const std::filesystem::path healthMarker = m_configuration.stateDirectory / "update-health";
        std::expected<void, std::string> written;
        try
        {
            written = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::expected<void, std::string>>(
                    loop,
                    [healthMarker, agentVersion]() -> std::expected<void, std::string>
                    {
                        std::string writeError;
                        if (!AsynGyanis::Platform::AtomicFileWriter::writeText(healthMarker, agentVersion, std::nullopt, &writeError))
                        {
                            return std::unexpected("写入升级健康标记 " + AsynGyanis::Platform::FileSystem::utf8FromPath(healthMarker) + " 失败：" + writeError);
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
        if (!written.has_value())
        {
            LOG_ERROR_FMT("{}", written.error());
            co_return;
        }

        const AsynGyanis::Base::ConfigValue detail  = AsynGyanis::Base::ConfigValue::object({{"version", agentVersion}});
        const AsynGyanis::Base::ConfigValue payload = AsynGyanis::Base::ConfigValue::object({{"status", "succeeded"}, {"result", detail.dump()}, {"message", "升级成功"}});
        const auto                          replaced = state.replaceResult(commandId, payload.dump());
        if (!replaced.has_value())
        {
            LOG_ERROR_FMT("升级成功结果替换入队失败，保留待上报结果等待下次启动：{}", replaced.error());
            co_return;
        }

        UpdateStateRecord record = *update;
        record.healthyAt          = utcNowText();
        record.pendingResult.clear();
        const auto saved = state.setUpdateState(record);
        if (!saved.has_value())
        {
            LOG_ERROR_FMT("升级健康标记已写入，但记录确认时刻或清除待上报结果失败：{}", saved.error());
        }
        LOG_INFO_FMT("升级健康标记已写入，版本 {}，命令 {} 已按成功上报", agentVersion, commandId);
        co_return;
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
                nodeId = registerWithBackoff(registrationLoop, registrationClient, registrationToken, agentVersion, m_configuration.requestTimeoutSeconds);
            }
            if (!nodeId.has_value())
            {
                return 1;
            }

            state->set("node_id", std::to_string(*nodeId));
            state->set("agent_version", agentVersion);
            state->set("cert_fingerprint", identity->fingerprint);
            state->set("cert_expiry", identity->expiresAt);
            LOG_INFO_FMT("注册成功，节点 {} 的 ID 为 {}，指纹与版本已写入状态库", m_configuration.name, *nodeId);

            AsynGyanis::Core::EventLoop loop;
            AgentClient                 client(loop, m_configuration, *identity);
            std::vector<std::shared_ptr<CommandExecutor>> executors;
            executors.push_back(std::make_shared<ConfigExecutor>(client, m_configuration, *nodeId));
            executors.push_back(std::make_shared<FirmwareExecutor>(client, *state, m_configuration, *nodeId));
            // 安装器在另一个进程里替换二进制并等待健康标记，本进程确认它已启动后立即退出
            executors.push_back(std::make_shared<UpdateExecutor>(client, *state, m_configuration, UpdateExecutor::Launcher{}, [this] { requestStop(); }));
            // 诊断指标由遥测采集器提供，提供者本身在诊断工作线程上执行
            executors.push_back(std::make_shared<DiagnosticsExecutor>(*state, m_configuration, agentVersion, [telemetryCollector] { return telemetryCollector->metricsJson(); }));
            // 服务托管接入后，回调必须在命令结果入队并上报完成之后再重启进程，否则结果会随进程退出丢失；
            // 当前没有可用的重启动作，执行器按失败返回，避免向服务端谎报已经重启
            executors.push_back(std::make_shared<RestartExecutor>(RestartExecutor::RestartCallback{}));
            CommandDispatcher  dispatcher(client, *state, std::move(executors));
            AgentWebSocket     webSocket(loop, m_configuration);
            TelemetryPublisher telemetryPublisher(client, *state);

            RuntimeFlags flags;
            flags.heartbeatIntervalSeconds = m_configuration.heartbeatIntervalSeconds;
            flags.fallback                 = WebSocketFallbackState{!m_configuration.websocketEnabled};

            std::optional<AsynGyanis::Core::GracefulShutdown> shutdown;
            if (m_consoleMode)
            {
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
            {
                const std::lock_guard lock(m_loopMutex);
                m_loop = nullptr;
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
