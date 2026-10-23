#include "Core/Http/MiddlewareSetup.h"

#include "Base/Log/LogMacros.h"
#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "Core/Http/RequestId.h"

#include <exception>
#include <functional>
#include <string>

namespace EdgePulse::Web
{
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
} // namespace EdgePulse::Web
