#include "Plugins/AuthPlugin/OidcController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/QueryParameters.h"
#include "Base/Json/JsonFields.h"
#include "Core/Auth/OidcService.h"
#include "Core/Http/RequestId.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <string>

namespace EdgePulse::Auth
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        [[nodiscard]] std::string safeReturnPath(std::string path)
        {
            // 只接受本站绝对路径：协议相对地址与反斜杠会把浏览器带去别的源
            if (path.empty() || path.front() != '/' || path.starts_with("//") || path.find('\\') != std::string::npos || path.size() > 512)
            {
                return {};
            }
            return path;
        }
    } // namespace

    void registerOidcStatusRoute(AsynGyanis::Net::Router &router, const bool enabled)
    {
        router.get("/api/auth/oidc/enabled",
                   [enabled](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       Common::writeSuccess(response, ConfigValue::object({{"enabled", enabled}, {"label", "企业登录"}}), traceId);
                       co_return;
                   });
    }

    void registerOidcRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, OidcService &oidc)
    {
        router.get("/api/auth/oidc/start",
                   [&loop, &oidc](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string     traceId    = Web::resolveRequestId(request);
                       const std::string     returnPath = safeReturnPath(Common::queryText(request.queryParams(), "return"));
                       const OidcStartResult result     = co_await oidc.start(loop, returnPath);
                       if (!result.ok)
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, result.error, traceId);
                           co_return;
                       }
                       response.setStatus(302);
                       response.setHeader("Location", result.redirectUrl);
                       co_return;
                   });

        router.get("/api/auth/oidc/callback",
                   [&loop, &oidc](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const Common::QueryMap   query  = request.queryParams();
                       const std::string        code   = Common::queryText(query, "code");
                       const std::string        state  = Common::queryText(query, "state");
                       const OidcCallbackResult result = co_await oidc.handleCallback(loop, code, state);
                       response.setStatus(302);
                       response.setHeader("Location", result.redirectUrl);
                       co_return;
                   });

        router.post("/api/auth/oidc/exchange",
                    [&loop, &oidc](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        body    = request.jsonBody();
                        if (!body.has_value() || !body->is_object())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                            co_return;
                        }
                        const OidcExchangeResult result = co_await oidc.exchange(loop, Json::stringField(*body, "code"), request.remoteIp(), traceId);
                        if (!result.ok)
                        {
                            // 兑换失败保留服务层给出的状态：MFA 暂不可用是 503，不是凭据无效
                            const int status = result.httpStatus > 0 ? result.httpStatus : Common::kCodeUnauthorized;
                            Common::writeFailure(response, status, status, result.error, traceId);
                            co_return;
                        }
                        if (result.mfaRequired)
                        {
                            // 外部身份已确认，但本地账号还差第二因素；只回挑战，不下发会话
                            response.setHeader("Cache-Control", "no-store");
                            Common::writeSuccess(response,
                                                 ConfigValue::object({{"mfaRequired", true},
                                                                      {"challengeToken", result.challengeToken},
                                                                      {"expiresIn", result.challengeExpiresIn},
                                                                      {"returnPath", result.returnPath}}),
                                                 traceId);
                            co_return;
                        }
                        Common::writeSuccess(response, ConfigValue::object({{"token", result.token}, {"expiresIn", result.expiresIn}, {"returnPath", result.returnPath}}), traceId);
                        co_return;
                    });
    }
} // namespace EdgePulse::Auth
