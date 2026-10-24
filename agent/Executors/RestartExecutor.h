/**
 * @file RestartExecutor.h
 * @brief agent.restart 执行器：只把结果标记为需要重启，由分发器在结果上报之后触发
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Executors/CommandExecutor.h"

#include <string_view>

namespace EdgePulse::Agent
{
    /**
     * @brief 处理 agent.restart 命令
     * @details 执行器不直接操作平台服务控制，只交回带 restartAfterResult 的成功结果。
     *          真正的重启由 CommandDispatcher 在结果行入队并尝试上报之后调用重启处理器完成，
     *          否则进程退出会把尚未落盘的结果一起带走。
     */
    class RestartExecutor final : public CommandExecutor
    {
    public:
        /**
         * @brief 装配重启执行器
         * @param restartConfigured 是否已为分发器配置重启处理器；未配置时命令按失败返回
         */
        explicit RestartExecutor(bool restartConfigured);

        /**
         * @brief 是否处理 agent.restart
         * @param type 命令类型
         * @return true 表示由本执行器执行
         */
        bool supports(std::string_view type) const override;

        /**
         * @brief 受理一次重启命令
         * @param loop 命令所属事件循环
         * @param command 已解析的命令，正文当前未使用
         * @return 已配置重启动作时返回 succeeded 且 restartAfterResult 为 true
         */
        AsynGyanis::Core::Task<CommandResult> execute(AsynGyanis::Core::EventLoop &loop, const AgentCommand &command) override;

    private:
        bool m_restartConfigured{false}; ///< 分发器是否会在结果上报后执行重启
    };
} // namespace EdgePulse::Agent
