#include "AgentWebSocket.h"

#include "CommandDispatcher.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/Tls/TlsContext.h"
#include "Core/Tls/TlsPolicy.h"

#include "Net/Http/Client/HttpClient.h"
#include "Net/WebSocket/WebSocketClient.h"

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Agent
{
    namespace
    {
        constexpr std::chrono::seconds kCommandReceiveWait{60}; ///< 等待下一条服务端消息的时限，超时只继续等

        /**
         * @brief 取对象里的字符串字段
         * @param value JSON 对象
         * @param key 字段名
         * @return std::string 字段文本；缺失或类型不符时为空
         */
        [[nodiscard]] std::string stringField(const AsynGyanis::Base::ConfigValue &value, const std::string_view key)
        {
            const auto iterator = value.find(std::string{key});
            return iterator != value.end() && iterator->is_string() ? iterator->get<std::string>() : std::string{};
        }
    } // namespace

    AgentWebSocket::AgentWebSocket(AsynGyanis::Core::EventLoop &loop, const AgentConfiguration &configuration) :
        m_loop(loop), m_handshakeTimeout(std::chrono::seconds{configuration.requestTimeoutSeconds > 0 ? configuration.requestTimeoutSeconds : 30})
    {
        std::string baseUrl = configuration.baseUrl;
        while (baseUrl.size() > 1 && baseUrl.back() == '/')
        {
            baseUrl.pop_back();
        }

        try
        {
            const AsynGyanis::Net::ParsedUrl parsed = AsynGyanis::Net::parseUrl(baseUrl);
            m_endpoint.tls                          = parsed.scheme == "https";
            m_endpoint.host                         = parsed.host;
            m_endpoint.port                         = parsed.port;
            std::string prefix                      = parsed.path;
            if (prefix.find('?') != std::string::npos)
            {
                m_initializationError = "WebSocket 地址不能从带查询串的 base_url 派生，请去掉查询串";
                return;
            }
            while (prefix.size() > 1 && prefix.back() == '/')
            {
                prefix.pop_back();
            }
            if (prefix == "/")
            {
                prefix.clear();
            }
            m_endpoint.target = prefix + "/api/agent/ws";
        } catch (const std::exception &failure)
        {
            m_initializationError = "WebSocket 地址从 base_url 派生失败：" + std::string{failure.what()};
            return;
        }

        if (!m_endpoint.tls)
        {
            return;
        }

        try
        {
            AsynGyanis::Core::TlsPolicy policy;
            if (!configuration.caFile.empty())
            {
                policy.certificateAuthorityFile = configuration.caFile.string();
            }
            m_tls = std::make_unique<AsynGyanis::Core::TlsContext>(policy, AsynGyanis::Core::TlsContext::Role::Client);
            if (!m_tls->loadCertificate(configuration.certificateFile.string(), configuration.privateKeyFile.string()))
            {
                m_tls.reset();
                m_initializationError = "加载 WebSocket 客户端证书与私钥失败：请确认两个文件都是 PEM 编码且相互配对（证书：" + configuration.certificateFile.string() + "，私钥：" +
                                        configuration.privateKeyFile.string() + "）";
            }
        } catch (const std::exception &failure)
        {
            m_tls.reset();
            m_initializationError = "初始化 WebSocket TLS 上下文失败：" + std::string{failure.what()};
        }
    }

    AgentWebSocket::~AgentWebSocket() = default;

    AsynGyanis::Core::Task<std::expected<void, std::string>> AgentWebSocket::serve(CommandDispatcher &dispatcher, const std::int64_t nodeId, std::string agentVersion,
                                                                                   const ConnectionObserver &onConnectionChanged)
    {
        if (!m_initializationError.empty())
        {
            co_return std::unexpected(m_initializationError);
        }

        AsynGyanis::Net::WebSocketClient::Configuration configuration;
        configuration.hostName         = m_endpoint.host;
        configuration.port             = m_endpoint.port;
        configuration.requestTarget    = m_endpoint.target;
        configuration.clientTls        = m_endpoint.tls ? m_tls.get() : nullptr;
        configuration.handshakeTimeout = m_handshakeTimeout;

        auto connection = co_await AsynGyanis::Net::WebSocketClient::connect(m_loop, configuration);
        if (!connection.has_value())
        {
            co_return std::unexpected("WebSocket 握手失败：" + connection.error().message);
        }
        std::unique_ptr<AsynGyanis::Net::WebSocketClient> client = std::move(*connection);

        const AsynGyanis::Base::ConfigValue hello = AsynGyanis::Base::ConfigValue::object({{"type", "hello"}, {"nodeId", nodeId}, {"agentVersion", agentVersion}});
        if (!co_await client->sendText(hello.dump()))
        {
            co_return std::unexpected("WebSocket hello 发送失败");
        }

        const auto reply = co_await client->receiveFor(m_handshakeTimeout);
        if (!reply.has_value() || reply->opCode != AsynGyanis::Net::WebSocketOpCode::Text)
        {
            co_return std::unexpected("WebSocket 未在时限内收到 hello 应答");
        }
        const auto replyValue = AsynGyanis::Base::parseConfigValue(reply->payload);
        if (!replyValue.has_value() || !replyValue->is_object() || stringField(*replyValue, "type") != "hello")
        {
            co_return std::unexpected("WebSocket hello 应答格式不正确：" + reply->payload);
        }
        const auto accepted = replyValue->find("ok");
        if (accepted == replyValue->end() || !accepted->is_boolean() || !accepted->get<bool>())
        {
            co_return std::unexpected("WebSocket hello 应答没有确认成功：" + reply->payload);
        }

        if (onConnectionChanged)
        {
            onConnectionChanged(true);
        }
        struct DisconnectNotify
        {
            const ConnectionObserver &observer; ///< 上层登记的状态回调

            ~DisconnectNotify()
            {
                if (observer)
                {
                    observer(false);
                }
            }
        } notify{onConnectionChanged};

        for (;;)
        {
            const auto frame = co_await client->receiveFor(kCommandReceiveWait);
            if (!frame.has_value())
            {
                if (frame.error().kind == AsynGyanis::Net::WebSocketSessionErrorKind::ReceiveTimedOut && client->isOpen())
                {
                    continue;
                }
                LOG_WARN_FMT("WebSocket 命令通道读取结束：{}", frame.error().message);
                break;
            }
            if (frame->opCode == AsynGyanis::Net::WebSocketOpCode::Close)
            {
                break;
            }
            if (frame->opCode != AsynGyanis::Net::WebSocketOpCode::Text)
            {
                continue;
            }

            const auto value = AsynGyanis::Base::parseConfigValue(frame->payload);
            if (!value.has_value() || !value->is_object())
            {
                LOG_WARN("WebSocket 收到无法解析的文本帧，已跳过");
                continue;
            }

            const std::string type = stringField(*value, "type");
            if (type == "ping")
            {
                if (!co_await client->sendText(R"({"type":"pong"})"))
                {
                    break;
                }
                continue;
            }
            if (type != "command")
            {
                LOG_DEBUG_FMT("WebSocket 收到未处理的消息类型 {}，已跳过", type);
                continue;
            }

            const auto commandValue = value->find("command");
            if (commandValue == value->end())
            {
                LOG_WARN("WebSocket 命令消息缺少 command 对象，已跳过");
                continue;
            }
            const auto parsed = parseAgentCommand(*commandValue);
            if (!parsed.has_value())
            {
                if (parsed.error().commandId > 0)
                {
                    co_await dispatcher.failCommand(parsed.error().commandId, parsed.error().message);
                } else
                {
                    LOG_WARN_FMT("WebSocket 命令无法识别，已跳过：{}", parsed.error().message);
                }
                continue;
            }
            co_await dispatcher.onWebSocketCommand(std::move(*parsed));
        }

        co_return std::expected<void, std::string>{};
    }
} // namespace EdgePulse::Agent
