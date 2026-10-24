/**
 * @file RestartExecutor.h
 * @brief agent.restart 执行器：把重启动作交给注入的回调
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Executors/CommandExecutor.h"

#include <functional>
#include <string_view>

namespace EdgePulse::Agent
{
    /**
     * @brief 处理 agent.restart 命令
     * @details 执行器只调用注入的回调，不直接操作平台服务控制。
     *          真正的服务重启必须在命令结果写入本地队列并完成一次上报之后再触发，
     *          否则进程退出会把尚未落盘的结果一起带走；服务托管接入时由回调承担这段时序。
     */
    class RestartExecutor final : public CommandExecutor
    {
    public:
        /// 重启动作；实现方负责在结果上报之后重启服务
        using RestartCallback = std::function<void()>;

        /**
         * @brief 装配重启执行器
         * @param restartCallback 重启回调；为空时命令按失败返回
         */
        explicit RestartExecutor(RestartCallback restartCallback);

        /**
         * @brief 是否处理 agent.restart
         * @param type 命令类型
         * @return true 表示由本执行器执行
         */
        bool supports(std::string_view type) const override;

        /**
         * @brief 执行一次重启命令
         * @param loop 命令所属事件循环
         * @param command 已解析的命令，正文当前未使用
         * @return 执行结果；回调抛出异常时按失败返回
         */
        AsynGyanis::Core::Task<CommandResult> execute(AsynGyanis::Core::EventLoop &loop, const AgentCommand &command) override;

    private:
        RestartCallback m_restartCallback; ///< 注入的重启动作
    };
} // namespace EdgePulse::Agent
