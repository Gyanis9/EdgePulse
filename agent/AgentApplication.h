/**
 * @file AgentApplication.h
 * @brief Agent 进程的装配入口
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AgentConfiguration.h"

namespace EdgePulse::Agent
{
    /**
     * @brief 按配置装配 Agent 运行期资源
     * @details 负责状态目录、日志、证书身份、状态库与注册；心跳与执行器由后续装配接入。
     */
    class AgentApplication
    {
    public:
        /**
         * @brief 保存一份已校验的配置
         * @param configuration 已通过形态校验的配置
         */
        explicit AgentApplication(AgentConfiguration configuration);

        /**
         * @brief 装配日志与身份、打开状态库并完成首次注册
         * @return int 注册成功返回 0；目录、日志、身份、状态库或注册失败返回 1
         * @note 网络与 5xx 失败按退避重试；403 停止并记录 WARN
         */
        int run();

    private:
        AgentConfiguration m_configuration; ///< 本次进程使用的配置副本
    };
} // namespace EdgePulse::Agent
