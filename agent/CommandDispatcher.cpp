#include "CommandDispatcher.h"

#include "AgentClient.h"
#include "AgentState.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"

#include "Core/EventLoop/EventLoop.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Agent
{
    namespace
    {
        constexpr std::size_t kResultBatchLimit = 10; ///< 单次结果上报最多读取的条数

        /**
         * @brief 取对象里的字符串字段
         * @param value JSON 对象
         * @param key 字段名
         * @param fallback 缺失或类型不符时的回退值
         * @return std::string 字段文本
         */
        [[nodiscard]] std::string textField(const AsynGyanis::Base::ConfigValue &value, const std::string_view key, std::string fallback = {})
        {
            const auto iterator = value.find(std::string{key});
            if (iterator == value.end() || !iterator->is_string())
            {
                return fallback;
            }
            return iterator->get<std::string>();
        }

        /**
         * @brief 把结果对象序列化成 JSON 文本
         * @param payload 结果对象
         * @param fallbackMessage 序列化失败时写进日志的中文原因
         * @return std::string 可上报的 JSON 文本
         */
        [[nodiscard]] std::string resultPayload(const AsynGyanis::Base::ConfigValue &payload, const std::string_view fallbackMessage)
        {
            const auto serialized = AsynGyanis::Base::serializeConfigValue(payload);
            if (serialized.has_value())
            {
                return *serialized;
            }
            LOG_ERROR_FMT("{}", fallbackMessage);
            return R"({"status":"failed","result":"{}","message":"命令结果无法序列化为 JSON"})";
        }
    } // namespace

    std::expected<AgentCommand, AgentCommandParseFailure> parseAgentCommand(const AsynGyanis::Base::ConfigValue &value)
    {
        if (!value.is_object())
        {
            return std::unexpected(AgentCommandParseFailure{0, "命令正文不是 JSON 对象"});
        }

        AgentCommand command;
        const auto   id = value.find("id");
        if (id == value.end())
        {
            return std::unexpected(AgentCommandParseFailure{0, "命令缺少 id"});
        }
        const auto parsedId = AsynGyanis::Base::configValueAs<std::int64_t>(*id);
        if (!parsedId.has_value() || *parsedId <= 0)
        {
            return std::unexpected(AgentCommandParseFailure{0, "命令 id 不是正整数"});
        }
        command.id = *parsedId;

        const auto type = value.find("type");
        if (type == value.end() || !type->is_string() || type->get<std::string>().empty())
        {
            return std::unexpected(AgentCommandParseFailure{command.id, "命令 " + std::to_string(command.id) + " 的 type 缺失、不是字符串或为空"});
        }
        command.type = type->get<std::string>();

        const auto payload = value.find("payload");
        if (payload == value.end() || !payload->is_string())
        {
            return std::unexpected(AgentCommandParseFailure{command.id, "命令 " + std::to_string(command.id) + " 的 payload 缺失或不是字符串"});
        }
        command.payload = payload->get<std::string>();

        const auto timeout       = value.find("timeoutSeconds");
        const auto parsedTimeout = timeout == value.end() ? std::nullopt : AsynGyanis::Base::configValueAs<int>(*timeout);
        if (!parsedTimeout.has_value() || *parsedTimeout <= 0)
        {
            return std::unexpected(AgentCommandParseFailure{command.id, "命令 " + std::to_string(command.id) + " 的 timeoutSeconds 缺失或不是正整数"});
        }
        command.timeoutSeconds = *parsedTimeout;
        return command;
    }

    CommandDispatcher::CommandDispatcher(AgentClient &client, AgentState &state, std::vector<std::shared_ptr<CommandExecutor>> executors, RestartHandler restartHandler) :
        m_client(client), m_state(state), m_executors(std::move(executors)), m_restartHandler(std::move(restartHandler))
    {
    }

    std::optional<std::int64_t> CommandDispatcher::nodeId()
    {
        const auto text = m_state.get("node_id");
        if (!text.has_value() || text->empty())
        {
            return std::nullopt;
        }
        std::int64_t value  = 0;
        const auto   parsed = std::from_chars(text->data(), text->data() + text->size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size() || value <= 0)
        {
            return std::nullopt;
        }
        return value;
    }

    void CommandDispatcher::enqueueFailure(const std::int64_t commandId, const std::string &message)
    {
        const AsynGyanis::Base::ConfigValue payload = AsynGyanis::Base::ConfigValue::object({{"status", "failed"}, {"result", "{}"}, {"message", message}});
        m_state.enqueueResult(commandId, resultPayload(payload, "命令失败结果无法序列化为 JSON，已改用固定文本"));
    }

    bool CommandDispatcher::alreadyHandled(const std::int64_t commandId) const
    {
        if (m_inFlight.contains(commandId) || m_completed.contains(commandId) || m_state.hasResult(commandId))
        {
            return true;
        }
        return std::any_of(m_pending.begin(), m_pending.end(), [commandId](const AgentCommand &pending) { return pending.id == commandId; });
    }

    bool CommandDispatcher::hasInFlight() const
    {
        return m_executing || !m_inFlight.empty() || !m_pending.empty();
    }

    void CommandDispatcher::rememberCompleted(const std::int64_t commandId)
    {
        if (!m_completed.insert(commandId).second)
        {
            return;
        }
        m_completedOrder.push_back(commandId);
        while (m_completedOrder.size() > kCompletedCommandLimit)
        {
            m_completed.erase(m_completedOrder.front());
            m_completedOrder.pop_front();
        }
    }

    AsynGyanis::Core::Task<bool> CommandDispatcher::execute(AgentCommand command)
    {
        m_inFlight.insert(command.id);

        CommandResult outcome;
        try
        {
            AsynGyanis::Core::EventLoop *loop = AsynGyanis::Core::EventLoop::currentEventLoop();
            if (loop == nullptr)
            {
                outcome = CommandResult{false, "{}", "命令分发不在事件循环线程上，已放弃执行"};
            } else
            {
                std::shared_ptr<CommandExecutor> selected;
                for (const auto &executor: m_executors)
                {
                    if (executor != nullptr && executor->supports(command.type))
                    {
                        selected = executor;
                        break;
                    }
                }
                if (selected == nullptr)
                {
                    outcome = CommandResult{false, "{}", "Agent 不支持命令类型 " + command.type};
                } else
                {
                    outcome = co_await selected->execute(*loop, command);
                }
            }
        } catch (const std::exception &failure)
        {
            outcome = CommandResult{false, "{}", "执行命令 " + command.type + " 时发生异常：" + failure.what()};
        } catch (...)
        {
            outcome = CommandResult{false, "{}", "执行命令 " + command.type + " 时发生未知异常"};
        }

        const bool                          restartAfterResult = outcome.succeeded && outcome.restartAfterResult;
        const AsynGyanis::Base::ConfigValue payload =
                AsynGyanis::Base::ConfigValue::object({{"status", outcome.succeeded ? "succeeded" : "failed"}, {"result", outcome.result}, {"message", outcome.message}});
        m_state.enqueueResult(command.id, resultPayload(payload, "命令执行结果无法序列化为 JSON，已改用固定文本"));
        rememberCompleted(command.id);
        m_inFlight.erase(command.id);
        co_return restartAfterResult;
    }

    AsynGyanis::Core::Task<> CommandDispatcher::triggerRestart()
    {
        if (!m_restartHandler)
        {
            LOG_ERROR("命令要求重启，但分发器没有配置重启处理器");
            co_return;
        }
        try
        {
            co_await m_restartHandler();
        } catch (const std::exception &failure)
        {
            LOG_ERROR_FMT("执行服务重启处理器失败：{}", failure.what());
        } catch (...)
        {
            LOG_ERROR("执行服务重启处理器失败：未知异常");
        }
        co_return;
    }

    AsynGyanis::Core::Task<> CommandDispatcher::runExclusive(AgentCommand command)
    {
        m_executing = true;
        struct ExecutingReset
        {
            bool &executing; ///< 分发器的串行执行标记

            ~ExecutingReset()
            {
                executing = false;
            }
        } reset{m_executing};

        for (;;)
        {
            const bool        restartAfterResult = co_await execute(std::move(command));
            const ResultFlush flushed            = co_await flushResults();
            if (flushed.failed)
            {
                LOG_WARN("命令结果未获服务端确认，已留在本地队列等待重试");
            }
            if (restartAfterResult)
            {
                // 结果行已入队并尝试上报；即使上报失败也重启，队列会在新进程里继续补报。
                // 进程即将重启，等待队列里的命令不再执行
                co_await triggerRestart();
                co_return;
            }
            if (m_pending.empty())
            {
                co_return;
            }
            command = std::move(m_pending.front());
            m_pending.pop_front();
            LOG_INFO_FMT("开始执行排队命令：command={} type={}", command.id, command.type);
        }
    }

    AsynGyanis::Core::Task<> CommandDispatcher::onWebSocketCommand(AgentCommand command)
    {
        if (command.id <= 0)
        {
            LOG_WARN("收到标识无效的命令，已跳过");
            co_return;
        }
        if (alreadyHandled(command.id))
        {
            LOG_WARN_FMT("命令 {} 正在执行、排队中、已有结果或已执行完成，忽略重复投递", command.id);
            co_return;
        }
        if (m_executing)
        {
            // 执行器可能已经挂起等待网络或外部进程；新命令只排队，不能在同一循环上并发跑第二个
            LOG_INFO_FMT("命令正在执行，命令 {} 进入等待队列", command.id);
            m_pending.push_back(std::move(command));
            co_return;
        }

        co_await runExclusive(std::move(command));
        co_return;
    }

    AsynGyanis::Core::Task<> CommandDispatcher::failCommand(const std::int64_t commandId, std::string message)
    {
        if (commandId <= 0)
        {
            LOG_WARN_FMT("命令正文无效且没有可用的标识，无法上报失败结果：{}", message);
            co_return;
        }
        if (alreadyHandled(commandId))
        {
            LOG_WARN_FMT("命令 {} 正在执行、排队中、已执行完成或已有结果，忽略失败上报", commandId);
            co_return;
        }

        LOG_WARN_FMT("命令 {} 无法执行：{}", commandId, message);
        enqueueFailure(commandId, message);
        rememberCompleted(commandId);
        const ResultFlush flushed = co_await flushResults();
        if (flushed.failed)
        {
            LOG_WARN_FMT("命令 {} 的失败结果未获服务端确认，已留在本地队列等待重试", commandId);
        }
        co_return;
    }

    AsynGyanis::Core::Task<> CommandDispatcher::pollOnce()
    {
        const auto node = nodeId();
        if (!node.has_value())
        {
            LOG_ERROR("状态库缺少可用的 node_id，跳过本轮命令轮询");
            co_return;
        }

        const auto next = co_await m_client.takeNextCommand(*node);
        if (!next.has_value())
        {
            LOG_WARN_FMT("拉取命令失败：{}", next.error());
            co_return;
        }
        if (!next->has_value())
        {
            co_return;
        }

        const auto parsed = parseAgentCommand(**next);
        if (!parsed.has_value())
        {
            if (parsed.error().commandId > 0)
            {
                co_await failCommand(parsed.error().commandId, parsed.error().message);
            } else
            {
                LOG_WARN_FMT("收到无法识别的命令，已跳过：{}", parsed.error().message);
            }
            co_return;
        }

        co_await onWebSocketCommand(std::move(*parsed));
        co_return;
    }

    AsynGyanis::Core::Task<ResultFlush> CommandDispatcher::flushResults()
    {
        ResultFlush report;
        if (m_flushing)
        {
            report.busy = true;
            co_return report;
        }
        m_flushing = true;
        struct FlushingReset
        {
            bool &flushing; ///< 分发器的上报标记

            ~FlushingReset()
            {
                flushing = false;
            }
        } reset{m_flushing};

        const auto node = nodeId();
        if (!node.has_value())
        {
            LOG_ERROR("状态库缺少可用的 node_id，命令结果暂不上报");
            report.failed = true;
            co_return report;
        }

        const std::vector<QueuedResult> queued = m_state.takeResults(kResultBatchLimit);
        for (const QueuedResult &row: queued)
        {
            const auto payload = AsynGyanis::Base::parseConfigValue(row.payload);
            if (!payload.has_value() || !payload->is_object())
            {
                LOG_ERROR_FMT("命令 {} 的结果正文已损坏，无法上报，已从队列删除：{}", row.commandId, row.payload);
                m_state.ackResult(row.id);
                continue;
            }

            // 服务端只认 succeeded 与 failed；队列里的其他取值一律按失败提交
            const std::string       rawStatus = textField(*payload, "status", "failed");
            const std::string       status    = rawStatus == "succeeded" ? "succeeded" : "failed";
            const std::string       result    = textField(*payload, "result", "{}");
            const std::string       message   = textField(*payload, "message");
            const ResultPostOutcome posted    = co_await m_client.reportCommandResult(row.commandId, *node, status, result, message);
            if (posted.disposition == ResultPostDisposition::Terminal)
            {
                // 终态拒绝不会因重试而改变；保留会永久挡住后续结果并让队列上限丢数据
                LOG_ERROR_FMT("命令 {} 的结果被服务端以 HTTP {} 拒绝，重试不会改变结论，已从本地队列删除：{}", row.commandId, posted.httpStatus, posted.message);
                m_state.ackResult(row.id);
                continue;
            }
            if (posted.disposition == ResultPostDisposition::Retryable)
            {
                LOG_WARN_FMT("命令 {} 的结果上报失败，保留待重试：{}", row.commandId, posted.message);
                report.failed = true;
                co_return report;
            }

            m_state.ackResult(row.id);
            ++report.delivered;
        }
        co_return report;
    }
} // namespace EdgePulse::Agent
