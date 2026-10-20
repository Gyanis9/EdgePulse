/**
 * @file Main.cpp
 * @brief mTLS Agent 探针：注册一个节点并发送若干心跳
 * @author Gyanis
 * @date 2026-10-20
 */

#include "Base/Config/ConfigValue.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Tls/TlsPolicy.h"
#include "Net/Http/Client/HttpClient.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
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
    };

    struct ProbeResult
    {
        std::string nodeId;
        int         heartbeats{0};
        std::string error;
    };

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
        } else if (argument == "--help")
        {
            std::cout << "用法：edgepulse-agent [--url https://127.0.0.1:9443] [--ca ca.pem] [--cert agent.pem] [--key agent-key.pem] [--fingerprint <sha256>] [--name "
                         "edgepulse-agent]\n";
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
    const ProbeResult result     = isFinished ? future.get() : ProbeResult{"", 0, "探针在 30 秒内没有完成"};
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
    std::cout << "nodeId=" << result.nodeId << " heartbeats=" << result.heartbeats << '\n';
    return 0;
}
