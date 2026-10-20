/**
 * @file CommandController.h
 * @brief 命令中心 HTTP 路由
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Command/CommandRepository.h"

#include <Core/Coroutine/Task.h>

#include <functional>

namespace AsynGyanis::Core
{
    class EventLoop;
}
namespace AsynGyanis::Net
{
    class Router;
}
namespace EdgePulse::Auth
{
    class AuthService;
}
namespace EdgePulse::Command
{
    class AgentChannelRegistry;
}

namespace EdgePulse::Command
{
    /**
     * @brief 命令变更通知
     */
    using CommandNotifier = std::function<AsynGyanis::Core::Task<>(const CommandRecord &)>;

    /**
     * @brief 命令路由依赖
     */
    struct CommandDependencies
    {
        Auth::AuthService    *auth{nullptr};     ///< 认证
        CommandRepository    *commands{nullptr}; ///< 命令仓储
        AgentChannelRegistry *channels{nullptr}; ///< Agent 通道
        CommandNotifier       notifier;          ///< 浏览器通知
    };

    /**
     * @brief 注册浏览器命令路由
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 依赖
     */
    void registerCommandRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const CommandDependencies &dependencies);

    /**
     * @brief 注册 Agent 命令轮询与结果路由
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 依赖
     */
    void registerCommandAgentRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const CommandDependencies &dependencies);
} // namespace EdgePulse::Command
