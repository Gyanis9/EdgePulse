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
     * @details 轮询在启动时立即运行，只有通道通过 hello 确认可用后才暂停；任何断开都恢复轮询。
     *          连续失败次数只用于重连退避与日志，不参与是否轮询的判定。这样在 hello 与通道
     *          登记之间的空窗里创建的命令不会滞留到下一次连接失败。
     * @note 只在事件循环线程上使用，内部不做同步。
     */
    class WebSocketFallbackState
    {
    public:
        /**
         * @brief 构造降级状态；初始未连接，轮询因此是运行的
         */
        WebSocketFallbackState() = default;

        /**
         * @brief 记录通道通过 hello 确认可用
         * @details 暂停轮询并清零失败计数
         */
        void recordConnected();

        /**
         * @brief 记录通道断开
         * @details 不清零失败计数；轮询立即恢复
         */
        void recordDisconnected();

        /**
         * @brief 记录一次连接失败
         * @details 只累计失败次数供重连退避使用；轮询在断开的任意时刻都运行
         */
        void recordConnectFailure();

        /**
         * @brief 轮询是否应当运行
         * @return 通道当前未连接时返回 true；只有 hello 已确认的连接会暂停轮询
         */
        [[nodiscard]] bool shouldPoll() const;

        /**
         * @brief 当前连续连接失败次数
         * @return int 连续失败次数
         */
        [[nodiscard]] int consecutiveFailures() const;

    private:
        bool m_connected{false};       ///< 通道是否已通过 hello 确认
        int  m_consecutiveFailures{0}; ///< 连续连接失败次数
    };

    /**
     * @brief 按配置建立 wss 或 ws 命令通道，完成 hello 并分发服务端推送
     * @details 地址从 base_url 派生：https 换成 wss、http 换成 ws，路径固定为 /api/agent/ws。
     *          连接、hello 与每 30 秒一次的文本 ping 都在本类处理，命令交给 CommandDispatcher。
     *          文本 ping 用于避免服务端的空闲关闭；服务端回 pong 或主动发 ping 时分别忽略与应答。
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

        /**
         * @brief 用磁盘上的新证书替换 wss 客户端身份
         * @return 明文通道没有 TLS 身份，返回 true；wss 下返回 TlsContext::reloadCertificate() 的结论
         * @details 新一代 SSL_CTX 与 createSSL 互斥，已建立的连接仍绑在旧上下文上继续收命令（服务端在重叠窗口内
         *          同时接受新旧证书），下一次 serve() 重连自动使用新证书。装载失败时旧上下文原样继续服务。
         * @note 与 serve() 一样只在所属事件循环线程上调用
         */
        bool reloadIdentityFromDisk();

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
