#include "Core/Http/RealtimeBus.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Base/Security/RandomToken.h"
#include "Core/Http/WebSocketEndpoints.h"
#include "Core/Http/RealtimeTopics.h"
#include "Core/Http/SseBroadcaster.h"
#include "Database/Redis/RedisConnection.h"
#include "Net/WebSocket/WebSocketHub.h"

#include <Core/EventLoop/EventLoop.h>

#include <array>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Web
{
    namespace
    {
        [[nodiscard]] AsynGyanis::Database::ConnectionConfig redisConfig(const RealtimeBusOptions &options)
        {
            AsynGyanis::Database::ConnectionConfig configuration = AsynGyanis::Database::ConnectionConfig::redisDefault();
            configuration.host                                   = options.host;
            configuration.port                                   = options.port;
            configuration.password                               = options.password;
            configuration.database                               = std::to_string(options.database);
            return configuration;
        }

        [[nodiscard]] std::string envelopeOf(const std::string_view instanceId, const std::string_view base, const std::int64_t tenantId, const std::string_view payload)
        {
            return AsynGyanis::Base::ConfigValue::object(
                           {{"instance", instanceId}, {"target", "browser"}, {"base", base}, {"tenantId", tenantId}, {"nodeId", 0}, {"commandId", 0}, {"payload", payload}})
                    .dump();
        }

        /**
         * @brief 生成 Agent 消息的跨实例信封
         * @param instanceId 发布实例标识
         * @param message 目标节点、命令正文或关闭指令
         * @return std::string 带 target=agent 的信封
         */
        [[nodiscard]] std::string agentEnvelopeOf(const std::string_view instanceId, const AgentMessage &message)
        {
            return AsynGyanis::Base::ConfigValue::object({{"instance", instanceId},
                                                          {"target", "agent"},
                                                          {"base", ""},
                                                          {"tenantId", 0},
                                                          {"nodeId", message.nodeId},
                                                          {"commandId", message.commandId},
                                                          {"close", message.close},
                                                          {"closeCode", message.closeCode},
                                                          {"closeReason", message.closeReason},
                                                          {"payload", message.payload}})
                    .dump();
        }

        /**
         * @brief 日志用的 Redis 端点
         * @param options 总线选项
         * @return std::string host:port；不含口令
         */
        [[nodiscard]] std::string endpointOf(const RealtimeBusOptions &options)
        {
            return options.host + ":" + std::to_string(options.port);
        }

        /// 发布连接重连的冷却窗口；健康检查本身由订阅线程按读推送周期触发
        constexpr std::chrono::seconds kPublisherProbeInterval{5};

        /// 关闭消息在同一连接上的最多发布次数：关闭丢失会让被吊销的 Agent 继续持有长连接
        constexpr int kClosePublishAttempts = 3;

        /**
         * @brief 从选项解析实例标识；空值退回随机值，不把空串写进信封
         * @param options 已构造完成的总线选项
         * @return std::string 非空实例标识
         */
        [[nodiscard]] std::string resolveInstanceId(const RealtimeBusOptions &options)
        {
            return options.instanceId.empty() ? Common::randomTokenHex(8) : options.instanceId;
        }

        /**
         * @brief 发布连接的生产实现：包住一条 Redis 连接
         * @details PUBLISH 走 argv 接口，信封里的空白、引号与内嵌 NUL 都按字节送出，
         *          不需要先拼成一行命令再切词
         */
        class RedisPublisherTransport final : public PublisherTransport
        {
        public:
            /**
             * @brief 绑定连接配置与收发超时；此阶段不发生网络交互
             * @param configuration Redis 连接配置
             * @param timeoutMilliseconds 连接与单条命令上限；发布在事件循环上执行，必须保持有限
             */
            RedisPublisherTransport(const AsynGyanis::Database::ConnectionConfig &configuration, const int timeoutMilliseconds) : m_connection(configuration)
            {
                // 建连与 PUBLISH 都可能在服务器半开时阻塞；发布路径不承接默认三十秒的命令超时
                m_connection.setConnectTimeout(timeoutMilliseconds);
                m_connection.setQueryTimeout(timeoutMilliseconds);
            }

            bool connect() override
            {
                return m_connection.connect();
            }

            void disconnect() override
            {
                m_connection.disconnect();
            }

            [[nodiscard]] bool isConnected() const override
            {
                return m_connection.isConnected();
            }

            [[nodiscard]] bool probeAlive() noexcept override
            {
                return m_connection.probeAlive();
            }

            [[nodiscard]] bool publish(const std::string_view channel, const std::string_view envelope) override
            {
                const std::vector<std::string_view> command{"PUBLISH", channel, envelope};
                return m_connection.executeCommand(command) != nullptr;
            }

            [[nodiscard]] std::string lastError() const override
            {
                return m_connection.lastError();
            }

            [[nodiscard]] int commandTimeoutMilliseconds() const noexcept override
            {
                return m_connection.queryTimeout();
            }

        private:
            AsynGyanis::Database::RedisConnection m_connection; ///< 事件发布专用连接
        };
    } // namespace

    std::unique_ptr<PublisherTransport> makeRedisPublisherTransport(const RealtimeBusOptions &options)
    {
        return std::make_unique<RedisPublisherTransport>(redisConfig(options), kPublisherCommandTimeoutMilliseconds);
    }

    std::string makeRealtimeEnvelope(const std::string_view instanceId, const std::string_view base, const std::int64_t tenantId, const std::string_view payload)
    {
        return envelopeOf(instanceId, base, tenantId, payload);
    }

    RealtimeBus::RealtimeBus(AsynGyanis::Core::EventLoop &loop, BrowserWebSocket &webSocket, SseBroadcaster &sse, RealtimeBusOptions options) :
        m_loop(loop), m_webSocket(webSocket), m_sse(sse), m_options(std::move(options)), m_instanceId(resolveInstanceId(m_options))
    {
        // 构造只建对象、不连网；start() 才建立发布连接。启用时先建好对象，让健康标记与连接在
        // 发布路径眼里始终成对存在（未连接就是未健康），也便于测试在 start 之外观察跳过行为
        if (m_options.enabled)
        {
            m_publisher = createPublisher();
        }
    }

    RealtimeBus::~RealtimeBus()
    {
        m_stopping.store(true);
        if (m_thread.joinable())
        {
            m_thread.request_stop();
            m_thread.join();
        }
        std::lock_guard<std::mutex> lock(m_publisherMutex);
        if (m_publisher)
        {
            m_publisher->disconnect();
        }
        m_publisherHealth.markUnhealthy();
        m_subscriberConnected.store(false);
    }

    void RealtimeBus::start()
    {
        if (!m_options.enabled)
        {
            return;
        }
        {
            std::lock_guard<std::mutex> publisherLock(m_publisherMutex);
            if (!m_publisher)
            {
                m_publisher = createPublisher();
            }
            if (!m_publisher || !m_publisher->connect())
            {
                m_publisherHealth.markUnhealthy();
                LOG_WARN_FMT("实时事件发布连接不可用：instance={} endpoint={} error={}", m_instanceId, endpointOf(m_options),
                             m_publisher ? m_publisher->lastError() : std::string{"发布连接构造失败"});
            } else
            {
                m_publisherHealth.markHealthy();
            }
        }
        LOG_INFO_FMT("实时事件跨实例转发已启用：instance={} channel={}", m_instanceId, m_options.channel);
        if (m_options.startSubscriber)
        {
            m_thread = std::jthread([this](const std::stop_token token) { subscribeLoop(token); });
        }
    }

    bool RealtimeBus::isHealthy() const noexcept
    {
        if (!m_options.enabled)
        {
            return true;
        }
        return m_publisherHealth.isHealthy() && m_subscriberConnected.load(std::memory_order_acquire);
    }

    std::string_view RealtimeBus::instanceId() const noexcept
    {
        return m_instanceId;
    }

    AsynGyanis::Core::Task<> RealtimeBus::publish(const std::string_view base, const std::int64_t tenantId, const std::string &payload)
    {
        co_await deliverLocal(base, tenantId, payload);
        if (!m_options.enabled)
        {
            co_return;
        }
        // 事件循环上只读一次原子健康标记。未健康时直接跳过：重连与 PING 只由订阅线程在后台做
        if (!m_publisherHealth.isHealthy())
        {
            co_return;
        }
        // 探针正在重连或 PING 时不等锁：拿到就发，拿不到就跳过本轮跨实例转发。
        // 跳过不是发布失败，因此不改健康标记——否则会把后台刚恢复成功的 true 覆盖成旧的 false
        std::unique_lock<std::mutex> publisherLock(m_publisherMutex, std::try_to_lock);
        if (!publisherLock.owns_lock() || !m_publisher)
        {
            co_return;
        }
        if (m_publisher->publish(m_options.channel, makeRealtimeEnvelope(m_instanceId, base, tenantId, payload)))
        {
            co_return;
        }
        // 真实尝试失败才标不健康；本轮不重发，避免连接其实已送达时重复扇出
        m_publisherHealth.markUnhealthy();
        LOG_WARN_FMT("实时事件发布到 Redis 失败：instance={} endpoint={} error={}", m_instanceId, endpointOf(m_options), m_publisher->lastError());
        co_return;
    }

    AsynGyanis::Core::Task<> RealtimeBus::publishAgent(AsynGyanis::Core::EventLoop &loop, AgentMessage message)
    {
        // 本地 handler 会就地访问 Agent 连接，必须与调用方给出的循环同属一条线程；这里用形参核对
        // 当前协程所在循环，错了就当场抛出，不能把连接操作留在别的线程上静默跑完
        if (AsynGyanis::Core::EventLoop::currentEventLoop() != &loop)
        {
            LOG_ERROR_FMT("publishAgent 不在调用方给出的循环上：node={} command={}", message.nodeId, message.commandId);
            throw std::logic_error("publishAgent 必须在调用方给出的循环上调用");
        }
        try
        {
            co_await deliverAgent(message);
        } catch (const std::exception &failure)
        {
            // 本地没有连接或写失败不能拖住跨实例转发：真正持有该节点连接的可能是别的实例
            LOG_WARN_FMT("Agent 消息本地投递失败：node={} error={}", message.nodeId, failure.what());
        }
        if (!m_options.enabled)
        {
            co_return;
        }
        if (!m_publisherHealth.isHealthy())
        {
            co_return;
        }
        std::unique_lock<std::mutex> publisherLock(m_publisherMutex, std::try_to_lock);
        if (!publisherLock.owns_lock() || !m_publisher)
        {
            co_return;
        }
        const std::string envelope = agentEnvelopeOf(m_instanceId, message);
        // 关闭消息丢失的代价是被吊销的 Agent 继续持有长连接，因此在同一条连接上多试两次；
        // 命令消息有 Agent 轮询兜底，重复发布没有收益，仍然只试一次
        const int attempts = message.close ? kClosePublishAttempts : 1;
        for (int attempt = 0; attempt < attempts; ++attempt)
        {
            if (m_publisher->publish(m_options.channel, envelope))
            {
                co_return;
            }
            if (attempt + 1 < attempts)
            {
                LOG_WARN_FMT("Agent 关闭消息发布失败，准备重试：instance={} node={} attempt={} error={}", m_instanceId, message.nodeId, attempt + 1, m_publisher->lastError());
            }
        }
        m_publisherHealth.markUnhealthy();
        LOG_WARN_FMT("Agent 消息发布到 Redis 失败：instance={} endpoint={} node={} error={}", m_instanceId, endpointOf(m_options), message.nodeId, m_publisher->lastError());
        co_return;
    }

    void RealtimeBus::setAgentMessageHandler(std::function<AsynGyanis::Core::Task<>(const AgentMessage &)> handler)
    {
        std::lock_guard<std::mutex> lock(m_agentHandlerMutex);
        m_agentHandler = std::move(handler);
    }

    AsynGyanis::Core::Task<> RealtimeBus::deliverAgent(const AgentMessage &message)
    {
        std::function<AsynGyanis::Core::Task<>(const AgentMessage &)> handler;
        {
            std::lock_guard<std::mutex> lock(m_agentHandlerMutex);
            handler = m_agentHandler;
        }
        if (handler)
        {
            co_await handler(message);
        }
        co_return;
    }

    AsynGyanis::Core::Task<> RealtimeBus::deliverLocal(const std::string_view base, const std::int64_t tenantId, const std::string_view payload)
    {
        if (m_options.localDelivery)
        {
            co_await m_options.localDelivery(base, tenantId, payload);
            co_return;
        }
        co_await m_webSocket.hub().publish(base, payload);
        co_await m_webSocket.hub().publish(tenantTopic(base, tenantId), payload);
        if (base == "alarms")
        {
            m_sse.publish(std::string{payload}, tenantId);
        }
        co_return;
    }

    void RealtimeBus::enqueueRemote(std::string base, const std::int64_t tenantId, std::string payload)
    {
        m_remoteQueue.push_back(RemoteEvent{false, std::move(base), tenantId, std::move(payload), AgentMessage{}});
        if (m_remoteRunning)
        {
            return;
        }
        m_remoteRunning = true;
        m_remoteWorker  = drainRemote();
        m_loop.scheduler().schedule(m_remoteWorker->handle());
    }

    void RealtimeBus::enqueueRemoteAgent(AgentMessage message)
    {
        m_remoteQueue.push_back(RemoteEvent{true, {}, 0, {}, std::move(message)});
        if (m_remoteRunning)
        {
            return;
        }
        m_remoteRunning = true;
        m_remoteWorker  = drainRemote();
        m_loop.scheduler().schedule(m_remoteWorker->handle());
    }

    AsynGyanis::Core::Task<> RealtimeBus::drainRemote()
    {
        while (!m_remoteQueue.empty())
        {
            RemoteEvent event = std::move(m_remoteQueue.front());
            m_remoteQueue.pop_front();
            try
            {
                if (event.agent)
                {
                    co_await deliverAgent(event.message);
                } else
                {
                    co_await deliverLocal(event.base, event.tenantId, event.payload);
                }
            } catch (const std::exception &failure)
            {
                LOG_WARN_FMT("远端实时事件本地投递失败：{}", failure.what());
            }
        }
        m_remoteRunning = false;
        co_return;
    }

    std::unique_ptr<PublisherTransport> RealtimeBus::createPublisher()
    {
        if (m_options.publisherFactory)
        {
            return m_options.publisherFactory();
        }
        return makeRedisPublisherTransport(m_options);
    }

    bool RealtimeBus::reconnectPublisherLocked()
    {
        // 重连可能阻塞到连接超时，只允许订阅线程在锁内调用；冷却窗口避免 Redis 长时间不可用时
        // 每五秒都重新建连
        const auto now = std::chrono::steady_clock::now();
        if (now < m_nextPublisherAttempt)
        {
            return false;
        }
        m_nextPublisherAttempt = now + kPublisherProbeInterval;
        if (!m_publisher)
        {
            m_publisher = createPublisher();
        }
        if (!m_publisher)
        {
            LOG_WARN_FMT("实时事件发布连接构造失败：instance={} endpoint={}", m_instanceId, endpointOf(m_options));
            return false;
        }
        if (!m_publisher->isConnected() && !m_publisher->connect())
        {
            LOG_WARN_FMT("实时事件发布连接重连失败：instance={} endpoint={} error={}", m_instanceId, endpointOf(m_options), m_publisher->lastError());
            return false;
        }
        if (!m_publisher->probeAlive())
        {
            LOG_WARN_FMT("实时事件发布连接探针失败，尝试重连：instance={} endpoint={} error={}", m_instanceId, endpointOf(m_options), m_publisher->lastError());
            // PING 失败通常意味着服务端已丢弃这条连接，isConnected() 看不出来；与缓存探针同一口径，
            // 断开后立刻重连一次再 PING，整个序列仍只占一个冷却窗口
            m_publisher->disconnect();
            if (!m_publisher->connect())
            {
                LOG_WARN_FMT("实时事件发布连接重连失败：instance={} endpoint={} error={}", m_instanceId, endpointOf(m_options), m_publisher->lastError());
                return false;
            }
            if (!m_publisher->probeAlive())
            {
                LOG_WARN_FMT("实时事件发布连接重连后探针仍失败：instance={} endpoint={} error={}", m_instanceId, endpointOf(m_options), m_publisher->lastError());
                m_publisher->disconnect();
                return false;
            }
        }
        LOG_INFO_FMT("实时事件发布连接已恢复：instance={} endpoint={}", m_instanceId, endpointOf(m_options));
        return true;
    }

    void RealtimeBus::probePublisher()
    {
        // 快路径不取锁：健康时周期检查不与发布路径争同一把锁
        if (m_publisherHealth.isHealthy())
        {
            return;
        }
        std::lock_guard<std::mutex> publisherLock(m_publisherMutex);
        // 拿到锁后复查并在锁内写回：恢复结果与连接状态同属一个临界区，发布路径的失败标记
        // 不会覆盖比它更新的恢复结果
        static_cast<void>(m_publisherHealth.probe([this] { return reconnectPublisherLocked(); }));
    }

    void RealtimeBus::subscribeLoop(const std::stop_token stopToken)
    {
        AsynGyanis::Database::RedisConnection connection(redisConfig(m_options));
        while (!stopToken.stop_requested() && !m_stopping.load())
        {
            // 健康时这里只是一次原子读；不健康时每轮都尝试恢复，真正的重连仍受 5 秒冷却限制。
            // 发布路径不再顺手重连，因此恢复延迟由这里的检查周期决定，最多约 500 毫秒
            probePublisher();
            if (!connection.isConnected() && !connection.connect())
            {
                m_subscriberConnected.store(false);
                LOG_WARN_FMT("实时事件订阅连接失败：instance={} endpoint={} error={}", m_instanceId, endpointOf(m_options), connection.lastError());
                std::this_thread::sleep_for(std::chrono::seconds{1});
                continue;
            }
            const std::array<std::string_view, 1> channels{m_options.channel};
            if (!connection.subscribe(channels))
            {
                m_subscriberConnected.store(false);
                LOG_WARN_FMT("实时事件订阅失败：instance={} endpoint={} error={}", m_instanceId, endpointOf(m_options), connection.lastError());
                connection.disconnect();
                std::this_thread::sleep_for(std::chrono::seconds{1});
                continue;
            }
            m_subscriberConnected.store(true);
            while (!stopToken.stop_requested() && !m_stopping.load())
            {
                const auto reply = connection.readPushReply(std::chrono::milliseconds{500});
                // 订阅连接空闲时也检查发布连接；读推送最多等 500 毫秒，检查不会被长期推后
                probePublisher();
                if (!reply.has_value())
                {
                    if (!connection.lastError().empty())
                    {
                        m_subscriberConnected.store(false);
                        connection.disconnect();
                        break;
                    }
                    continue;
                }
                if (reply->kind != "message" || reply->channel != m_options.channel)
                {
                    continue;
                }
                const auto value = AsynGyanis::Base::parseConfigValue(reply->payload);
                if (!value.has_value() || !value->is_object())
                {
                    continue;
                }
                try
                {
                    // instance 也必须放在 try 内：对端把字段写成数字或 null 时 value() 会抛类型异常，
                    // 放在外面会让异常穿出订阅线程并终止整个进程
                    const std::string instance = value->value("instance", std::string{});
                    if (instance == m_instanceId)
                    {
                        continue;
                    }
                    const std::string target = value->value("target", std::string{"browser"});
                    if (target == "agent")
                    {
                        AgentMessage message;
                        message.nodeId      = value->value("nodeId", std::int64_t{0});
                        message.commandId   = value->value("commandId", std::int64_t{0});
                        message.payload     = value->value("payload", std::string{});
                        message.close       = value->value("close", false);
                        message.closeCode   = value->value("closeCode", 1008);
                        message.closeReason = value->value("closeReason", std::string{});
                        if (message.nodeId <= 0 || (!message.close && message.payload.empty()))
                        {
                            continue;
                        }
                        // 只投回本循环的本地 handler，不再二次发布：否则两个实例会互相回声
                        m_loop.scheduler().postRemote(
                                [this, message = std::move(message)]
                                {
                                    if (!m_stopping.load())
                                    {
                                        enqueueRemoteAgent(std::move(message));
                                    }
                                });
                        continue;
                    }
                    if (target != "browser")
                    {
                        continue;
                    }
                    const std::string  base     = value->value("base", std::string{});
                    const std::int64_t tenantId = value->value("tenantId", std::int64_t{0});
                    const std::string  payload  = value->value("payload", std::string{});
                    if (base.empty() || payload.empty())
                    {
                        continue;
                    }
                    m_loop.scheduler().postRemote(
                            [this, base, tenantId, payload]
                            {
                                if (!m_stopping.load())
                                {
                                    enqueueRemote(base, tenantId, payload);
                                }
                            });
                } catch (const std::exception &failure)
                {
                    // 信封字段类型不对属于对端或版本问题：丢弃这一条，不能带走订阅线程
                    LOG_WARN_FMT("远端实时事件信封解析失败：{}", failure.what());
                }
            }
        }
        m_subscriberConnected.store(false);
        connection.disconnect();
    }
} // namespace EdgePulse::Web
