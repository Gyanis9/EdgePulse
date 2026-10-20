#include "Plugins/AuthPlugin/AuthController.h"

#include "Core/Auth/AuthService.h"
#include "Core/Auth/CaptchaService.h"
#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"
#include "Core/Http/Authorization.h"
#include "Core/Http/RequestId.h"

#include <Core/EventLoop/EventLoop.h>

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::Auth
{
    namespace
    {
        /**
         * @brief 取请求 JSON 对象里的字符串字段
         */
        [[nodiscard]] std::optional<std::string> stringField(const AsynGyanis::Base::ConfigValue &body, const std::string_view key)
        {
            if (!body.is_object())
            {
                return std::nullopt;
            }
            const auto iterator = body.find(key);
            if (iterator == body.end() || !iterator->is_string())
            {
                return std::nullopt;
            }
            return iterator->get<std::string>();
        }

        /**
         * @brief 把字符串列表转成 JSON 数组
         */
        [[nodiscard]] AsynGyanis::Base::ConfigValue toJsonArray(const std::vector<std::string> &values)
        {
            AsynGyanis::Base::ConfigValue array = AsynGyanis::Base::ConfigValue::array();
            for (const std::string &value: values)
            {
                array.push_back(value);
            }
            return array;
        }

        /**
         * @brief 把菜单节点转成前端 RouteItem 形状
         */
        [[nodiscard]] AsynGyanis::Base::ConfigValue menuToJson(const System::MenuNode &node)
        {
            AsynGyanis::Base::ConfigValue value    = AsynGyanis::Base::ConfigValue::object({{"id", node.id},
                                                                                            {"parentId", node.parentId},
                                                                                            {"name", node.name},
                                                                                            {"type", node.type},
                                                                                            {"path", node.path},
                                                                                            {"component", node.component},
                                                                                            {"icon", node.icon},
                                                                                            {"permission", node.permission}});
            AsynGyanis::Base::ConfigValue children = AsynGyanis::Base::ConfigValue::array();
            for (const System::MenuNode &child: node.children)
            {
                children.push_back(menuToJson(child));
            }
            value["children"] = std::move(children);
            return value;
        }

        /**
         * @brief 写认证失败
         */
        void writeAuthError(AsynGyanis::Net::HttpResponse &response, const AuthError &error, const std::string_view traceId)
        {
            Common::writeFailure(response, error.httpStatus, error.code, error.message, traceId);
        }

        /**
         * @brief 认证失败统一回 401
         */
        void writeUnauthorized(AsynGyanis::Net::HttpResponse &response, const std::string_view traceId)
        {
            Common::writeFailure(response, Common::kCodeUnauthorized, Common::kCodeUnauthorized, "未登录或会话已过期", traceId);
        }
    } // namespace

    void registerRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, AuthService &authService, CaptchaService &captchaService, const bool exposeCaptchaCode)
    {
        router.get("/api/auth/captcha",
                   [&captchaService, &loop, exposeCaptchaCode](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId   = Web::resolveRequestId(request);
                       const auto        challenge = co_await captchaService.issue(loop, request.remoteIp());
                       if (!challenge.has_value())
                       {
                           // 限流是唯一可恢复失败；失败文案里不解释内部计数
                           Common::writeFailure(response, Common::kCodeTooManyRequests, Common::kCodeTooManyRequests, challenge.error().message, traceId);
                           co_return;
                       }
                       AsynGyanis::Base::ConfigValue data = AsynGyanis::Base::ConfigValue::object({{"uuid", challenge->uuid}, {"imageSvg", challenge->svg}});
                       // 只有开发档把答案交给调用方；生产档必须从响应里拿不到
                       if (exposeCaptchaCode)
                       {
                           data["code"] = challenge->code;
                       }
                       Common::writeSuccess(response, std::move(data), traceId);
                       co_return;
                   });

        router.post("/api/auth/login",
                    [&authService, &loop](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string                                  traceId = Web::resolveRequestId(request);
                        const std::optional<AsynGyanis::Base::ConfigValue> body    = request.jsonBody();
                        if (!body.has_value())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                            co_return;
                        }
                        std::optional<std::string> username   = stringField(*body, "username");
                        std::optional<std::string> password   = stringField(*body, "password");
                        std::optional<std::string> captcha    = stringField(*body, "captcha");
                        std::optional<std::string> captchaId  = stringField(*body, "captchaUuid");
                        const std::string          tenantCode = stringField(*body, "tenantCode").value_or("default");
                        // 缺字段是调用方错误：一次性说清缺哪几项，不把空串当有效输入
                        if (!username || !password || !captcha || !captchaId)
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少 username、password、captcha 或 captchaUuid", traceId);
                            co_return;
                        }
                        const auto result = co_await authService.login(loop, LoginCredentials{*username, *password, *captcha, *captchaId, request.remoteIp(), traceId, tenantCode});
                        if (!result.has_value())
                        {
                            writeAuthError(response, result.error(), traceId);
                            co_return;
                        }
                        AsynGyanis::Base::ConfigValue data = AsynGyanis::Base::ConfigValue::object({{"token", result->token}, {"expiresIn", result->expiresIn}});
                        Common::writeSuccess(response, std::move(data), traceId);
                        co_return;
                    });

        router.post("/api/auth/logout",
                    [&authService, &loop](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string                traceId = Web::resolveRequestId(request);
                        const std::optional<std::string> token   = Web::bearerToken(request);
                        if (!token.has_value() || !(co_await authService.authenticate(loop, *token)).has_value())
                        {
                            writeUnauthorized(response, traceId);
                            co_return;
                        }
                        static_cast<void>(co_await authService.logout(loop, *token));
                        Common::writeSuccess(response, nullptr, traceId);
                        co_return;
                    });

        router.get("/api/auth/me",
                   [&authService, &loop](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string                traceId = Web::resolveRequestId(request);
                       const std::optional<std::string> token   = Web::bearerToken(request);
                       std::optional<SessionRecord>     session;
                       if (token.has_value())
                       {
                           session = co_await authService.authenticate(loop, *token);
                       }
                       if (!session.has_value())
                       {
                           writeUnauthorized(response, traceId);
                           co_return;
                       }
                       AsynGyanis::Base::ConfigValue data = AsynGyanis::Base::ConfigValue::object({{"id", session->userId},
                                                                                                   {"userId", session->userId},
                                                                                                   {"username", session->username},
                                                                                                   {"nickname", session->nickname},
                                                                                                   {"deptId", session->departmentId},
                                                                                                   {"deptName", session->departmentName},
                                                                                                   {"tenantId", session->tenantId},
                                                                                                   {"tenantCode", session->tenantCode},
                                                                                                   {"tenantName", session->tenantName},
                                                                                                   {"platformAdmin", session->platformAdmin},
                                                                                                   {"roles", toJsonArray(session->roles)},
                                                                                                   {"permissions", toJsonArray(session->permissions)}});
                       Common::writeSuccess(response, std::move(data), traceId);
                       co_return;
                   });

        router.get("/api/auth/routes",
                   [&authService, &loop](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string                traceId = Web::resolveRequestId(request);
                       const std::optional<std::string> token   = Web::bearerToken(request);
                       std::optional<SessionRecord>     session;
                       if (token.has_value())
                       {
                           session = co_await authService.authenticate(loop, *token);
                       }
                       if (!session.has_value())
                       {
                           writeUnauthorized(response, traceId);
                           co_return;
                       }
                       const std::vector<System::MenuNode> tree   = co_await authService.routes(loop, *session);
                       AsynGyanis::Base::ConfigValue       routes = AsynGyanis::Base::ConfigValue::array();
                       for (const System::MenuNode &node: tree)
                       {
                           routes.push_back(menuToJson(node));
                       }
                       Common::writeSuccess(response, AsynGyanis::Base::ConfigValue::object({{"routes", std::move(routes)}}), traceId);
                       co_return;
                   });

        router.put("/api/auth/password",
                   [&authService, &loop](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string                traceId = Web::resolveRequestId(request);
                       const std::optional<std::string> token   = Web::bearerToken(request);
                       std::optional<SessionRecord>     session;
                       if (token.has_value())
                       {
                           session = co_await authService.authenticate(loop, *token);
                       }
                       if (!session.has_value())
                       {
                           writeUnauthorized(response, traceId);
                           co_return;
                       }
                       const std::optional<AsynGyanis::Base::ConfigValue> body = request.jsonBody();
                       if (!body.has_value())
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                           co_return;
                       }
                       const std::optional<std::string> oldPassword = stringField(*body, "oldPassword");
                       const std::optional<std::string> newPassword = stringField(*body, "newPassword");
                       if (!oldPassword || !newPassword)
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少 oldPassword 或 newPassword", traceId);
                           co_return;
                       }
                       const auto result = co_await authService.changePassword(loop, *session, *oldPassword, *newPassword);
                       if (!result.has_value())
                       {
                           writeAuthError(response, result.error(), traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });
    }
} // namespace EdgePulse::Auth
