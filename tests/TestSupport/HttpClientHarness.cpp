#include "TestSupport/HttpClientHarness.h"

#include "Core/Socket/InetAddress.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/Router.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>

namespace EdgePulse::TestSupport
{
    namespace
    {
        constexpr std::chrono::seconds      kClientTimeout{5};              ///< 单次请求与等待响应的上限
        constexpr int                       kListenPollAttemptCount = 5000; ///< 等端口就绪的轮询次数
        constexpr std::chrono::milliseconds kListenPollInterval{1};         ///< 每次轮询的间隔

        [[nodiscard]] bool equalsIgnoreCase(const std::string_view left, const std::string_view right)
        {
            // 头部名按 RFC 9110 大小写不敏感，逐字节折小写比较
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                if (std::tolower(static_cast<unsigned char>(left[index])) != std::tolower(static_cast<unsigned char>(right[index])))
                {
                    return false;
                }
            }
            return true;
        }

        AsynGyanis::Core::Task<> performRequest(AsynGyanis::Core::EventLoop &loop, std::string url, std::string method, std::string body, std::string contentType,
                                                std::vector<std::pair<std::string, std::string>> headers, std::shared_ptr<std::promise<HttpResult>> promise)
        {
            HttpResult result;
            try
            {
                AsynGyanis::Net::HttpClientRequest request;
                request.method      = std::move(method);
                request.body        = body;
                request.contentType = contentType;
                request.headers     = std::move(headers);

                // 正文视图指向本协程帧里的 body，co_await 结束前一直有效
                const auto response = co_await AsynGyanis::Net::HttpClient::send(loop, url, request, kClientTimeout);
                if (!response.has_value())
                {
                    result.error = response.error();
                } else
                {
                    result.transportOk = true;
                    result.status      = response->statusCode;
                    result.body        = response->body;
                    result.headers     = response->headers;
                }
            } catch (const std::exception &failure)
            {
                // 传输层之外的异常也要带回测试线程，否则等待方只看到超时
                result.error = failure.what();
            }
            promise->set_value(std::move(result));
        }
    } // namespace

    std::optional<std::string> HttpResult::header(const std::string_view name) const
    {
        // 同名多条时取第一条，与引擎 HttpClientResponse::headerValue 的口径一致
        for (const auto &[fieldName, fieldValue]: headers)
        {
            if (equalsIgnoreCase(fieldName, name))
            {
                return fieldValue;
            }
        }
        return std::nullopt;
    }

    std::optional<AsynGyanis::Base::ConfigValue> HttpResult::json() const
    {
        // 解析失败返回空而不是抛异常：用例判空后打印原始正文
        return AsynGyanis::Base::parseConfigValue(body);
    }

    ServerHarness::ServerHarness(const std::function<void(AsynGyanis::Net::Router &)> &configure)
    {
        // 端口 0 让内核分配，多个用例并行时不会撞固定端口
        const std::optional<AsynGyanis::Core::InetAddress> address = AsynGyanis::Core::InetAddress::resolve("127.0.0.1", 0);
        if (!address.has_value())
        {
            throw std::runtime_error("回环地址解析失败");
        }
        m_server = std::make_unique<AsynGyanis::Net::HttpServer>(m_loop, *address);
        configure(m_server->router());
        m_acceptTask.emplace(m_server->start());
        // 循环尚未开始运行，本线程可安全入本地队列
        m_loop.scheduler().schedule(m_acceptTask->handle());
        m_loopThread.emplace([this] { m_loop.run(); });

        // 绑定发生在接受协程首次恢复时，等端口亮出来再让用例发请求
        for (int attempt = 0; attempt < kListenPollAttemptCount; ++attempt)
        {
            m_port = m_server->listeningPort();
            if (m_port != 0)
            {
                break;
            }
            std::this_thread::sleep_for(kListenPollInterval);
        }
        if (m_port == 0)
        {
            throw std::runtime_error("服务器未在时限内进入监听状态");
        }
    }

    ServerHarness::~ServerHarness()
    {
        // 先停循环并等承载线程退出，再按成员声明逆序销毁协程帧与服务器
        m_loop.stop();
        if (m_loopThread.has_value() && m_loopThread->joinable())
        {
            m_loopThread->join();
        }
    }

    std::uint16_t ServerHarness::port() const noexcept
    {
        return m_port;
    }

    HttpResult ServerHarness::send(std::string method, std::string path, std::string body, std::string contentType, std::vector<std::pair<std::string, std::string>> headers)
    {
        if (path.empty() || path.front() != '/')
        {
            path.insert(path.begin(), '/');
        }
        std::string url = "http://127.0.0.1:" + std::to_string(m_port) + path;

        auto                     promise = std::make_shared<std::promise<HttpResult>>();
        auto                     future  = promise->get_future();
        AsynGyanis::Core::Task<> task    = performRequest(m_loop, std::move(url), std::move(method), std::move(body), std::move(contentType), std::move(headers), promise);
        m_loop.scheduler().scheduleRemote(task.handle());
        // 帧交给夹具保管到析构：请求超时后它可能仍在挂起，提前销毁就是与恢复竞态
        m_pendingRequests.push_back(std::move(task));

        if (future.wait_for(kClientTimeout) != std::future_status::ready)
        {
            HttpResult timeout;
            timeout.error = "客户端等待响应超时";
            return timeout;
        }
        return future.get();
    }

    HttpResult ServerHarness::get(std::string path, std::vector<std::pair<std::string, std::string>> headers)
    {
        return send("GET", std::move(path), {}, {}, std::move(headers));
    }

    HttpResult ServerHarness::post(std::string path, std::string body, std::string contentType, std::vector<std::pair<std::string, std::string>> headers)
    {
        return send("POST", std::move(path), std::move(body), std::move(contentType), std::move(headers));
    }
} // namespace EdgePulse::TestSupport
