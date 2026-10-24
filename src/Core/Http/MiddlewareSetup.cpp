#include "Core/Http/MiddlewareSetup.h"

#include "Base/Log/LogMacros.h"
#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Middleware.h"
#include "Net/Http/Routing/Router.h"
#include "Core/Http/RequestId.h"

#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Web
{
    namespace
    {
        /**
         * @brief 判断路径是否属于存活/就绪探针
         * @param path 请求路径
         * @return true 表示不参与限流
         * @note /api/probe/ 覆盖 h3 与后续探针端点；探针被 429 会让编排器误判进程不健康
         */
        [[nodiscard]] bool isProbePath(const std::string_view path) noexcept
        {
            return path == "/healthz" || path == "/readyz" || path == "/api/health" || path.starts_with("/api/probe/");
        }
    } // namespace

    void applySecurityHeaders(AsynGyanis::Net::Router &router)
    {
        router.addMiddleware(
                [](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response, std::function<AsynGyanis::Core::Task<>()> next) -> AsynGyanis::Core::Task<>
                {
                    const std::string traceId = resolveRequestId(request);
                    try
                    {
                        co_await next();
                    } catch (const std::exception &failure)
                    {
                        // 内部原文只进日志：where/文件路径与符号名不该随响应交给调用方
                        LOG_ERROR_FMT("请求处理失败：{}", failure.what());
                        Common::writeFailure(response, Common::kCodeInternalServerError, Common::kCodeInternalServerError, "internal server error", traceId);
                    }

                    static_cast<void>(response.setHeader("x-content-type-options", "nosniff"));
                    static_cast<void>(response.setHeader("x-frame-options", "DENY"));
                    static_cast<void>(response.setHeader("referrer-policy", "no-referrer"));
                    static_cast<void>(response.setHeader("content-security-policy",
                                                         "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
                                                         "connect-src 'self' ws: wss:"));
                    if (request.overTls())
                    {
                        static_cast<void>(response.setHeader("strict-transport-security", "max-age=31536000"));
                    }
                    co_return;
                });
    }

    AsynGyanis::Net::MiddlewareFunc rateLimitExceptProbes(std::shared_ptr<AsynGyanis::Net::TokenBucket> bucket)
    {
        const auto limited = AsynGyanis::Net::tokenBucketRateLimiterMiddleware(std::move(bucket));
        return [limited](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response, std::function<AsynGyanis::Core::Task<>()> next) -> AsynGyanis::Core::Task<>
        {
            // 探针不取令牌也不改响应：开销只剩一次路径比较
            if (isProbePath(request.path()))
            {
                co_await next();
                co_return;
            }
            co_await limited(request, response, std::move(next));
        };
    }

    void applyRateLimit(AsynGyanis::Net::Router &router, const std::shared_ptr<AsynGyanis::Net::TokenBucket> &bucket)
    {
        // 空桶代表运维显式关闭限流：不注册比注册一个恒放行的中间件更容易从路由表上确认
        if (bucket != nullptr)
        {
            router.addMiddleware(rateLimitExceptProbes(bucket));
        }
    }
} // namespace EdgePulse::Web
