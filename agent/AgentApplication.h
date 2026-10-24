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
     * @details 当前骨架只负责状态目录、日志与启动日志；身份、心跳与执行器由后续装配接入。
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
         * @brief 创建状态目录、初始化日志并记录启动日志
         * @return int 成功返回 0；目录或日志初始化失败返回 1
         */
        int run();

    private:
        AgentConfiguration m_configuration; ///< 本次进程使用的配置副本
    };
} // namespace EdgePulse::Agent
