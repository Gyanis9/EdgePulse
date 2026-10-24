/**
 * @file DiagnosticsExecutor.h
 * @brief diagnostics.collect 执行器：汇总版本、状态、指标与有界日志尾
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AgentConfiguration.h"
#include "Executors/CommandExecutor.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace EdgePulse::Agent
{
    class AgentState;

    /**
     * @brief 处理 diagnostics.collect 命令
     * @details 指标由外部提供者采集，日志尾与指标读取都在阻塞执行器上完成；
     *          任一提供者缺失或读取失败只降级为空并记录 WARN，命令仍按成功返回。
     *          序列化结果不超过 256 KiB。
     */
    class DiagnosticsExecutor final : public CommandExecutor
    {
    public:
        /// 返回 JSON 指标数组文本的提供者；空回调表示当前没有指标来源
        using MetricsProvider = std::function<std::string()>;

        /**
         * @brief 装配诊断执行器
         * @param state 本地状态库，读取配置版本与队列深度
         * @param configuration 已校验的配置，使用节点名、日志路径与日志尾上限
         * @param agentVersion 当前 Agent 版本
         * @param metricsProvider 指标提供者；为空时 metrics 返回空数组
         */
        DiagnosticsExecutor(AgentState &state, AgentConfiguration configuration, std::string agentVersion, MetricsProvider metricsProvider);

        /**
         * @brief 是否处理 diagnostics.collect
         * @param type 命令类型
         * @return true 表示由本执行器执行
         */
        bool supports(std::string_view type) const override;

        /**
         * @brief 执行一次诊断采集
         * @param loop 命令所属事件循环
         * @param command 已解析的命令，正文当前未使用
         * @return 执行结果；结果正文是诊断 JSON 对象
         */
        AsynGyanis::Core::Task<CommandResult> execute(AsynGyanis::Core::EventLoop &loop, const AgentCommand &command) override;

    private:
        AgentState         &m_state;         ///< 本地状态库
        AgentConfiguration  m_configuration; ///< 本次进程使用的配置副本
        std::string         m_agentVersion;  ///< 当前 Agent 版本
        MetricsProvider     m_metricsProvider; ///< 指标提供者
    };
} // namespace EdgePulse::Agent
