/**
 * @file Main.cpp
 * @brief mTLS Agent 探针：注册一个节点，按需发送心跳或走 WebSocket 接收命令
 * @author Gyanis
 * @date 2026-10-20
 */

#include "Base/Config/ConfigValue.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Tls/TlsContext.h"
#include "Core/Tls/TlsPolicy.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/WebSocket/WebSocketClient.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace
{
    struct ProbeOptions
    {
        std::string baseUrl{"https://127.0.0.1:9443"};
        std::string certificateAuthority{"var/certs/ca.pem"};
        std::string certificate{"var/certs/agent.pem"};
        std::string privateKey{"var/certs/agent-key.pem"};
        std::string fingerprint;
        std::string name{"edgepulse-agent"};
        std::string websocketUrl{"wss://127.0.0.1:9443/api/agent/ws"};
        bool        websocket{false};
    };

    struct ProbeResult
    {
        std::string nodeId;
        int         heartbeats{0};
        int         commands{0};
        bool        websocket{false};
        std::string error;
    };

    struct WebSocketEndpoint
    {
        bool          tls{true};
        std::string   host;
        std::uint16_t port{443};
        std::string   target{"/api/agent/ws"};
    };

    [[nodiscard]] std::optional<WebSocketEndpoint> parseWebSocketUrl(std::string_view url)
    {
        WebSocketEndpoint endpoint;
        if (url.starts_with("wss://"))
        {
            endpoint.tls  = true;
            endpoint.port = 443;
            url.remove_prefix(6);
        } else if (url.starts_with("ws://"))
        {
            endpoint.tls  = false;
            endpoint.port = 80;
            url.remove_prefix(5);
        } else
        {
            return std::nullopt;
        }
        const std::size_t      slash     = url.find('/');
        const std::string_view authority = url.substr(0, slash);
        endpoint.target                  = slash == std::string_view::npos ? std::string{"/"} : std::string{url.substr(slash)};
        const std::size_t colon          = authority.rfind(':');
        if (colon == std::string_view::npos)
        {
            endpoint.host = std::string{authority};
        } else
        {
            endpoint.host                   = std::string{authority.substr(0, colon)};
            const std::string_view portText = authority.substr(colon + 1);
            unsigned               port     = 0;
            const auto [end, error]         = std::from_chars(portText.data(), portText.data() + portText.size(), port);
            if (error != std::errc{} || end != portText.data() + portText.size() || port == 0 || port > 65535)
            {
                return std::nullopt;
            }
            endpoint.port = static_cast<std::uint16_t>(port);
        }
        if (endpoint.host.empty())
        {
            return std::nullopt;
        }
        return endpoint;
    }

    [[nodiscard]] std::string stringOf(const AsynGyanis::Base::ConfigValue &value, const std::string_view key)
    {
        const auto iterator = value.find(std::string{key});
        return iterator != value.end() && iterator->is_string() ? iterator->get<std::string>() : std::string{};
    }

    AsynGyanis::Core::Task<> runWebSocketProbe(AsynGyanis::Core::EventLoop &loop, const ProbeOptions &options, std::int64_t nodeId, ProbeResult &result)
    {
        const std::optional<WebSocketEndpoint> endpoint = parseWebSocketUrl(options.websocketUrl);
        if (!endpoint.has_value())
        {
            result.error = "WebSocket 地址不合法，需要 ws:// 或 wss:// 加主机与端口";
            co_return;
        }

        AsynGyanis::Core::TlsPolicy policy;
        policy.certificateAuthorityFile = options.certificateAuthority;
        AsynGyanis::Core::TlsContext tls(policy, AsynGyanis::Core::TlsContext::Role::Client);
        if (endpoint->tls && !tls.loadCertificate(options.certificate, options.privateKey))
        {
            result.error = "加载客户端证书失败";
            co_return;
        }

        AsynGyanis::Net::WebSocketClient::Configuration configuration;
        configuration.hostName      = endpoint->host;
        configuration.port          = endpoint->port;
        configuration.requestTarget = endpoint->target;
        configuration.clientTls     = endpoint->tls ? &tls : nullptr;
        auto connection             = co_await AsynGyanis::Net::WebSocketClient::connect(loop, configuration);
        if (!connection)
        {
            result.error = "WebSocket 握手失败：" + connection.error().message;
            co_return;
        }
        std::unique_ptr<AsynGyanis::Net::WebSocketClient> client = std::move(*connection);

        const AsynGyanis::Base::ConfigValue hello = AsynGyanis::Base::ConfigValue::object({{"type", "hello"}, {"nodeId", nodeId}, {"agentVersion", "0.1.0"}});
        if (!co_await client->sendText(hello.dump()))
        {
            result.error = "WebSocket hello 发送失败";
            co_return;
        }
        const auto reply = co_await client->receive();
        if (!reply || reply->opCode != AsynGyanis::Net::WebSocketOpCode::Text)
        {
            result.error = "WebSocket 未收到 hello 应答";
            co_return;
        }
        const auto replyJson = AsynGyanis::Base::parseConfigValue(reply->payload);
        if (!replyJson.has_value() || stringOf(*replyJson, "type") != "hello")
        {
            result.error = "WebSocket hello 应答格式不正确：" + reply->payload;
            co_return;
        }
        result.websocket = true;
        std::cout << "ws-ready nodeId=" << nodeId << std::endl;

        while (const auto frame = co_await client->receive())
        {
            if (frame->opCode == AsynGyanis::Net::WebSocketOpCode::Close)
            {
                break;
            }
            if (frame->opCode != AsynGyanis::Net::WebSocketOpCode::Text)
            {
                continue;
            }
            const auto value = AsynGyanis::Base::parseConfigValue(frame->payload);
            if (!value.has_value() || !value->is_object() || stringOf(*value, "type") != "command")
            {
                continue;
            }
            const auto command = value->find("command");
            if (command == value->end() || !command->is_object())
            {
                continue;
            }
            const auto identifier = command->find("id");
            if (identifier == command->end() || !identifier->is_number_integer())
            {
                continue;
            }
            const auto commandId       = identifier->get<std::int64_t>();
            const auto acknowledgement = AsynGyanis::Base::ConfigValue::object({{"type", "command.result"},
                                                                                {"commandId", commandId},
                                                                                {"status", "succeeded"},
                                                                                {"result", R"({"ok":true})"},
                                                                                {"message", "ws-agent 执行 " + stringOf(*command, "type")}});
            static_cast<void>(co_await client->sendText(acknowledgement.dump()));
            ++result.commands;
            static_cast<void>(co_await client->close(1000, "done"));
            break;
        }
        co_return;
    }

    AsynGyanis::Core::Task<ProbeResult> runProbe(AsynGyanis::Core::EventLoop &loop, const ProbeOptions &options)
    {
        ProbeResult result;
        try
        {
            AsynGyanis::Core::TlsPolicy policy;
            policy.certificateAuthorityFile = options.certificateAuthority;
            AsynGyanis::Net::HttpClient client(loop, {}, policy);
            if (!client.setClientCertificate(options.certificate, options.privateKey))
            {
                result.error = "加载客户端证书失败";
                co_return result;
            }

            AsynGyanis::Base::ConfigValue registration = AsynGyanis::Base::ConfigValue::object(
                    {{"name", options.name}, {"agentVersion", "0.1.0"}, {"certFingerprint", options.fingerprint}, {"labels", {{"probe", "true"}}}});
            const std::string                  registrationText = registration.dump();
            AsynGyanis::Net::HttpClientRequest request;
            request.method      = "POST";
            request.contentType = "application/json";
            request.body        = registrationText;

            const auto registrationResponse = co_await client.send(options.baseUrl + "/api/agent/register", request, std::chrono::seconds{10});
            if (!registrationResponse)
            {
                result.error = "注册请求失败（原因见引擎日志）";
                co_return result;
            }
            if (registrationResponse->statusCode != 200)
            {
                result.error = "注册返回 HTTP " + std::to_string(registrationResponse->statusCode) + "：" + registrationResponse->body;
                co_return result;
            }
            const std::optional<AsynGyanis::Base::ConfigValue> registrationJson = AsynGyanis::Base::parseConfigValue(registrationResponse->body);
            if (!registrationJson.has_value())
            {
                result.error = "注册响应不是 JSON";
                co_return result;
            }
            const auto data = registrationJson->find("data");
            if (data == registrationJson->end() || !(*data).contains("id"))
            {
                result.error = "注册响应缺少 data.id";
                co_return result;
            }
            result.nodeId = std::to_string((*data).at("id").get<std::int64_t>());

            if (options.websocket)
            {
                co_await runWebSocketProbe(loop, options, (*data).at("id").get<std::int64_t>(), result);
                co_return result;
            }

            for (int index = 0; index < 3; ++index)
            {
                AsynGyanis::Core::Timer ticker(loop);
                co_await ticker.waitFor(std::chrono::seconds{1});
                AsynGyanis::Base::ConfigValue      heartbeat = AsynGyanis::Base::ConfigValue::object({{"nodeId", (*data).at("id").get<std::int64_t>()}, {"agentVersion", "0.1.0"}});
                const std::string                  heartbeatText = heartbeat.dump();
                AsynGyanis::Net::HttpClientRequest heartbeatRequest;
                heartbeatRequest.method      = "POST";
                heartbeatRequest.contentType = "application/json";
                heartbeatRequest.body        = heartbeatText;
                const auto heartbeatResponse = co_await client.send(options.baseUrl + "/api/agent/heartbeat", heartbeatRequest, std::chrono::seconds{10});
                if (!heartbeatResponse)
                {
                    result.error = "心跳请求失败（原因见引擎日志）";
                    co_return result;
                }
                if (heartbeatResponse->statusCode != 200)
                {
                    result.error = "心跳返回 HTTP " + std::to_string(heartbeatResponse->statusCode) + "：" + heartbeatResponse->body;
                    co_return result;
                }
                ++result.heartbeats;
            }
        } catch (const std::exception &failure)
        {
            result.error = failure.what();
        }
        co_return result;
    }
} // namespace

