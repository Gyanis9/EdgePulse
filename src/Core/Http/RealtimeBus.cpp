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
            return AsynGyanis::Base::ConfigValue::object({{"instance", instanceId}, {"base", base}, {"tenantId", tenantId}, {"payload", payload}}).dump();
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

        /// 发布连接失效后的恢复检查周期
        constexpr std::chrono::seconds kPublisherProbeInterval{5};

        /**
         * @brief 从选项解析实例标识；空值退回随机值，不把空串写进信封
         * @param options 已构造完成的总线选项
         * @return std::string 非空实例标识
         */
        [[nodiscard]] std::string resolveInstanceId(const RealtimeBusOptions &options)
        {
            return options.instanceId.empty() ? Common::randomTokenHex(8) : options.instanceId;
        }
    } // namespace

    std::string makeRealtimeEnvelope(const std::string_view instanceId, const std::string_view base, const std::int64_t tenantId, const std::string_view payload)
    {
        return envelopeOf(instanceId, base, tenantId, payload);
    }

    RealtimeBus::RealtimeBus(AsynGyanis::Core::EventLoop &loop, BrowserWebSocket &webSocket, SseBroadcaster &sse, RealtimeBusOptions options) :
        m_loop(loop), m_webSocket(webSocket), m_sse(sse), m_options(std::move(options)), m_instanceId(resolveInstanceId(m_options))
    {
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
            std::lock_guard<std::mutex> lock(m_publisherMutex);
            m_publisher = std::make_unique<AsynGyanis::Database::RedisConnection>(redisConfig(m_options));
            if (!m_publisher->connect())
            {
                m_publisherHealth.markUnhealthy();
                LOG_WARN_FMT("实时事件发布连接不可用：instance={} endpoint={} error={}", m_instanceId, endpointOf(m_options), m_publisher->lastError());
                m_publisher.reset();
            } else
            {
                m_publisherHealth.markHealthy();
            }
        }
        LOG_INFO_FMT("实时事件跨实例转发已启用：instance={} channel={}", m_instanceId, m_options.channel);
        m_thread = std::jthread([this](const std::stop_token token) { subscribeLoop(token); });
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
        std::lock_guard<std::mutex> lock(m_publisherMutex);
        if (!m_publisher || !m_publisher->isConnected())
        {
            // 发布失败后的一次有界恢复：重连并 PING，不重发本轮事件，避免连接其实已送达时重复扇出
            if (!m_publisherHealth.probe([this] { return reconnectPublisherLocked(); }, true))
            {
                co_return;
            }
        }
        const std::string                   envelope = makeRealtimeEnvelope(m_instanceId, base, tenantId, payload);
        const std::vector<std::string_view> command{"PUBLISH", m_options.channel, envelope};
        if (!m_publisher->executeCommand(command))
        {
            m_publisherHealth.markUnhealthy();
            LOG_WARN_FMT("实时事件发布到 Redis 失败：instance={} endpoint={} error={}", m_instanceId, endpointOf(m_options), m_publisher->lastError());
            // 下一拍发布前把连接恢复到可用状态；本轮不再重发，避免重复投递
            static_cast<void>(m_publisherHealth.probe([this] { return reconnectPublisherLocked(); }, true));
        } else
        {
            m_publisherHealth.markHealthy();
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
        m_remoteQueue.push_back(RemoteEvent{std::move(base), tenantId, std::move(payload)});
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
                co_await deliverLocal(event.base, event.tenantId, event.payload);
            } catch (const std::exception &failure)
            {
                LOG_WARN_FMT("远端实时事件本地投递失败：{}", failure.what());
            }
        }
        m_remoteRunning = false;
        co_return;
    }

    bool RealtimeBus::reconnectPublisherLocked()
    {
        // 重连可能阻塞到连接超时；发布路径与订阅线程共用同一冷却窗口，避免 Redis 长时间不可用时
        // 每次发布都在事件循环上再试一次
        const auto now = std::chrono::steady_clock::now();
        if (now < m_nextPublisherAttempt)
        {
            return false;
        }
        m_nextPublisherAttempt = now + kPublisherProbeInterval;
        if (!m_publisher)
        {
            m_publisher = std::make_unique<AsynGyanis::Database::RedisConnection>(redisConfig(m_options));
        }
        if (!m_publisher->isConnected() && !m_publisher->connect())
        {
            LOG_WARN_FMT("实时事件发布连接重连失败：instance={} endpoint={} error={}", m_instanceId, endpointOf(m_options), m_publisher->lastError());
            return false;
        }
        if (!m_publisher->probeAlive())
        {
            LOG_WARN_FMT("实时事件发布连接探针失败：instance={} endpoint={} error={}", m_instanceId, endpointOf(m_options), m_publisher->lastError());
            m_publisher->disconnect();
            return false;
        }
        LOG_INFO_FMT("实时事件发布连接已恢复：instance={} endpoint={}", m_instanceId, endpointOf(m_options));
        return true;
    }

    bool RealtimeBus::reconnectPublisher()
    {
        std::lock_guard<std::mutex> lock(m_publisherMutex);
        return reconnectPublisherLocked();
    }

    void RealtimeBus::probePublisher()
    {
        // PublisherHealth 只在已判定不健康时调用探针；锁的作用域只覆盖一次重连与 PING
        static_cast<void>(m_publisherHealth.probe([this] { return reconnectPublisher(); }));
    }

    void RealtimeBus::subscribeLoop(const std::stop_token stopToken)
    {
        AsynGyanis::Database::RedisConnection connection(redisConfig(m_options));
        auto                                  nextPublisherProbe = std::chrono::steady_clock::now() + kPublisherProbeInterval;
        while (!stopToken.stop_requested() && !m_stopping.load())
        {
            if (std::chrono::steady_clock::now() >= nextPublisherProbe)
            {
                probePublisher();
                nextPublisherProbe = std::chrono::steady_clock::now() + kPublisherProbeInterval;
            }
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
                // 订阅连接空闲时也按周期检查发布连接；读推送最多等 500 毫秒，检查不会被长期推后
                if (std::chrono::steady_clock::now() >= nextPublisherProbe)
                {
                    probePublisher();
                    nextPublisherProbe = std::chrono::steady_clock::now() + kPublisherProbeInterval;
                }
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
                const std::string instance = value->value("instance", std::string{});
                if (instance == m_instanceId)
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
            }
        }
        m_subscriberConnected.store(false);
        connection.disconnect();
    }
} // namespace EdgePulse::Web
