#include "Core/Http/WebSocketEndpoints.h"

#include "Core/Auth/AuthService.h"
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

    AsynGyanis::Net::WebSocketHub &BrowserWebSocket::hub() noexcept
    {
        return m_hub;
    }

    std::function<AsynGyanis::Core::Task<std::optional<BrowserSubscriptionScope>>(std::string_view)> browserTokenValidator(std::shared_ptr<Auth::AuthService> auth,
                                                                                                                           AsynGyanis::Core::EventLoop       &loop)
    {
        return [auth = std::move(auth), &loop](const std::string_view token) -> AsynGyanis::Core::Task<std::optional<BrowserSubscriptionScope>>
        {
            const auto session = co_await auth->authenticate(loop, token);
            // 浏览器实时通道不接收 API 令牌；令牌只用于 HTTP 的有权限接口
            if (!session.has_value() || session->apiToken)
            {
                co_return std::nullopt;
            }
            co_return BrowserSubscriptionScope{session->tenantId, session->platformAdmin};
        };
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
                                   // 首条消息与令牌校验共用同一份预算：两者合计超过 5 秒即按 1008 收口
                                   const auto                                             authenticationDeadline = std::chrono::steady_clock::now() + kAuthenticationTimeout;
                                   const std::optional<AsynGyanis::Net::WebSocketMessage> first                  = co_await peer.receiveFor(kAuthenticationTimeout);
                                   if (!first.has_value())
                                   {
                                       // 超时只结束等待，连接仍可用：由这里主动收口；对端已断开则无需再发 Close
                                       if (peer.isOpen())
                                       {
                                           co_await peer.close(AsynGyanis::Net::kWebSocketPolicyViolationCode, "unauthorized");
                                       }
                                       co_return;
                                   }
                                   if (first->opCode != AsynGyanis::Net::WebSocketOpCode::Text || std::chrono::steady_clock::now() > authenticationDeadline)
                                   {
                                       co_await peer.close(AsynGyanis::Net::kWebSocketPolicyViolationCode, "unauthorized");
                                       co_return;
                                   }
                                   const std::optional<std::string>        token = parseAuthToken(first->payload);
                                   std::optional<BrowserSubscriptionScope> scope;
                                   if (token.has_value())
                                   {
                                       scope = co_await tokenValidator(*token);
                                   }
                                   if (!scope.has_value() || std::chrono::steady_clock::now() > authenticationDeadline)
                                   {
                                       // 明确的策略拒绝或校验超出预算：先关 1008，再让会话收口
                                       co_await peer.close(AsynGyanis::Net::kWebSocketPolicyViolationCode, "unauthorized");
                                       co_return;
                                   }
                                   const std::string acknowledgement = R"({"type":"auth","ok":true})";
                                   if (!co_await peer.sendText(acknowledgement))
                                   {
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
                                   co_return;
                               },
                               std::move(policy));
                       co_return;
                   });
    }
} // namespace EdgePulse::Web
