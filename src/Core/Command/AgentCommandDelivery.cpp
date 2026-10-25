#include "Core/Command/AgentCommandDelivery.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Core/Command/AgentChannelRegistry.h"

#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Command
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        constexpr std::size_t kMaximumCloseReasonBytes = 123; ///< 控制帧上限 125 字节减去两字节状态码

        /**
         * @brief 把关闭状态码收口到线上允许的取值
         * @param code 候选状态码
         * @return std::uint16_t 1000-1003、1007-1014 或 3000-4999 原样返回，其余退回 1008
         * @note 1005/1006/1015 是保留哨兵值，1016-2999 段未经注册，发出去对端只能按协议错误收口
         */
        [[nodiscard]] std::uint16_t normalizeCloseCode(const int code)
        {
            if ((code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) || (code >= 3000 && code <= 4999))
            {
                return static_cast<std::uint16_t>(code);
            }
            return 1008;
        }

        /**
         * @brief 把关闭原因裁到控制帧上限内且不截断 UTF-8 字符
         * @param reason 原始原因
         * @return std::string 不超过 123 字节的原因
         */
        [[nodiscard]] std::string truncateCloseReason(const std::string_view reason)
        {
            if (reason.size() <= kMaximumCloseReasonBytes)
            {
                return std::string{reason};
            }
            std::size_t length = kMaximumCloseReasonBytes;
            // 第一个被排除的字节若是续字节，就继续退到它的前导字节之前，避免留下半个字符
            while (length > 0 && (static_cast<unsigned char>(reason[length]) & 0xC0U) == 0x80U)
            {
                --length;
            }
            return std::string{reason.substr(0, length)};
        }
    } // namespace

    std::string makeAgentCommandPayload(const CommandRecord &command)
    {
        const ConfigValue commandValue = ConfigValue::object(
                {{"id", command.id}, {"nodeId", command.nodeId}, {"type", command.type}, {"payload", command.payload}, {"timeoutSeconds", command.timeoutSeconds}});
        return ConfigValue::object({{"type", "command"}, {"command", commandValue}}).dump();
    }

    Web::AgentMessage makeAgentCommandMessage(const CommandRecord &command)
    {
        return Web::AgentMessage{.nodeId = command.nodeId, .payload = makeAgentCommandPayload(command), .commandId = command.id};
    }

    Web::AgentMessage makeAgentCloseMessage(const std::int64_t nodeId, const int code, std::string reason)
    {
        return Web::AgentMessage{.nodeId = nodeId, .close = true, .closeCode = code, .closeReason = std::move(reason)};
    }

    AgentCommandDelivery::AgentCommandDelivery(AgentChannelRegistry &channels, std::shared_ptr<CommandRepository> commands, DispatchNotifier notifier) :
        m_channels(channels), m_commands(std::move(commands)), m_notifier(std::move(notifier))
    {
    }

    AsynGyanis::Core::Task<> AgentCommandDelivery::deliver(AsynGyanis::Core::EventLoop &loop, const Web::AgentMessage &message)
    {
        if (message.nodeId <= 0)
        {
            co_return;
        }
        if (message.close)
        {
            try
            {
                co_await m_channels.closeNode(message.nodeId, normalizeCloseCode(message.closeCode), truncateCloseReason(message.closeReason));
            } catch (const std::exception &failure)
            {
                LOG_WARN_FMT("关闭 Agent 连接失败：node={} error={}", message.nodeId, failure.what());
            }
            co_return;
        }
        if (message.payload.empty() || !m_channels.isConnected(message.nodeId))
        {
            co_return;
        }
        bool sent = false;
        try
        {
            sent = co_await m_channels.send(message.nodeId, message.payload);
        } catch (const std::exception &failure)
        {
            LOG_WARN_FMT("Agent 命令写出失败：node={} command={} error={}", message.nodeId, message.commandId, failure.what());
            co_return;
        }
        if (!sent || message.commandId <= 0 || m_commands == nullptr)
        {
            co_return;
        }
        try
        {
            // 条件更新只命中 pending：别的实例已经推进过的命令不会被改写下发时间
            const bool advanced = co_await m_commands->markDispatched(loop, message.commandId);
            if (advanced && m_notifier)
            {
                try
                {
                    // 重新读库再通知：浏览器需要 type 与最新状态，不能只凭消息正文拼一条不完整的记录
                    const auto updated = co_await m_commands->find(loop, message.commandId);
                    if (updated.has_value())
                    {
                        co_await m_notifier(*updated);
                    }
                } catch (const std::exception &failure)
                {
                    // 状态已经推进；通知失败只影响界面刷新，不能回滚命令也不能抛回 HTTP 层
                    LOG_WARN_FMT("命令已推进但浏览器通知失败：command={} error={}", message.commandId, failure.what());
                }
            }
        } catch (const std::exception &failure)
        {
            // 命令正文已经写出；状态推进失败只影响界面读数，命令行留在 pending 由轮询再取
            LOG_WARN_FMT("命令已写出但状态推进失败：command={} error={}", message.commandId, failure.what());
        }
        co_return;
    }
} // namespace EdgePulse::Command
