/**
 * @file AgentCommandDelivery.h
 * @brief 把 Agent 消息投给本实例连接，并在真正写出后推进命令行
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Command/CommandRepository.h"
#include "Core/Http/RealtimeBus.h"

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Command
{
    class AgentChannelRegistry;

    /**
     * @brief 拼出推给 Agent 的命令正文
     * @param command 已创建的命令
     * @return std::string 形如 {"type":"command","command":{...}} 的 JSON
     */
    [[nodiscard]] std::string makeAgentCommandPayload(const CommandRecord &command);

    /**
     * @brief 把一条命令包装成 Agent 消息
     * @param command 已创建的命令
     * @return Web::AgentMessage 目标节点、命令正文与命令标识
     */
    [[nodiscard]] Web::AgentMessage makeAgentCommandMessage(const CommandRecord &command);

    /**
     * @brief 构造关闭在线连接的 Agent 消息
     * @param nodeId 目标节点
     * @param code 关闭状态码
     * @param reason 关闭原因
     * @return Web::AgentMessage 关闭消息
     */
    [[nodiscard]] Web::AgentMessage makeAgentCloseMessage(std::int64_t nodeId, int code, std::string reason);

    /**
     * @brief 本实例的 Agent 消息投递
     * @details 有连接时写出命令；close 消息关闭并摘除连接；发送成功且带命令标识时把命令
     *          从 pending 推进到 running，并在推进成功后调用可选的浏览器通知口。没有连接或写出
     *          失败时命令行保持待执行：立即命令由 Agent 的 60 秒强制轮询再取，计划命令由
     *          计划调度器按节流间隔重新发布。
     * @note 只在服务器事件循环线程上调用；内部不检查命令归属，调用方必须传全租户视图的仓储。
     */
    class AgentCommandDelivery
    {
    public:
        /// 命令推进到 running 后的浏览器通知口；参数是重新读出的命令记录
        using DispatchNotifier = std::function<AsynGyanis::Core::Task<>(const CommandRecord &)>;

        /**
         * @brief 绑定通道表与命令仓储
         * @param channels Agent 通道表
         * @param commands 全租户命令仓储，用于条件推进状态
         * @param notifier 可选的浏览器通知口；远程实例推进状态后由它补发 command.status
         */
        AgentCommandDelivery(AgentChannelRegistry &channels, std::shared_ptr<CommandRepository> commands, DispatchNotifier notifier = {});

        AgentCommandDelivery(const AgentCommandDelivery &)            = delete;
        AgentCommandDelivery &operator=(const AgentCommandDelivery &) = delete;

        /**
         * @brief 投递一条本地 Agent 消息
         * @param loop 恢复协程用的事件循环
         * @param message 命令正文或关闭指令
         * @return AsynGyanis::Core::Task<> 写出与状态推进尝试完成即返回
         * @note 数据库推进失败只记日志，不向调用方抛出；命令仍会留在 pending，由轮询再取
         */
        AsynGyanis::Core::Task<> deliver(AsynGyanis::Core::EventLoop &loop, const Web::AgentMessage &message);

    private:
        AgentChannelRegistry              &m_channels; ///< Agent 通道表
        std::shared_ptr<CommandRepository> m_commands; ///< 全租户命令仓储
        DispatchNotifier                   m_notifier; ///< 推进状态后的浏览器通知，可空
    };
} // namespace EdgePulse::Command