int main(const int argc, char **argv)
{
    ProbeOptions options;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        const auto             requireValue = [&](std::string &destination)
        {
            if (index + 1 >= argc)
            {
                return false;
            }
            destination = argv[++index];
            return true;
        };
        if (argument == "--url")
        {
            if (!requireValue(options.baseUrl))
                return 2;
        } else if (argument == "--ca")
        {
            if (!requireValue(options.certificateAuthority))
                return 2;
        } else if (argument == "--cert")
        {
            if (!requireValue(options.certificate))
                return 2;
        } else if (argument == "--key")
        {
            if (!requireValue(options.privateKey))
                return 2;
        } else if (argument == "--fingerprint")
        {
            if (!requireValue(options.fingerprint))
                return 2;
        } else if (argument == "--name")
        {
            if (!requireValue(options.name))
                return 2;
        } else if (argument == "--ws")
        {
            options.websocket = true;
        } else if (argument == "--ws-url")
        {
            if (!requireValue(options.websocketUrl))
                return 2;
            options.websocket = true;
        } else if (argument == "--help")
        {
            std::cout << "用法：edgepulse-agent [--url https://127.0.0.1:9443] [--ca ca.pem] [--cert agent.pem] [--key agent-key.pem] [--fingerprint <sha256>] [--name "
                         "edgepulse-agent] [--ws] [--ws-url wss://127.0.0.1:9443/api/agent/ws]\n"
                         "  --ws  注册后保持 WebSocket 连接，收到一条命令并回报结果后退出\n";
            return 0;
        } else
        {
            std::cerr << "未知参数：" << argument << '\n';
            return 2;
        }
    }

    AsynGyanis::Core::EventLoop loop;
    std::jthread                loopThread([&loop] { loop.run(); });
    auto                        promise = std::make_shared<std::promise<ProbeResult>>();
    std::future<ProbeResult>    future  = promise->get_future();
    AsynGyanis::Core::Task<>    task    = [](AsynGyanis::Core::EventLoop &runningLoop, ProbeOptions probeOptions,
                                             std::shared_ptr<std::promise<ProbeResult>> completion) -> AsynGyanis::Core::Task<>
    {
        completion->set_value(co_await runProbe(runningLoop, probeOptions));
        co_return;
    }(loop, options, promise);
    loop.scheduler().scheduleRemote(task.handle());

    const bool        isFinished = future.wait_for(std::chrono::seconds{30}) == std::future_status::ready;
    const ProbeResult result     = isFinished ? future.get() : ProbeResult{"", 0, 0, false, "探针在 30 秒内没有完成"};
    loop.stop();
    if (loopThread.joinable())
    {
        loopThread.join();
    }

    if (!result.error.empty())
    {
        std::cerr << "Agent 探针失败：" << result.error << '\n';
        return 1;
    }
    std::cout << "nodeId=" << result.nodeId << " heartbeats=" << result.heartbeats << " ws=" << (result.websocket ? "ok" : "off") << " commands=" << result.commands << '\n';
    return 0;
}
