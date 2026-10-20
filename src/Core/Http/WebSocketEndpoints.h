/**
 * @file WebSocketEndpoints.h
 * @brief 浏览器 WebSocket 升级、首条认证与节点状态扇出
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/WebSocket/WebSocketHub.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace AsynGyanis::Net
{
    class Router;
    class WebSocketPeer;
} // namespace AsynGyanis::Net

namespace EdgePulse::Web
{
    /**
     * @brief 认证后的实时订阅范围
     */
    struct BrowserSubscriptionScope
    {
        std::int64_t tenantId{0};          ///< 会话租户
        bool         platformAdmin{false}; ///< 平台管理员订阅跨租户主题
    };

    /**
     * @brief 浏览器 WebSocket 端点
     * @details 升级后 5 秒内必须发来 {"type":"auth","token":"..."}，否则按 1008 关闭；
     *          认证成功后按会话租户订阅 nodes/alarms/commands 主题，平台管理员订阅跨租户主题。
     */
    class BrowserWebSocket
    {
    public:
        /**
         * @brief 构造
         * @param loop 服务器所属事件循环
         */
        explicit BrowserWebSocket(AsynGyanis::Core::EventLoop &loop);

        ~BrowserWebSocket();

        BrowserWebSocket(const BrowserWebSocket &)            = delete;
        BrowserWebSocket &operator=(const BrowserWebSocket &) = delete;

        /**
         * @brief 集线器，供 App 发布节点状态
         */
        [[nodiscard]] AsynGyanis::Net::WebSocketHub &hub() noexcept;

        /**
         * @brief 注册 /api/ws 路由
         * @param router 目标路由器
         * @param tokenValidator 校验首条消息里的会话令牌并给出租户订阅范围
         * @param allowedOrigins Origin 白名单；空表示只允许同源
         */
        void registerRoute(AsynGyanis::Net::Router &router, std::function<AsynGyanis::Core::Task<std::optional<BrowserSubscriptionScope>>(std::string_view)> tokenValidator,
                           std::vector<std::string> allowedOrigins);

    private:
        /**
         * @brief 启动首条认证的 5 秒兜底关闭
         * @param peer 当前连接
         * @param authenticated 认证成功标记
         * @param finished 处理器退出标记
         * @note 该路径用一条休眠线程代替引擎缺失的带超时 receive；见实现里的 shortcut 说明
         */
        void startAuthenticationTimer(AsynGyanis::Net::WebSocketPeer &peer, std::shared_ptr<std::atomic<bool>> authenticated, std::shared_ptr<std::atomic<bool>> finished);

        AsynGyanis::Core::EventLoop          &m_loop;              ///< 服务器循环
        AsynGyanis::Net::WebSocketHub         m_hub;               ///< 进程内节点主题扇出
        std::atomic<bool>                     m_isStopping{false}; ///< 析构开始标记
        std::mutex                            m_threadsMutex;      ///< 保护计时线程表
        std::vector<std::jthread>             m_authThreads;       ///< 未完成的首条认证计时线程
        std::mutex                            m_tasksMutex;        ///< 保护超时关闭任务帧
        std::vector<AsynGyanis::Core::Task<>> m_timeoutTasks;      ///< 超时关闭任务，活到端点点析构
    };
} // namespace EdgePulse::Web
