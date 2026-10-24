#include "Plugins/AuthPlugin/MfaController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/MfaService.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
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
         * @brief 写两步验证的可恢复错误
         */
        void writeMfaError(AsynGyanis::Net::HttpResponse &response, const MfaError &error, const std::string_view traceId)
        {
            Common::writeFailure(response, error.httpStatus, error.httpStatus, error.message, traceId);
        }

        /**
         * @brief 禁止中间层缓存含秘密或恢复码的响应
         */
        void markNoStore(AsynGyanis::Net::HttpResponse &response)
        {
            response.setHeader("Cache-Control", "no-store");
        }
    } // namespace

    void registerMfaRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, AuthService &authService, MfaService &mfaService)
    {
        router.get("/api/auth/mfa",
                   [&loop, &authService, &mfaService](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authService, loop, request, response, "", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const MfaStatus status = co_await mfaService.status(loop, session->userId);
                       markNoStore(response);
                       Common::writeSuccess(response,
                                            AsynGyanis::Base::ConfigValue::object(
                                                    {{"enabled", status.enabled}, {"recoveryCodesRemaining", static_cast<std::int64_t>(status.remainingRecoveryCodes)}}),
                                            traceId);
                       co_return;
                   });

        router.post("/api/auth/mfa/setup",
                    [&loop, &authService, &mfaService](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(authService, loop, request, response, "", traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const auto setup = co_await mfaService.beginSetup(loop, *session);
                        if (!setup.has_value())
                        {
                            writeMfaError(response, setup.error(), traceId);
                            co_return;
                        }
                        markNoStore(response);
                        Common::writeSuccess(response, AsynGyanis::Base::ConfigValue::object({{"secret", setup->secret}, {"otpauthUri", setup->otpauthUri}}), traceId);
                        co_return;
                    });

        router.post("/api/auth/mfa/enable",
                    [&loop, &authService, &mfaService](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(authService, loop, request, response, "", traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const std::optional<AsynGyanis::Base::ConfigValue> body = request.jsonBody();
                        const std::optional<std::string>                   code = body.has_value() ? stringField(*body, "code") : std::nullopt;
                        if (!code.has_value() || code->empty())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少验证码", traceId);
                            co_return;
                        }
                        const auto recoveryCodes = co_await mfaService.enable(loop, *session, *code);
                        if (!recoveryCodes.has_value())
                        {
                            writeMfaError(response, recoveryCodes.error(), traceId);
                            co_return;
                        }
                        markNoStore(response);
                        Common::writeSuccess(response, AsynGyanis::Base::ConfigValue::object({{"recoveryCodes", toJsonArray(*recoveryCodes)}}), traceId);
                        co_return;
                    });

        router.post("/api/auth/mfa/disable",
                    [&loop, &authService, &mfaService](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(authService, loop, request, response, "", traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const std::optional<AsynGyanis::Base::ConfigValue> body     = request.jsonBody();
                        const std::optional<std::string>                   password = body.has_value() ? stringField(*body, "password") : std::nullopt;
                        const std::optional<std::string>                   code     = body.has_value() ? stringField(*body, "code") : std::nullopt;
                        if (!password.has_value() || password->empty() || !code.has_value() || code->empty())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少 password 或 code", traceId);
                            co_return;
                        }
                        // 关闭是降低账号保护强度的操作：先确认当前口令，再校验未消费过的 TOTP
                        if (!co_await authService.verifyPassword(loop, session->userId, *password))
                        {
                            Common::writeFailure(response, Common::kCodeUnauthorized, Common::kCodeUnauthorized, "密码不正确，无法关闭两步验证", traceId);
                            co_return;
                        }
                        const auto disabled = co_await mfaService.disable(loop, *session, *code);
                        if (!disabled.has_value())
                        {
                            writeMfaError(response, disabled.error(), traceId);
                            co_return;
                        }
                        markNoStore(response);
                        Common::writeSuccess(response, nullptr, traceId);
                        co_return;
                    });

        router.post("/api/auth/mfa/verify",
                    [&loop, &authService](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string                                  traceId        = Web::resolveRequestId(request);
                        const std::optional<AsynGyanis::Base::ConfigValue> body           = request.jsonBody();
                        const std::optional<std::string>                   challengeToken = body.has_value() ? stringField(*body, "challengeToken") : std::nullopt;
                        const std::optional<std::string>                   code           = body.has_value() ? stringField(*body, "code") : std::nullopt;
                        if (!challengeToken.has_value() || challengeToken->empty() || !code.has_value() || code->empty())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少 challengeToken 或 code", traceId);
                            co_return;
                        }
                        const auto result = co_await authService.verifyMfa(loop, *challengeToken, *code, request.remoteIp(), traceId);
                        if (!result.has_value())
                        {
                            Common::writeFailure(response, result.error().httpStatus, result.error().code, result.error().message, traceId);
                            co_return;
                        }
                        markNoStore(response);
                        Common::writeSuccess(response, AsynGyanis::Base::ConfigValue::object({{"token", result->token}, {"expiresIn", result->expiresIn}}), traceId);
                        co_return;
                    });

        router.post("/api/auth/mfa/recovery-codes",
                    [&loop, &authService, &mfaService](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(authService, loop, request, response, "", traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const std::optional<AsynGyanis::Base::ConfigValue> body = request.jsonBody();
                        const std::optional<std::string>                   code = body.has_value() ? stringField(*body, "code") : std::nullopt;
                        if (!code.has_value() || code->empty())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少验证码", traceId);
                            co_return;
                        }
                        const auto recoveryCodes = co_await mfaService.regenerateRecoveryCodes(loop, *session, *code);
                        if (!recoveryCodes.has_value())
                        {
                            writeMfaError(response, recoveryCodes.error(), traceId);
                            co_return;
                        }
                        markNoStore(response);
                        Common::writeSuccess(response, AsynGyanis::Base::ConfigValue::object({{"recoveryCodes", toJsonArray(*recoveryCodes)}}), traceId);
                        co_return;
                    });
    }
} // namespace EdgePulse::Auth
