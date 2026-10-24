#include "Plugins/AuthPlugin/MfaController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Log/LogMacros.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/MfaService.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Core/System/LogRepository.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <exception>
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

        /**
         * @brief 写一条两步验证管理操作审计
         * @param loop 恢复协程用的事件循环
         * @param logs 操作日志仓储
         * @param session 当前身份
         * @param request 当前请求
         * @param action 动作名，如「启用」
         * @param success 是否成功
         * @param errorMessage 失败原因；成功时为空
         * @note 请求体含验证码与口令，审计不复刻参数，只记用户、IP、动作与结果
         */
        AsynGyanis::Core::Task<> recordMfaAudit(AsynGyanis::Core::EventLoop &loop, EdgePulse::System::LogRepository &logs, const SessionRecord &session,
                                                const AsynGyanis::Net::HttpRequest &request, const std::string_view action, const bool success, std::string errorMessage)
        {
            EdgePulse::System::OperLogRecord record;
            record.module   = "两步验证";
            record.action   = std::string{action};
            record.title    = record.module + " · " + record.action;
            record.method   = "POST";
            record.url      = std::string{request.path()};
            record.ip       = request.remoteIp();
            record.operName = session.username;
            record.deptName = session.departmentName;
            record.operId   = session.userId;
            record.deptId   = session.departmentId;
            record.tenantId = session.tenantId;
            record.params   = "{}";
            record.result   = success ? 1 : 0;
            record.errorMsg = success ? std::string{} : std::move(errorMessage);
            record.traceId  = std::string{request.requestId()};
            try
            {
                if (!co_await logs.insertOperLog(loop, std::move(record)))
                {
                    LOG_ERROR("两步验证操作审计未写入：数据库返回失败");
                }
            } catch (const std::exception &failure)
            {
                // 审计失败不推翻已经完成的管理动作，但必须在服务端日志留痕
                LOG_ERROR_FMT("两步验证操作审计写入异常：{}", failure.what());
            }
            co_return;
        }
    } // namespace

    void registerMfaRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, AuthService &authService, MfaService &mfaService,
                           EdgePulse::System::LogRepository &logs)
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
                    [&loop, &authService, &mfaService, &logs](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(authService, loop, request, response, "", traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const std::optional<AsynGyanis::Base::ConfigValue> body     = request.jsonBody();
                        const std::optional<std::string>                   password = body.has_value() ? stringField(*body, "password") : std::nullopt;
                        if (!password.has_value() || password->empty())
                        {
                            co_await recordMfaAudit(loop, logs, *session, request, "开始设置", false, "缺少 password");
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少 password", traceId);
                            co_return;
                        }
                        // 只持有会话不足以登记新的第二因素：先复核当前口令
                        if (!co_await authService.verifyPassword(loop, session->userId, *password))
                        {
                            co_await recordMfaAudit(loop, logs, *session, request, "开始设置", false, "密码不正确");
                            Common::writeFailure(response, Common::kCodeUnauthorized, Common::kCodeUnauthorized, "密码不正确，无法开始设置两步验证", traceId);
                            co_return;
                        }
                        const auto setup = co_await mfaService.beginSetup(loop, *session);
                        if (!setup.has_value())
                        {
                            co_await recordMfaAudit(loop, logs, *session, request, "开始设置", false, setup.error().message);
                            writeMfaError(response, setup.error(), traceId);
                            co_return;
                        }
                        co_await recordMfaAudit(loop, logs, *session, request, "开始设置", true, {});
                        markNoStore(response);
                        Common::writeSuccess(response, AsynGyanis::Base::ConfigValue::object({{"secret", setup->secret}, {"otpauthUri", setup->otpauthUri}}), traceId);
                        co_return;
                    });

        router.post("/api/auth/mfa/enable",
                    [&loop, &authService, &mfaService, &logs](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
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
                            co_await recordMfaAudit(loop, logs, *session, request, "启用", false, "缺少 password 或 code");
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少 password 或 code", traceId);
                            co_return;
                        }
                        // 启用同样要求当前口令，防止被盗会话自行登记攻击者的验证器
                        if (!co_await authService.verifyPassword(loop, session->userId, *password))
                        {
                            co_await recordMfaAudit(loop, logs, *session, request, "启用", false, "密码不正确");
                            Common::writeFailure(response, Common::kCodeUnauthorized, Common::kCodeUnauthorized, "密码不正确，无法启用两步验证", traceId);
                            co_return;
                        }
                        const auto recoveryCodes = co_await mfaService.enable(loop, *session, *code);
                        if (!recoveryCodes.has_value())
                        {
                            co_await recordMfaAudit(loop, logs, *session, request, "启用", false, recoveryCodes.error().message);
                            writeMfaError(response, recoveryCodes.error(), traceId);
                            co_return;
                        }
                        co_await recordMfaAudit(loop, logs, *session, request, "启用", true, {});
                        markNoStore(response);
                        Common::writeSuccess(response, AsynGyanis::Base::ConfigValue::object({{"recoveryCodes", toJsonArray(*recoveryCodes)}}), traceId);
                        co_return;
                    });

        router.post("/api/auth/mfa/disable",
                    [&loop, &authService, &mfaService, &logs](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
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
                            co_await recordMfaAudit(loop, logs, *session, request, "关闭", false, "缺少 password 或 code");
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少 password 或 code", traceId);
                            co_return;
                        }
                        // 关闭是降低账号保护强度的操作：先确认当前口令，再校验未消费过的 TOTP
                        if (!co_await authService.verifyPassword(loop, session->userId, *password))
                        {
                            co_await recordMfaAudit(loop, logs, *session, request, "关闭", false, "密码不正确");
                            Common::writeFailure(response, Common::kCodeUnauthorized, Common::kCodeUnauthorized, "密码不正确，无法关闭两步验证", traceId);
                            co_return;
                        }
                        const auto disabled = co_await mfaService.disable(loop, *session, *code);
                        if (!disabled.has_value())
                        {
                            co_await recordMfaAudit(loop, logs, *session, request, "关闭", false, disabled.error().message);
                            writeMfaError(response, disabled.error(), traceId);
                            co_return;
                        }
                        co_await recordMfaAudit(loop, logs, *session, request, "关闭", true, {});
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
                    [&loop, &authService, &mfaService, &logs](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
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
                            co_await recordMfaAudit(loop, logs, *session, request, "重置恢复码", false, "缺少验证码");
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少验证码", traceId);
                            co_return;
                        }
                        const auto recoveryCodes = co_await mfaService.regenerateRecoveryCodes(loop, *session, *code);
                        if (!recoveryCodes.has_value())
                        {
                            co_await recordMfaAudit(loop, logs, *session, request, "重置恢复码", false, recoveryCodes.error().message);
                            writeMfaError(response, recoveryCodes.error(), traceId);
                            co_return;
                        }
                        co_await recordMfaAudit(loop, logs, *session, request, "重置恢复码", true, {});
                        markNoStore(response);
                        Common::writeSuccess(response, AsynGyanis::Base::ConfigValue::object({{"recoveryCodes", toJsonArray(*recoveryCodes)}}), traceId);
                        co_return;
                    });
    }
} // namespace EdgePulse::Auth
