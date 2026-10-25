#include "Core/Http/AgentWebSocket.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Command/AgentChannelRegistry.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/CertificateBinding.h"
#include "Core/Device/NodeLifecycleService.h"
#include "Core/Device/NodeRepository.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "Net/WebSocket/WebSocketPeer.h"
#include "Net/WebSocket/WebSocketUpgradePolicy.h"

#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <exception>
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
                                Command::AgentChannelRegistry &channels, Device::NodeLifecycleService *lifecycle,
                                std::function<AsynGyanis::Core::Task<>(std::int64_t)> beforeHeartbeat)
    {
        router.get("/api/agent/ws",
                   [&loop, &nodes, &commands, &channels, lifecycle,
                    beforeHeartbeat = std::move(beforeHeartbeat)](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       // Agent 请求没有会话：节点与命令都按全租户视图处理，归属由节点自身决定
                       const auto                              allNodes    = nodes.scoped(System::TenantScope{0, true});
                       const auto                              allCommands = commands.scoped(System::TenantScope{0, true});
                       AsynGyanis::Net::WebSocketUpgradePolicy policy;
                       // Agent 已在 mTLS 监听器上完成证书校验；此处不再要求浏览器 Origin
                       policy.originMode = AsynGyanis::Net::WebSocketOriginMode::AllowAny;
                       response.upgradeToWebSocket(
                               [&loop, allNodes, allCommands, &channels, lifecycle, beforeHeartbeat](AsynGyanis::Net::WebSocketPeer &peer) -> AsynGyanis::Core::Task<>
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
                                       if (!node->certRevokedAt.empty())
                                       {
                                           LOG_WARN_FMT("Agent WebSocket 拒绝已吊销证书：node={}", *id);
                                           co_await peer.close(AsynGyanis::Net::kWebSocketPolicyViolationCode, "certificate revoked");
                                           co_return;
                                       }
                                       // mTLS 只证明对端持有 CA 签发的证书；节点身份还必须与登记的叶证书指纹一致。
                                       // 明文连接没有对端身份，沿用测试与开发链路的放行口径；TLS 连接则接受 active 或未过期的 previous。
                                       const auto &peerIdentity = peer.peerIdentity();
                                       if (peerIdentity && !Device::certificateMatchesNode(peerIdentity, *node, Common::nowDateTimeUtc()))
                                       {
                                           LOG_WARN_FMT("Agent WebSocket 证书绑定失败：node={} reason=证书与节点登记指纹不一致", *id);
                                           co_await peer.close(AsynGyanis::Net::kWebSocketPolicyViolationCode, "certificate mismatch");
                                           co_return;
                                       }
                                       nodeId = *id;
                                       if (beforeHeartbeat)
                                       {
                                           // 钩子落在心跳落库之前：测试可在此窗口内吊销节点，稳定命中心跳返回空的分支
                                           co_await beforeHeartbeat(nodeId);
                                       }
                                       const auto beat = co_await allNodes->heartbeat(loop, nodeId, stringOf(*hello, "agentVersion"));
                                       if (!beat.has_value())
                                       {
                                           // 心跳返回空表示节点在 hello 校验之后被吊销或删除；不能继续登记通道
                                           LOG_WARN_FMT("Agent WebSocket 心跳被拒：node={}", nodeId);
                                           co_await peer.close(AsynGyanis::Net::kWebSocketPolicyViolationCode, "certificate revoked");
                                           co_return;
                                       }
                                       // 登记通道前重读一次，避免把刚被吊销的节点留在通道表里
                                       const auto beforeRegister = co_await allNodes->findById(loop, nodeId);
                                       if (!beforeRegister.has_value() || !beforeRegister->certRevokedAt.empty())
                                       {
                                           LOG_WARN_FMT("Agent WebSocket 登记前发现证书已吊销：node={}", nodeId);
                                           co_await peer.close(AsynGyanis::Net::kWebSocketPolicyViolationCode, "certificate revoked");
                                           co_return;
                                       }
                                       if (lifecycle != nullptr)
                                       {
                                           try
                                           {
                                               // 非在线转换的心跳也要交给生命周期，靠它销案扫描插入的离线告警
                                               co_await lifecycle->onHeartbeat(loop, beat->node, beat->becameOnline);
                                           } catch (const std::exception &failure)
                                           {
                                               // 状态已落库；发布或销案失败只记日志，不能让 hello 失败
                                               LOG_ERROR_FMT("节点在线状态处理失败：node={} reason={}", nodeId, failure.what());
                                           }
                                       }
                                       channels.registerNode(nodeId, &peer);
                                       bool        afterRegisterFailed = false;
                                       std::string afterRegisterFailure;
                                       try
                                       {
                                           // 登记后再读一次：吊销可能恰好落在读取与登记之间，必须摘表并关闭
                                           const auto afterRegister = co_await allNodes->findById(loop, nodeId);
                                           if (!afterRegister.has_value() || !afterRegister->certRevokedAt.empty())
                                           {
                                               channels.unregisterNode(nodeId, &peer);
                                               LOG_WARN_FMT("Agent WebSocket 登记后发现证书已吊销：node={}", nodeId);
                                               co_await peer.close(AsynGyanis::Net::kWebSocketPolicyViolationCode, "证书已吊销");
                                               co_return;
                                           }
                                       } catch (const std::exception &failure)
                                       {
                                           // co_await 不能出现在 catch 体里：先记下原因，出了 catch 再摘表与关闭
                                           afterRegisterFailed  = true;
                                           afterRegisterFailure = failure.what();
                                       } catch (...)
                                       {
                                           afterRegisterFailed  = true;
                                           afterRegisterFailure = "未知异常";
                                       }
                                       if (afterRegisterFailed)
                                       {
                                           // 复查失败时通道已经指向这条连接：先摘表再关闭，不能让异常把悬空项留在注册表里
                                           channels.unregisterNode(nodeId, &peer);
                                           LOG_ERROR_FMT("Agent WebSocket 登记后复查失败：node={} reason={}", nodeId, afterRegisterFailure);
                                           try
                                           {
                                               // 1011 表示服务端内部错误；关闭本身再失败时通道已摘除，只记录原因
                                               co_await peer.close(1011, "登记后复查失败，连接已关闭");
                                           } catch (const std::exception &closeFailure)
                                           {
                                               LOG_ERROR_FMT("Agent WebSocket 关闭失败：node={} reason={}", nodeId, closeFailure.what());
                                           }
                                           co_return;
                                       }
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
