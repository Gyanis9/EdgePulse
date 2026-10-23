#include "Core/Http/ReverseProxy.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Core/Auth/AuthService.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/Message/HttpMethod.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::Web
{
    namespace
    {
        [[nodiscard]] bool isHopByHopHeader(const std::string_view name)
        {
            return name == "connection" || name == "keep-alive" || name == "proxy-authenticate" || name == "proxy-authorization" || name == "te" || name == "trailer" ||
                   name == "transfer-encoding" || name == "upgrade" || name == "content-length";
        }

        [[nodiscard]] bool isForwardedRequestHeader(const std::string_view name)
        {
            return name == "accept" || name == "accept-language" || name == "content-type" || name == "if-none-match" || name == "if-modified-since" || name == "range" ||
                   name == "user-agent";
        }

        [[nodiscard]] std::string targetPath(const ReverseProxyConfiguration &configuration, const std::string_view path)
        {
            std::string target{path};
            if (configuration.stripPrefix && target.starts_with(configuration.prefix))
            {
                target.erase(0, configuration.prefix.size());
            }
            if (target.empty() || target.front() != '/')
            {
                target.insert(target.begin(), '/');
            }
            return target;
        }

        [[nodiscard]] std::string upstreamUrl(const ReverseProxyConfiguration &configuration, const std::string_view path, const std::string_view uri)
        {
            std::string url = configuration.upstream;
            while (!url.empty() && url.back() == '/')
            {
                url.pop_back();
            }
            url += targetPath(configuration, path);
            if (const std::size_t query = uri.find('?'); query != std::string_view::npos)
            {
                url.append(uri.substr(query));
            }
            return url;
        }
    } // namespace

    void registerReverseProxy(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, ReverseProxyConfiguration configuration, Auth::AuthService *auth)
    {
        if (configuration.prefix.empty() || configuration.prefix.front() != '/' || configuration.prefix == "/" || configuration.prefix == "/api")
        {
            throw std::invalid_argument("反向代理 prefix 必须以 / 开头，且不能是 / 或 /api");
        }
        if (!configuration.upstream.starts_with("http://") && !configuration.upstream.starts_with("https://"))
        {
            throw std::invalid_argument("反向代理 upstream 必须是 http(s) 地址");
        }
        if (configuration.requireSession && auth == nullptr)
        {
            throw std::invalid_argument("反向代理要求会话时没有可用的认证服务");
        }
        if (configuration.timeoutMilliseconds <= 0 || configuration.maximumRequestBodyBytes == 0 || configuration.maximumResponseBodyBytes == 0)
        {
            throw std::invalid_argument("反向代理的超时与正文上限必须是正数");
        }
        auto       configurationHandle = std::make_shared<ReverseProxyConfiguration>(std::move(configuration));
        const auto handler = [&loop, configurationHandle, auth](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
        {
            const std::string traceId = Web::resolveRequestId(request);
            if (configurationHandle->requireSession)
            {
                const auto session = co_await Web::requireSession(*auth, loop, request, response, configurationHandle->permission, traceId);
                if (!session.has_value())
                {
                    co_return;
                }
            }
            if (request.body().size() > configurationHandle->maximumRequestBodyBytes)
            {
                Common::writeFailure(response, Common::kCodePayloadTooLarge, Common::kCodePayloadTooLarge, "请求正文超过反向代理上限", traceId);
                co_return;
            }
            const std::string target = targetPath(*configurationHandle, request.path());
            if (target.find("/../") != std::string::npos || target.ends_with("/.."))
            {
                Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "反向代理不接受包含父目录段的路径", traceId);
                co_return;
            }

            const std::string_view method = AsynGyanis::Net::methodKeyword(request.method());
            if (method.empty())
            {
                Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "不支持转发的 HTTP 方法", traceId);
                co_return;
            }
            // body 与 contentType 在客户端里是视图，必须活到 co_await 返回；不能直接指向临时字符串
            const std::string requestBody{request.body()};
            std::string       requestContentType;

            AsynGyanis::Net::HttpClientRequest upstreamRequest;
            upstreamRequest.method = std::string{method};
            upstreamRequest.body   = requestBody;
            for (const auto &[name, value]: request.headers())
            {
                if (!isForwardedRequestHeader(name))
                {
                    continue;
                }
                if (name == "content-type")
                {
                    requestContentType = value;
                } else
                {
                    upstreamRequest.headers.emplace_back(name, value);
                }
            }
            upstreamRequest.contentType = requestContentType;
            if (!request.remoteIp().empty())
            {
                upstreamRequest.headers.emplace_back("X-Forwarded-For", request.remoteIp());
            }

            const std::string url = upstreamUrl(*configurationHandle, request.path(), request.uri());
            const auto        upstreamResponse =
                    co_await AsynGyanis::Net::HttpClient::send(loop, url, upstreamRequest, std::chrono::milliseconds{configurationHandle->timeoutMilliseconds});
            if (!upstreamResponse)
            {
                Common::writeFailure(response, Common::kCodeBadGateway, Common::kCodeBadGateway, "上游转发失败：" + upstreamResponse.error(), traceId);
                co_return;
            }
            if (upstreamResponse->body.size() > configurationHandle->maximumResponseBodyBytes)
            {
                Common::writeFailure(response, Common::kCodeBadGateway, Common::kCodeBadGateway, "上游响应正文超过反向代理上限", traceId);
                co_return;
            }
            response.setStatus(upstreamResponse->statusCode);
            for (const auto &[name, value]: upstreamResponse->headers)
            {
                if (!isHopByHopHeader(name))
                {
                    static_cast<void>(response.setHeader(name, value));
                }
            }
            response.setOwnedBody(upstreamResponse->body);
            co_return;
        };
        router.any(configurationHandle->prefix, handler);
        router.any(configurationHandle->prefix + "/*", handler);
    }
} // namespace EdgePulse::Web
