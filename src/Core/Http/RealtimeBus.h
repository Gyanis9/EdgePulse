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
#include "Core/Http/PublisherHealth.h"

#include <atomic>
#include <chrono>
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

namespace EdgePulse::Web
{
    class BrowserWebSocket;
    class SseBroadcaster;

    /**
     * @brief 跨实例发布连接的抽象口
     * @details 生产实现包住 Redis 连接；测试注入替身以观察重连、PING 与 PUBLISH。
     *          连接与探针只在订阅线程持有发布连接锁时调用；publish() 由事件循环在
     *          拿到同一把锁后调用。
     */
    class PublisherTransport
    {
    public:
        PublisherTransport() = default;

        virtual ~PublisherTransport() = default;

        PublisherTransport(const PublisherTransport &)            = delete;
        PublisherTransport &operator=(const PublisherTransport &) = delete;

        /**
         * @brief 建立连接
         * @return bool 已连接为 true；失败原因见 lastError()
         */
        [[nodiscard]] virtual bool connect() = 0;

        /**
         * @brief 断开连接
         */
        virtual void disconnect() = 0;

        /**
         * @brief 本端是否仍标记为已连接
         * @return bool 只读连接标志，不主动探测
         */
        [[nodiscard]] virtual bool isConnected() const = 0;

        /**
         * @brief 发一条 PING 并读回回复
         * @return bool 收到回复为 true
         */
        [[nodiscard]] virtual bool probeAlive() noexcept = 0;

        /**
         * @brief 把一条事件发到跨实例频道
         * @param channel 频道名
         * @param envelope 已序列化的事件信封
         * @return bool 服务端接受了 PUBLISH
         */
        [[nodiscard]] virtual bool publish(std::string_view channel, std::string_view envelope) = 0;

        /**
         * @brief 最近一次失败原因
         * @return std::string 面向日志的错误文本；不含凭据
         */
        [[nodiscard]] virtual std::string lastError() const = 0;

        /**
         * @brief 该实现声明的单条命令超时
         * @return int 毫秒；0 表示实现没有单独的有限超时
         * @note 只用于启动与用例观察「发布路径是否被限时」，不参与收发本身
         */
        [[nodiscard]] virtual int commandTimeoutMilliseconds() const noexcept
        {
            return 0;
        }
    };

    /// 发布连接的单条命令上限：半开连接最多卡住事件循环两秒，而不是连接默认的三十秒
    inline constexpr int kPublisherCommandTimeoutMilliseconds = 2000;

    /**
     * @brief 生成跨实例信封
     * @param instanceId 发布实例标识
     * @param base 主题基名
     * @param tenantId 归属租户
     * @param payload 已序列化的 JSON 文本
     * @return std::string 形如 {"instance":...,"target":"browser","base":...,"tenantId":...,"nodeId":0,"commandId":0,"payload":...} 的 JSON
     */
    [[nodiscard]] std::string makeRealtimeEnvelope(std::string_view instanceId, std::string_view base, std::int64_t tenantId, std::string_view payload);

    /**
     * @brief 发给某台 Agent 的命令或关闭指令
     */
    struct AgentMessage
    {
        std::int64_t nodeId{0};       ///< 目标节点
        std::string  payload;         ///< 已序列化的命令 JSON；关闭消息为空
        std::int64_t commandId{0};    ///< 关联命令标识；0 表示不关联数据库行
        bool         close{false};    ///< 是否关闭目标连接
        int          closeCode{1008}; ///< 关闭状态码，默认策略违规
        std::string  closeReason;     ///< 关闭原因
    };

    /**
     * @brief Redis 转发参数
     */
    struct RealtimeBusOptions
    {
        bool          enabled{false};              ///< 是否启用跨实例转发
        std::string   instanceId;                  ///< 本实例标识；空时由总线生成随机值，HA 部署应显式配置
        std::string   host{"127.0.0.1"};           ///< Redis 主机
        std::uint16_t port{6379};                  ///< Redis 端口
        std::string   password;                    ///< Redis 口令
        int           database{0};                 ///< 键空间
        std::string   channel{"edgepulse:events"}; ///< 频道名

