/**
 * @file Main.cpp
 * @brief H3 探针：只把 h3 路由器上存在的路径当作成功，TCP 回落会得到 404
 * @author Gyanis
 * @date 2026-10-20
 */

#include "Base/Config/ConfigValue.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Tls/TlsPolicy.h"
#include "Net/Http/Client/HttpClient.h"

#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace
{
    struct ProbeResult
    {
        int         status{0};
        std::string body;
        std::string error;
    };

    AsynGyanis::Core::Task<ProbeResult> probe(AsynGyanis::Core::EventLoop &loop, std::string url, const std::string &certificateAuthority)
    {
        ProbeResult result;
        try
        {
            AsynGyanis::Core::TlsPolicy policy;
            policy.certificateAuthorityFile = certificateAuthority;
            AsynGyanis::Net::HttpClient client(loop, {}, policy);
            client.setHttp3Enabled(true);
            AsynGyanis::Net::HttpClientRequest request;
            request.method      = "GET";
            const auto response = co_await client.send(url, request, std::chrono::seconds{15});
            if (!response)
            {
                result.error = "HTTP 请求失败（原因见引擎日志）";
            } else
            {
                result.status = response->statusCode;
                result.body   = response->body;
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
    std::string url = "https://127.0.0.1:8443/api/probe/h3";
    std::string ca  = "var/certs/ca.pem";
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--url" && index + 1 < argc)
        {
            url = argv[++index];
        } else if (argument == "--ca" && index + 1 < argc)
        {
            ca = argv[++index];
        } else if (argument == "--help")
        {
            std::cout << "用法：H3Probe [--url https://127.0.0.1:8443/api/probe/h3] [--ca var/certs/ca.pem]\n";
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
    AsynGyanis::Core::Task<>    task    = [](AsynGyanis::Core::EventLoop &runningLoop, std::string target, std::string authority,
                                             std::shared_ptr<std::promise<ProbeResult>> completion) -> AsynGyanis::Core::Task<>
    {
        completion->set_value(co_await probe(runningLoop, std::move(target), authority));
        co_return;
    }(loop, std::move(url), ca, promise);
    loop.scheduler().scheduleRemote(task.handle());

    const bool        isFinished = future.wait_for(std::chrono::seconds{30}) == std::future_status::ready;
    const ProbeResult result     = isFinished ? future.get() : ProbeResult{0, {}, "探针在 30 秒内没有完成"};
    loop.stop();
    if (loopThread.joinable())
    {
        loopThread.join();
    }

    if (!result.error.empty())
    {
        std::cerr << "h3 探针失败：" << result.error << '\n';
        return 1;
    }
    std::cout << "status=" << result.status << " body=" << result.body << '\n';
    // TCP 回落的 404 不能算通过：只有 h3 专属路径返回 200 才打印 h3
    if (result.status == 200 && result.body.find("\"h3\"") != std::string::npos)
    {
        std::cout << "h3\n";
        return 0;
    }
    std::cerr << "h3 未生效：请求落到了 TCP 回退路径\n";
    return 1;
}
