#include "Core/Http/AgentWebSocket.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Core/Command/AgentChannelRegistry.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/CertificateBinding.h"
#include "Core/Device/NodeRepository.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "Net/WebSocket/WebSocketPeer.h"
#include "Net/WebSocket/WebSocketUpgradePolicy.h"

#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace EdgePulse::Web
{
    namespace
    {
        [[nodiscard]] std::optional<AsynGyanis::Base::ConfigValue> parseObject(const std::string_view payload)
        {
            const auto value = AsynGyanis::Base::parseConfigValue(payload);
            if (!value.has_value() || !value->is_object())
            {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]] std::optional<std::int64_t> integerOf(const AsynGyanis::Base::ConfigValue &value, const std::string_view key)
        {
            const auto iterator = value.find(std::string{key});
            if (iterator == value.end() || !iterator->is_number_integer())
            {
                return std::nullopt;
            }
            return iterator->get<std::int64_t>();
        }

        [[nodiscard]] std::string stringOf(const AsynGyanis::Base::ConfigValue &value, const std::string_view key)
        {
            const auto iterator = value.find(std::string{key});
            return iterator != value.end() && iterator->is_string() ? iterator->get<std::string>() : std::string{};
        }
    } // namespace

    void registerAgentWebSocket(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, Device::NodeRepository &nodes, Command::CommandRepository &commands,
                                Command::AgentChannelRegistry &channels)
    {
        router.get("/api/agent/ws",
                   [&loop, &nodes, &commands, &channels](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       // Agent 请求没有会话：节点与命令都按全租户视图处理，归属由节点自身决定
                       const auto                              allNodes    = nodes.scoped(System::TenantScope{0, true});
                       const auto                              allCommands = commands.scoped(System::TenantScope{0, true});
                       AsynGyanis::Net::WebSocketUpgradePolicy policy;
                       // Agent 已在 mTLS 监听器上完成证书校验；此处不再要求浏览器 Origin
                       policy.originMode = AsynGyanis::Net::WebSocketOriginMode::AllowAny;
                       response.upgradeToWebSocket(
                               [&loop, allNodes, allCommands, &channels](AsynGyanis::Net::WebSocketPeer &peer) -> AsynGyanis::Core::Task<>
                               {
                                   std::int64_t nodeId = 0;
                                   // 首条消息必须是 hello；mTLS 已经挡住匿名对端，这里只校验节点确实存在
                                   while (const auto message = co_await peer.receive())
                                   {
                                       if (message->opCode != AsynGyanis::Net::WebSocketOpCode::Text)
                                       {
                                           continue;
                                       }
                                       const auto hello = parseObject(message->payload);
                                       if (!hello.has_value() || stringOf(*hello, "type") != "hello")
                                       {
                                           continue;
                                       }
                                       const auto id = integerOf(*hello, "nodeId");
                                       if (!id.has_value())
                                       {
                                           co_await peer.close(AsynGyanis::Net::kWebSocketPolicyViolationCode, "unknown node");
                                           co_return;
                                       }
                                       const auto node = co_await allNodes->findById(loop, *id);
                                       if (!node.has_value())
                                       {
                                           co_await peer.close(AsynGyanis::Net::kWebSocketPolicyViolationCode, "unknown node");
                                           co_return;
                                       }
                                       // mTLS 只证明对端持有 CA 签发的证书；节点身份还必须与登记的叶证书指纹一致
                                       if (const std::optional<std::string> bindingFailure = Device::certificateBindingFailure(peer.peerIdentity(), node->certFingerprint);
                                           bindingFailure.has_value())
                                       {
                                           LOG_WARN_FMT("Agent WebSocket 证书绑定失败：node={} reason={}", *id, *bindingFailure);
                                           co_await peer.close(AsynGyanis::Net::kWebSocketPolicyViolationCode, "certificate mismatch");
                                           co_return;
                                       }
                                       nodeId = *id;
                                       static_cast<void>(co_await allNodes->heartbeat(loop, nodeId, stringOf(*hello, "agentVersion")));
                                       channels.registerNode(nodeId, &peer);
                                       static_cast<void>(co_await peer.sendText(R"({"type":"hello","ok":true})"));
                                       break;
                                   }
                                   if (nodeId == 0)
                                   {
                                       co_return;
                                   }
                                   struct ChannelGuard
                                   {
                                       Command::AgentChannelRegistry  &channels;
                                       std::int64_t                    nodeId;
                                       AsynGyanis::Net::WebSocketPeer *peer;
                                       ~ChannelGuard()
                                       {
                                           channels.unregisterNode(nodeId, peer);
                                       }
                                   } guard{channels, nodeId, &peer};

                                   while (const auto message = co_await peer.receive())
                                   {
                                       if (message->opCode != AsynGyanis::Net::WebSocketOpCode::Text)
                                       {
                                           continue;
                                       }
                                       const auto value = parseObject(message->payload);
                                       if (!value.has_value())
                                       {
                                           continue;
                                       }
                                       const std::string type = stringOf(*value, "type");
                                       if (type == "device.pong" || type == "pong")
                                       {
                                           continue;
                                       }
                                       if (type == "command.result")
                                       {
                                           const auto commandId   = integerOf(*value, "commandId");
                                           const auto status      = stringOf(*value, "status");
                                           const auto result      = stringOf(*value, "result");
                                           const auto messageText = stringOf(*value, "message");
                                           if (commandId.has_value())
                                           {
                                               const auto command = co_await allCommands->find(loop, *commandId);
                                               if (!command.has_value() || command->nodeId != nodeId)
                                               {
                                                   continue;
                                               }
                                               static_cast<void>(co_await allCommands->finish(loop, *commandId, status.empty() ? "failed" : status, result, messageText));
                                           }
                                           continue;
                                       }
                                       if (type == "ping")
                                       {
                                           static_cast<void>(co_await peer.sendText(R"({"type":"pong"})"));
                                       }
                                   }
                                   co_return;
                               },
                               std::move(policy));
                       co_return;
                   });
    }
} // namespace EdgePulse::Web