        /// 本地投递覆盖口：为空时走 WebSocket/SSE；测试用它观察跨实例送达
        std::function<AsynGyanis::Core::Task<>(std::string_view, std::int64_t, std::string_view)> localDelivery;

        /// 发布连接构造口：为空时构造真实 Redis 连接；测试注入替身以观察重连与发布
        std::function<std::unique_ptr<PublisherTransport>()> publisherFactory;

        /// 是否启动跨实例订阅线程；测试用 false 只验证发布路径，生产保持默认
        bool startSubscriber{true};
    };

    /**
     * @brief 构造真实 Redis 发布连接
     * @param options Redis 参数
     * @return std::unique_ptr<PublisherTransport> 尚未连接、命令超时已收紧到 kPublisherCommandTimeoutMilliseconds 的发布连接
     * @note 构造不发起网络交互；连接与超时应用发生在 connect()。公开它是为了让用例能核对发布路径的有限超时
     */
    [[nodiscard]] std::unique_ptr<PublisherTransport> makeRedisPublisherTransport(const RealtimeBusOptions &options);

    /**
     * @brief 实时事件总线
     * @details publish() 先在本进程扇出（WebSocket 与告警 SSE），再把同一事件以带实例标识的信封发到 Redis；
     *          publishAgent() 走同一条频道，但只投给本实例登记的 Agent 投递口，不进入浏览器扇出。
     *          订阅线程按信封的 target 字段分流：agent 投回本循环的 Agent handler，browser（含缺省
     *          target 的旧信封）走本地浏览器扇出；两条路径都不再二次发布。
     * @note publish() 与 publishAgent() 都在服务器循环上调用，只做本地投递与不等待的 PUBLISH 尝试，
     *       不重连也不 PING；发布连接的恢复只由订阅线程在后台完成
     * @note 跨实例转发是尽力而为：Redis 不可用或发布锁被占住时本轮远端事件直接丢弃，本地投递不受影响；
     *       恢复后不补发历史事件，依赖远端副本书写的调用方必须以本地数据库与 Agent 轮询兜底
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
         * @brief 跨实例转发是否可用
         * @return bool 未启用 Redis 时恒为 true；启用时要求发布连接与订阅连接均处于可用状态
         * @note 只读原子快照，可在任意线程调用；发布连接失效后由订阅线程在锁内重连并 PING，
         *       重连冷却 5 秒，恢复后无需重启进程。发布路径本身不会改动健康标记，也不会触发恢复
         */
        [[nodiscard]] bool isHealthy() const noexcept;

        /**
         * @brief 本实例标识
         * @return std::string_view 配置值；配置为空时为构造期生成的随机值
         */
        [[nodiscard]] std::string_view instanceId() const noexcept;

        /**
         * @brief 发布一条事件到本进程与其它实例
         * @param base 主题基名：nodes / alarms / commands
         * @param tenantId 归属租户
         * @param payload 已序列化的 JSON 文本
         * @return AsynGyanis::Core::Task<> 本地扇出完成的协程
         * @note 跨实例转发只在发布连接已标为健康且发布锁可立即取得时尝试；不健康或锁被订阅线程
         *       占住时跳过本轮，不等待、不重连、不 PING，也不改动健康标记
         */
        AsynGyanis::Core::Task<> publish(std::string_view base, std::int64_t tenantId, const std::string &payload);

        /**
         * @brief 登记 Agent 消息的本地投递口
         * @param handler 收到本实例或远端 Agent 消息时调用；空表示丢弃
         * @note 置空或替换可在任意线程调用；handler 本身只在服务器循环上执行
         */
        void setAgentMessageHandler(std::function<AsynGyanis::Core::Task<>(const AgentMessage &)> handler);

