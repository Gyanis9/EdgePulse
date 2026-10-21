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
    } // namespace

    RealtimeBus::RealtimeBus(AsynGyanis::Core::EventLoop &loop, BrowserWebSocket &webSocket, SseBroadcaster &sse, RealtimeBusOptions options) :
        m_loop(loop), m_webSocket(webSocket), m_sse(sse), m_options(std::move(options)), m_instanceId(Common::randomTokenHex(8))
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
                LOG_WARN_FMT("实时事件跨实例转发不可用：{}", m_publisher->lastError());
                m_publisher.reset();
            }
        }
        m_thread = std::jthread([this](const std::stop_token token) { subscribeLoop(token); });
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
            co_return;
        }
        const std::string                   envelope = envelopeOf(m_instanceId, base, tenantId, payload);
        const std::vector<std::string_view> command{"PUBLISH", m_options.channel, envelope};
        if (!m_publisher->executeCommand(command))
        {
            LOG_WARN_FMT("实时事件发布到 Redis 失败：{}", m_publisher->lastError());
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

    void RealtimeBus::subscribeLoop(const std::stop_token stopToken)
    {
        AsynGyanis::Database::RedisConnection connection(redisConfig(m_options));
        while (!stopToken.stop_requested() && !m_stopping.load())
        {
            if (!connection.isConnected() && !connection.connect())
            {
                LOG_WARN_FMT("实时事件订阅连接失败：{}", connection.lastError());
                std::this_thread::sleep_for(std::chrono::seconds{1});
                continue;
            }
            const std::array<std::string_view, 1> channels{m_options.channel};
            if (!connection.subscribe(channels))
            {
                LOG_WARN_FMT("实时事件订阅失败：{}", connection.lastError());
                connection.disconnect();
                std::this_thread::sleep_for(std::chrono::seconds{1});
                continue;
            }
            while (!stopToken.stop_requested() && !m_stopping.load())
            {
                const auto reply = connection.readPushReply(std::chrono::milliseconds{500});
                if (!reply.has_value())
                {
                    if (!connection.lastError().empty())
                    {
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
        connection.disconnect();
    }
} // namespace EdgePulse::Web
