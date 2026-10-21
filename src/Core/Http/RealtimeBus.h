/**
 * @file RealtimeBus.h
 * @brief 实时事件的本地扇出与 Redis 跨实例转发
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <Core/Coroutine/Task.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace AsynGyanis::Core
{
    class EventLoop;
}
namespace AsynGyanis::Database
{
    class RedisConnection;
}

namespace EdgePulse::Web
{
    class BrowserWebSocket;
    class SseBroadcaster;

    /**
     * @brief Redis 转发参数
     */
    struct RealtimeBusOptions
    {
        bool          enabled{false};              ///< 是否启用跨实例转发
        std::string   host{"127.0.0.1"};           ///< Redis 主机
        std::uint16_t port{6379};                  ///< Redis 端口
        std::string   password;                    ///< Redis 口令
        int           database{0};                 ///< 键空间
        std::string   channel{"edgepulse:events"}; ///< 频道名

        /// 本地投递覆盖口：为空时走 WebSocket/SSE；测试用它观察跨实例送达
        std::function<AsynGyanis::Core::Task<>(std::string_view, std::int64_t, std::string_view)> localDelivery;
    };

    /**
     * @brief 实时事件总线
     * @details publish() 先在本进程扇出（WebSocket 与告警 SSE），再把同一事件以带实例标识的信封发到 Redis；
     *          订阅线程收到其他实例的信封后投回本循环，只做本地扇出，不再二次发布。
     * @note publish() 在服务器循环上调用；订阅线程只持有自己的 Redis 连接
     */
    class RealtimeBus
    {
    public:
        /**
         * @brief 构造
         * @param loop 服务器循环
         * @param webSocket 浏览器 WebSocket
         * @param sse 告警 SSE
         * @param options Redis 参数
         */
        RealtimeBus(AsynGyanis::Core::EventLoop &loop, BrowserWebSocket &webSocket, SseBroadcaster &sse, RealtimeBusOptions options);

        ~RealtimeBus();

        RealtimeBus(const RealtimeBus &)            = delete;
        RealtimeBus &operator=(const RealtimeBus &) = delete;

        /**
         * @brief 启动跨实例订阅线程（未启用 Redis 时为空操作）
         */
        void start();

        /**
         * @brief 发布一条事件到本进程与其它实例
         * @param base 主题基名：nodes / alarms / commands
         * @param tenantId 归属租户
         * @param payload 已序列化的 JSON 文本
         * @return AsynGyanis::Core::Task<> 本地扇出完成的协程
         */
        AsynGyanis::Core::Task<> publish(std::string_view base, std::int64_t tenantId, const std::string &payload);

    private:
        /**
         * @brief 只在本进程扇出
         */
        AsynGyanis::Core::Task<> deliverLocal(std::string_view base, std::int64_t tenantId, std::string_view payload);

        /**
         * @brief 把远端事件排进本地队列并按需启动投递协程
         */
        void enqueueRemote(std::string base, std::int64_t tenantId, std::string payload);

        /**
         * @brief 投递远端事件队列
         */
        AsynGyanis::Core::Task<> drainRemote();

        /**
         * @brief 订阅线程主体：断线重连、读推送、投回循环
         */
        void subscribeLoop(std::stop_token stopToken);

        AsynGyanis::Core::EventLoop                           &m_loop;            ///< 服务器循环
        BrowserWebSocket                                      &m_webSocket;       ///< 浏览器扇出
        SseBroadcaster                                        &m_sse;             ///< 告警 SSE
        RealtimeBusOptions                                     m_options;         ///< Redis 参数
        std::string                                            m_instanceId;      ///< 本实例标识
        std::jthread                                           m_thread;          ///< 订阅线程
        std::unique_ptr<AsynGyanis::Database::RedisConnection> m_publisher;       ///< 发布连接，只在循环线程使用
        std::mutex                                             m_publisherMutex;  ///< 保护发布连接
        std::atomic<bool>                                      m_stopping{false}; ///< 析构开始标记

        struct RemoteEvent
        {
            std::string  base;
            std::int64_t tenantId{0};
            std::string  payload;
        };

        std::deque<RemoteEvent>                 m_remoteQueue;          ///< 待本地投递的远端事件
        std::optional<AsynGyanis::Core::Task<>> m_remoteWorker;         ///< 投递协程帧
        bool                                    m_remoteRunning{false}; ///< 是否已有投递协程
    };
} // namespace EdgePulse::Web
