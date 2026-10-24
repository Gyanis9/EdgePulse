/**
 * @file AgentWebSocket.h
 * @brief Agent 的命令长连接
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AgentConfiguration.h"

#include "Core/Coroutine/Task.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace AsynGyanis::Core
{
    class EventLoop;
    class TlsContext;
} // namespace AsynGyanis::Core

namespace EdgePulse::Agent
{
    class CommandDispatcher;

    /**
     * @brief 按连续失败次数取 WebSocket 重连退避秒数
     * @param consecutiveFailures 连续失败次数，0 表示刚断开或从未失败
     * @return int 退避秒数，依次为 1、2、5、15、30、60，超出表长后一直为 60
     */
    [[nodiscard]] int webSocketReconnectBackoffSeconds(int consecutiveFailures);

    /**
     * @brief WebSocket 通道与命令轮询之间的降级状态
     * @details 连续失败达到 3 次后启用轮询；通道通过 hello 确认可用时暂停轮询并清零失败计数。
     *          正常断开只标记通道不可用，轮询保持暂停，直到再次连续失败 3 次。
     * @note 只在事件循环线程上使用，内部不做同步。
     */
    class WebSocketFallbackState
    {
    public:
        /**
         * @brief 构造降级状态
         * @param pollingInitiallyActive 初始是否启用轮询；关闭 WebSocket 时为 true
         */
        explicit WebSocketFallbackState(bool pollingInitiallyActive = false);

        /**
         * @brief 记录通道通过 hello 确认可用
         */
        void recordConnected();

        /**
         * @brief 记录通道正常断开
         * @details 不清零失败计数；轮询暂停状态保持不变
         */
        void recordDisconnected();

        /**
         * @brief 记录一次连接失败
         * @details 连续失败达到 3 次后启用轮询
         */
        void recordConnectFailure();

        /**
         * @brief 轮询是否应当运行
         * @return 已触发降级且通道当前不可用时返回 true
         */
        [[nodiscard]] bool shouldPoll() const;

        /**
         * @brief 当前连续连接失败次数
         * @return int 连续失败次数
         */
        [[nodiscard]] int consecutiveFailures() const;

    private:
        bool m_connected{false};           ///< 通道是否已通过 hello 确认
        bool m_pollingActive{false};       ///< 是否已启用轮询
        int  m_consecutiveFailures{0};     ///< 连续连接失败次数
    };

    /**
     * @brief 按配置建立 wss 或 ws 命令通道，完成 hello 并分发服务端推送
     * @details 地址从 base_url 派生：https 换成 wss、http 换成 ws，路径固定为 /api/agent/ws。
     *          连接、hello 与心跳式文本 ping 都在本类处理，命令交给 CommandDispatcher。
     * @note 所有方法都只在构造时给出的那个事件循环线程上调用。
     */
    class AgentWebSocket
    {
    public:
        /**
         * @brief 连接状态变化回调
         * @param connected true 表示 hello 已确认、通道可用；false 表示通道已断开
         */
        using ConnectionObserver = std::function<void(bool)>;

        /**
         * @brief 从配置派生地址并准备 TLS 上下文
         * @param loop 所属事件循环
         * @param configuration 已校验的配置
         * @note 地址解析或客户端证书装载失败不会抛出，而是在 serve() 时返回中文原因
         */
        AgentWebSocket(AsynGyanis::Core::EventLoop &loop, const AgentConfiguration &configuration);

        ~AgentWebSocket();

        AgentWebSocket(const AgentWebSocket &)            = delete;
        AgentWebSocket &operator=(const AgentWebSocket &) = delete;

        /**
         * @brief 建立一次连接并持续接收命令，直到对端关闭或读取失败
         * @param dispatcher 命令分发器
         * @param nodeId 节点标识
         * @param agentVersion 当前 Agent 版本
         * @param onConnectionChanged 连接状态回调，只在循环线程上调用
         * @return 通道正常收口时成功；连接或 hello 失败时返回中文原因供上层退避
         */
        AsynGyanis::Core::Task<std::expected<void, std::string>> serve(CommandDispatcher &dispatcher, std::int64_t nodeId, std::string agentVersion,
                                                                       const ConnectionObserver &onConnectionChanged);

    private:
        /**
         * @brief 从 base_url 拆出的连接参数
         */
        struct Endpoint
        {
            bool          tls{false}; ///< 是否走 wss
            std::string   host;       ///< 主机名或 IP 字面量
            std::uint16_t port{0};    ///< 端口
            std::string   target;     ///< origin-form 请求目标
        };

        AsynGyanis::Core::EventLoop                  &m_loop;                ///< 所属事件循环
        Endpoint                                      m_endpoint;            ///< 连接参数
        std::unique_ptr<AsynGyanis::Core::TlsContext> m_tls;                 ///< wss 使用的客户端 TLS 上下文
        std::string                                   m_initializationError; ///< 装配失败原因，空表示可用
        std::chrono::milliseconds                     m_handshakeTimeout;    ///< 握手与 hello 应答的时限
    };
} // namespace EdgePulse::Agent