        /**
         * @brief 把一条 Agent 消息投给本地连接并转发给其它实例
         * @param loop 调用方所在的循环；本地 handler 在该循环上就地执行
         * @param message 目标节点、命令正文或关闭指令
         * @return AsynGyanis::Core::Task<> 本地投递与 PUBLISH 尝试完成即返回
         * @throws std::logic_error 当前协程不在 loop 指定的循环上：本地 handler 会就地写出
         *         Agent 连接，跨线程执行会把连接访问放到错误的线程
         * @note 本地 handler 先执行；本地失败不影响跨实例转发。远端消息由总线订阅线程投回
         *       总线自身的服务器循环。未启用 Redis 时只做本地投递，没有本地连接时是空操作，
         *       命令行保持待执行。关闭消息的 PUBLISH 失败时最多重试三次（同一条连接上立即重试），
         *       命令消息只尝试一次。
         */
        AsynGyanis::Core::Task<> publishAgent(AsynGyanis::Core::EventLoop &loop, AgentMessage message);

    private:
        /**
         * @brief 只在本进程扇出
         */
        AsynGyanis::Core::Task<> deliverLocal(std::string_view base, std::int64_t tenantId, std::string_view payload);

        /**
         * @brief 只在本进程把 Agent 消息交给登记的处理口
         */
        AsynGyanis::Core::Task<> deliverAgent(const AgentMessage &message);

        /**
         * @brief 把远端事件排进本地队列并按需启动投递协程
         */
        void enqueueRemote(std::string base, std::int64_t tenantId, std::string payload);

        /**
         * @brief 把远端 Agent 消息排进本地队列并按需启动投递协程
         */
        void enqueueRemoteAgent(AgentMessage message);

        /**
         * @brief 投递远端事件队列
         */
        AsynGyanis::Core::Task<> drainRemote();

        /**
         * @brief 订阅线程主体：断线重连、读推送、投回循环
         */
        void subscribeLoop(std::stop_token stopToken);

        /**
         * @brief 按配置或注入的工厂构造发布连接
         * @return std::unique_ptr<PublisherTransport> 发布连接；工厂返回空时为空
         */
        [[nodiscard]] std::unique_ptr<PublisherTransport> createPublisher();

        /**
         * @brief 订阅线程到点后按需恢复发布连接
         * @note 只在发布连接不健康时持锁调用注入探针；恢复结果在锁内写回健康标记
         */
        void probePublisher();

        /**
         * @brief 在已持有发布连接锁时断开并重连，再 PING
         * @return bool 恢复成功为 true
         */
        bool reconnectPublisherLocked();

        AsynGyanis::Core::EventLoop          &m_loop;                       ///< 服务器循环
        BrowserWebSocket                     &m_webSocket;                  ///< 浏览器扇出
        SseBroadcaster                       &m_sse;                        ///< 告警 SSE
        RealtimeBusOptions                    m_options;                    ///< Redis 参数
        std::string                           m_instanceId;                 ///< 本实例标识
        std::jthread                          m_thread;                     ///< 订阅线程
        std::unique_ptr<PublisherTransport>   m_publisher;                  ///< 发布连接，只在发布连接锁内使用
        std::mutex                            m_publisherMutex;             ///< 保护发布连接
        PublisherHealth                       m_publisherHealth;            ///< 发布连接健康状态与按需探针
        std::chrono::steady_clock::time_point m_nextPublisherAttempt{};     ///< 下一次允许重连的时刻，和发布连接同锁
        std::atomic<bool>                     m_subscriberConnected{false}; ///< 订阅连接是否已订阅频道
        std::atomic<bool>                     m_stopping{false};            ///< 析构开始标记

        struct RemoteEvent
        {
            bool         agent{false}; ///< 是否为 Agent 消息
            std::string  base;         ///< 浏览器主题基名
            std::int64_t tenantId{0};  ///< 浏览器事件归属租户
            std::string  payload;      ///< 浏览器事件正文
            AgentMessage message;      ///< Agent 消息
        };

        std::deque<RemoteEvent>                 m_remoteQueue;          ///< 待本地投递的远端事件
        std::optional<AsynGyanis::Core::Task<>> m_remoteWorker;         ///< 投递协程帧
        bool                                    m_remoteRunning{false}; ///< 是否已有投递协程

        std::mutex                                                    m_agentHandlerMutex; ///< 保护 Agent 投递口
        std::function<AsynGyanis::Core::Task<>(const AgentMessage &)> m_agentHandler;      ///< Agent 本地投递口
    };
} // namespace EdgePulse::Web
