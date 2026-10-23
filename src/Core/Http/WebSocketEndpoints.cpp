#include "Core/Http/WebSocketEndpoints.h"

#include "Core/Http/RealtimeTopics.h"

#include "Base/Config/ConfigValue.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "Net/WebSocket/WebSocketPeer.h"
#include "Net/WebSocket/WebSocketUpgradePolicy.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Web
{
    namespace
    {
        constexpr std::chrono::seconds kAuthenticationTimeout{5};
        constexpr std::string_view     kNodeTopic = "nodes";

        [[nodiscard]] bool equalsIgnoreCase(const std::string_view left, const std::string_view right)
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                const auto leftCharacter  = static_cast<unsigned char>(left[index]);
                const auto rightCharacter = static_cast<unsigned char>(right[index]);
                if (std::tolower(leftCharacter) != std::tolower(rightCharacter))
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 判 Origin 是否允许
         * @param request 升级请求
         * @param allowedOrigins 白名单；空表示同源
         */
        [[nodiscard]] bool isOriginAllowed(const AsynGyanis::Net::HttpRequest &request, const std::vector<std::string> &allowedOrigins)
        {
            const std::optional<std::string> origin = request.getHeader("origin");
            // 非浏览器客户端可以不带 Origin；浏览器的 WebSocket 一定带
            if (!origin.has_value())
            {
                return true;
            }
            if (!allowedOrigins.empty())
            {
                return std::find(allowedOrigins.begin(), allowedOrigins.end(), *origin) != allowedOrigins.end();
            }
            const std::optional<std::string> host = request.getHeader("host");
            if (!host.has_value())
            {
                return false;
            }
            const std::string sameOrigin = std::string{request.overTls() ? "https://" : "http://"} + *host;
            return equalsIgnoreCase(*origin, sameOrigin);
        }

        /**
         * @brief 5 秒内未认证时关闭连接
         * @param peer 目标连接
         * @param finished 处理器是否已退出
         */
        AsynGyanis::Core::Task<> closeAfterTimeout(AsynGyanis::Net::WebSocketPeer *peer, std::shared_ptr<std::atomic<bool>> finished)
        {
            // 处理器已经退出的情况下对端对象可能已销毁，不能再碰
            if (finished->load() || !peer->isOpen())
            {
                co_return;
            }
            co_await peer->close(AsynGyanis::Net::kWebSocketPolicyViolationCode, "unauthorized");
            co_return;
        }

        /**
         * @brief 解析首条认证消息
         * @param payload 文本负载
         * @return 令牌；类型或字段不符时为空
         */
        [[nodiscard]] std::optional<std::string> parseAuthToken(const std::string_view payload)
        {
            const std::optional<AsynGyanis::Base::ConfigValue> message = AsynGyanis::Base::parseConfigValue(payload);
            if (!message.has_value() || !message->is_object())
            {
                return std::nullopt;
            }
            const auto type  = message->find("type");
            const auto token = message->find("token");
            if (type == message->end() || !type->is_string() || type->get<std::string>() != "auth" || token == message->end() || !token->is_string())
            {
                return std::nullopt;
            }
            return token->get<std::string>();
        }
    } // namespace

    BrowserWebSocket::BrowserWebSocket(AsynGyanis::Core::EventLoop &loop) : m_loop(loop)
    {
    }

    BrowserWebSocket::~BrowserWebSocket()
    {
        m_isStopping.store(true);
        // 计时线程最长休眠 5 秒；这里等它们退出，避免线程继续向已停止的循环投递
        std::lock_guard<std::mutex> lock(m_threadsMutex);
        for (std::jthread &thread: m_authThreads)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
    }

    AsynGyanis::Net::WebSocketHub &BrowserWebSocket::hub() noexcept
    {
        return m_hub;
    }

    void BrowserWebSocket::startAuthenticationTimer(AsynGyanis::Net::WebSocketPeer &peer, std::shared_ptr<std::atomic<bool>> authenticated,
                                                    std::shared_ptr<std::atomic<bool>> finished)
    {
        // shortcut: 引擎 v4.0.0 的 receive() 没有超时参数，这里用一条休眠线程兜住首条认证的 5 秒；
        // 每条未认证连接占一条线程，且停机会等待其到期。升级到带超时的 receive 或可取消等待后删除。
        std::jthread timer(
                [this, peer = &peer, authenticated = std::move(authenticated), finished = std::move(finished)](std::stop_token) mutable
                {
                    std::this_thread::sleep_for(kAuthenticationTimeout);
                    if (authenticated->load() || m_isStopping.load())
                    {
                        return;
                    }
                    // 只把动作投回循环；线程本身不碰对端对象
                    m_loop.scheduler().postRemote(
                            [this, peer, authenticated, finished]
                            {
                                if (m_isStopping.load() || authenticated->load() || finished->load() || !peer->isOpen())
                                {
                                    return;
                                }
                                AsynGyanis::Core::Task<> task = closeAfterTimeout(peer, finished);
                                {
                                    std::lock_guard<std::mutex> taskLock(m_tasksMutex);
                                    m_timeoutTasks.push_back(std::move(task));
                                }
                                m_loop.scheduler().schedule(m_timeoutTasks.back().handle());
                            });
                });
        std::lock_guard<std::mutex> lock(m_threadsMutex);
        m_authThreads.push_back(std::move(timer));
    }

    void BrowserWebSocket::registerRoute(AsynGyanis::Net::Router                                                                         &router,
                                         std::function<AsynGyanis::Core::Task<std::optional<BrowserSubscriptionScope>>(std::string_view)> tokenValidator,
                                         std::vector<std::string>                                                                         allowedOrigins)
    {
        router.get("/api/ws",
                   [this, tokenValidator = std::move(tokenValidator),
                    allowedOrigins = std::move(allowedOrigins)](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       // Origin 不是同源也不在白名单时，直接 403，不给升级机会
                       if (!isOriginAllowed(request, allowedOrigins))
                       {
                           response.setStatus(403);
                           response.setBody("origin rejected");
                           co_return;
                       }
                       AsynGyanis::Net::WebSocketUpgradePolicy policy;
                       policy.originMode     = allowedOrigins.empty() ? AsynGyanis::Net::WebSocketOriginMode::SameOriginOnly : AsynGyanis::Net::WebSocketOriginMode::AllowList;
                       policy.allowedOrigins = allowedOrigins;
                       response.upgradeToWebSocket(
                               [this, tokenValidator](AsynGyanis::Net::WebSocketPeer &peer) -> AsynGyanis::Core::Task<>
                               {
                                   auto authenticated = std::make_shared<std::atomic<bool>>(false);
                                   auto finished      = std::make_shared<std::atomic<bool>>(false);
                                   // 会话收口会直接销毁处理器帧；靠 RAII 把退出标记送到超时路径
                                   struct FinishGuard
                                   {
                                       std::shared_ptr<std::atomic<bool>> flag;
                                       ~FinishGuard()
                                       {
                                           flag->store(true);
                                       }
                                   } finishGuard{finished};
                                   startAuthenticationTimer(peer, authenticated, finished);

                                   const std::optional<AsynGyanis::Net::WebSocketMessage> first = co_await peer.receive();
                                   if (!first.has_value() || first->opCode != AsynGyanis::Net::WebSocketOpCode::Text)
                                   {
                                       finished->store(true);
                                       co_return;
                                   }
                                   const std::optional<std::string>        token = parseAuthToken(first->payload);
                                   std::optional<BrowserSubscriptionScope> scope;
                                   if (token.has_value())
                                   {
                                       scope = co_await tokenValidator(*token);
                                   }
                                   if (!scope.has_value())
                                   {
                                       // 明确的策略拒绝：先关 1008，再让会话收口
                                       co_await peer.close(AsynGyanis::Net::kWebSocketPolicyViolationCode, "unauthorized");
                                       finished->store(true);
                                       co_return;
                                   }
                                   authenticated->store(true);
                                   const std::string acknowledgement = R"({"type":"auth","ok":true})";
                                   if (!co_await peer.sendText(acknowledgement))
                                   {
                                       finished->store(true);
                                       co_return;
                                   }
                                   // 认证回执之后所有出站都走集线器，处理器只读入站帧；三类主题共用同一条连接。
                                   // 平台管理员订阅跨租户主题，其余账号只订阅本租户主题，避免实时推送越权。
                                   const std::string                      nodeTopic    = scope->platformAdmin ? std::string{kNodeTopic} : tenantTopic(kNodeTopic, scope->tenantId);
                                   const std::string                      alarmTopic   = scope->platformAdmin ? std::string{"alarms"} : tenantTopic("alarms", scope->tenantId);
                                   const std::string                      commandTopic = scope->platformAdmin ? std::string{"commands"} : tenantTopic("commands", scope->tenantId);
                                   AsynGyanis::Net::WebSocketSubscription nodeSubscription    = m_hub.subscribe(nodeTopic, peer);
                                   AsynGyanis::Net::WebSocketSubscription alarmSubscription   = m_hub.subscribe(alarmTopic, peer);
                                   AsynGyanis::Net::WebSocketSubscription commandSubscription = m_hub.subscribe(commandTopic, peer);
                                   while (const auto message = co_await peer.receive())
                                   {
                                       // 客户端保活文本只用于续上连接的空闲计时，内容不参与业务
                                       static_cast<void>(message);
                                   }
                                   finished->store(true);
                                   co_return;
                               },
                               std::move(policy));
                       co_return;
                   });
    }
} // namespace EdgePulse::Web
