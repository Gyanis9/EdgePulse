/**
 * @file CommandExecutor.h
 * @brief 命令模型与执行器接口
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Config/ConfigValue.h"
#include "Core/Coroutine/Task.h"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace AsynGyanis::Core
{
    class EventLoop;
} // namespace AsynGyanis::Core

namespace EdgePulse::Agent
{
    /**
     * @brief 一条从服务端下发的命令
     */
    struct AgentCommand
    {
        std::int64_t id{0};              ///< 服务端命令标识，正整数
        std::string  type;               ///< 命令类型，决定由哪个执行器处理
        std::string  payload;            ///< 服务端给出的正文，JSON 文本
        int          timeoutSeconds{60}; ///< 服务端给出的执行时限
    };

    /**
     * @brief 一次命令执行的结果
     */
    struct CommandResult
    {
        bool        succeeded{false}; ///< 是否成功；对应上报状态 succeeded 或 failed
        std::string result{"{}"};     ///< 结果正文，JSON 文本
        std::string message;          ///< 面向运维的中文说明
    };

    /**
     * @brief 命令正文无法解析时的结论
     */
    struct AgentCommandParseFailure
    {
        std::int64_t commandId{0}; ///< 能识别出的命令标识；0 表示无法按标识回报结果
        std::string  message;      ///< 中文原因
    };

    /**
     * @brief 把服务端给出的命令对象解析成 AgentCommand
     * @param value 命令对象，来源是轮询响应的 data 或 WebSocket 的 command 字段
     * @return 成功返回命令；失败时 commandId 非零表示仍可按该标识上报失败结果
     * @note 缺少或非正整数的 id、空的 type、非字符串的 payload、非正整数的 timeoutSeconds 都拒绝
     */
    [[nodiscard]] std::expected<AgentCommand, AgentCommandParseFailure> parseAgentCommand(const AsynGyanis::Base::ConfigValue &value);

    /**
     * @brief 一种命令类型的执行器
     * @details 执行器自己负责超时，并且无论成败都要交回结果；分发器不强制取消执行中的协程。
     *          所有方法都只在事件循环线程上调用。
     */
    class CommandExecutor
    {
    public:
        virtual ~CommandExecutor() = default;

        /**
         * @brief 是否处理该命令类型
         * @param type 命令类型
         * @return true 表示由本执行器执行
         */
        virtual bool supports(std::string_view type) const = 0;

        /**
         * @brief 执行一条命令
         * @param loop 命令所属事件循环
         * @param command 已解析的命令
         * @return 执行结果；失败也要交回结果，不能抛出后静默丢弃
         */
        virtual AsynGyanis::Core::Task<CommandResult> execute(AsynGyanis::Core::EventLoop &loop, const AgentCommand &command) = 0;
    };
} // namespace EdgePulse::Agent
