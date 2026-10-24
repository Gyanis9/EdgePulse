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

#include <atomic>

namespace AsynGyanis::Core
{
    class EventLoop;
} // namespace AsynGyanis::Core

namespace EdgePulse::Agent
{
    /**
     * @brief 按配置装配 Agent 运行期资源并常驻运行
     * @details 负责状态目录、日志、证书身份、状态库与注册；注册成功后在同一条事件循环上
     *          运行心跳、WebSocket 命令通道与轮询兜底，直到收到停止请求。
     */
    class AgentApplication
    {
    public:
        /**
         * @brief 保存一份已校验的配置
         * @param configuration 已通过形态校验的配置
         * @param consoleMode 是否以控制台方式运行；为 true 时接管 SIGINT/SIGTERM 并转为停止请求
         */
        explicit AgentApplication(AgentConfiguration configuration, bool consoleMode = false);

        /**
         * @brief 装配日志与身份、打开状态库、完成首次注册并常驻运行
         * @return int 正常停止返回 0；目录、日志、身份、状态库或注册失败返回 1
         * @note 状态库已有节点标识时优先使用换绑令牌；网络、5xx 与少量 409 失败按退避重试；
         *       403 立即停止，409 累计 3 次后按名字冲突停止并记录 ERROR
         */
        int run();

        /**
         * @brief 请求停止常驻运行
         * @note 可被任意线程调用；在注册阶段调用时，注册完成后的常驻段会直接跳过
         */
        void requestStop();

    private:
        AgentConfiguration                         m_configuration;        ///< 本次进程使用的配置副本
        bool                                       m_consoleMode{false};   ///< 是否接管控制台停机信号
        std::atomic<bool>                          m_stopRequested{false}; ///< 是否已请求停止
        std::atomic<AsynGyanis::Core::EventLoop *> m_loop{nullptr};        ///< 常驻事件循环，仅在 run() 期间有效
    };
} // namespace EdgePulse::Agent
