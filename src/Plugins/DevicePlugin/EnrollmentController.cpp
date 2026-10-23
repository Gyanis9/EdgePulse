#include "Plugins/DevicePlugin/EnrollmentController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/QueryParameters.h"
#include "Base/Api/RequestParsing.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Auth/AuthService.h"
#include "Core/Device/EnrollmentService.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Core/System/TenantRepository.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Device
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        void fail(AsynGyanis::Net::HttpResponse &response, const int status, const std::string &message, const std::string_view traceId)
        {
            Common::writeFailure(response, status, status, message, traceId);
        }

        [[nodiscard]] bool validStatus(const std::string &status)
        {
            return status.empty() || status == "active" || status == "used" || status == "expired" || status == "revoked";
        }

        /**
         * @brief 由使用、吊销与到期三个时间派生展示状态
         * @param record 令牌记录
         * @param now 当前 UTC 时间，同一次响应内复用
         * @return active / used / expired / revoked
         */
        [[nodiscard]] std::string statusOf(const EnrollmentTokenRecord &record, const std::string &now)
        {
            // 吊销优先于使用：管理员主动作废的令牌按作废归档，不再回看它是否曾被消费
            if (!record.revokedAt.empty())
            {
                return "revoked";
            }
            if (!record.usedAt.empty())
            {
                return "used";
            }
            if (!record.expiresAt.empty() && record.expiresAt <= now)
            {
                return "expired";
            }
            return "active";
        }

        [[nodiscard]] ConfigValue tokenJson(const EnrollmentTokenRecord &record, const std::string &now)
        {
            return ConfigValue::object({{"id", record.id},
                                        {"tenantId", record.tenantId},
                                        {"nodeName", record.nodeName},
                                        {"action", record.action},
                                        {"expiresAt", record.expiresAt},
                                        {"usedAt", record.usedAt},
                                        {"revokedAt", record.revokedAt},
                                        {"createdBy", record.createdBy},
                                        {"createdAt", record.createdAt},
                                        {"status", statusOf(record, now)}});
        }
    } // namespace

    void registerEnrollmentRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const EnrollmentDependencies &dependencyInput)
    {
        const auto dependencies = std::make_shared<EnrollmentDependencies>(dependencyInput);

        router.get("/api/enrollment-tokens",
                   [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*dependencies->auth, loop, request, response, "device:node:write", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const std::string status = Common::queryText(request.queryParams(), "status");
                       if (!validStatus(status))
                       {
                           fail(response, Common::kCodeBadRequest, "status 只能是 active、used、expired 或 revoked", traceId);
                           co_return;
                       }
                       const auto        service = dependencies->enrollments->scoped(session->tenantScope());
                       const std::string now     = Common::nowDateTimeUtc();
                       ConfigValue       rows    = ConfigValue::array();
                       for (const EnrollmentTokenRecord &record: co_await service->list(loop, status))
                       {
                           rows.push_back(tokenJson(record, now));
                       }
                       Common::writeSuccess(response, std::move(rows), traceId);
                       co_return;
                   });

        router.post("/api/enrollment-tokens",
                    [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(*dependencies->auth, loop, request, response, "device:node:write", traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const auto body = request.jsonBody();
                        if (!body.has_value() || !body->is_object())
                        {
                            fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                            co_return;
                        }
                        // 可选字段一旦出现就必须是对应类型，不能把写错的类型当成缺省值
                        std::string nodeName;
                        if (body->contains("nodeName"))
                        {
                            const auto iterator = body->find("nodeName");
                            if (iterator == body->end() || !iterator->is_string())
                            {
                                fail(response, Common::kCodeBadRequest, "nodeName 必须是字符串", traceId);
                                co_return;
                            }
                            nodeName = iterator->get<std::string>();
                        }
                        std::string action = "enroll";
                        if (body->contains("action"))
                        {
                            const auto iterator = body->find("action");
                            if (iterator == body->end() || !iterator->is_string())
                            {
                                fail(response, Common::kCodeBadRequest, "action 必须是字符串", traceId);
                                co_return;
                            }
                            action = iterator->get<std::string>();
                        }
                        std::int64_t expiresInSeconds = 3600;
                        if (body->contains("expiresInSeconds"))
                        {
                            const auto iterator = body->find("expiresInSeconds");
                            if (iterator == body->end() || !iterator->is_number_integer())
                            {
                                fail(response, Common::kCodeBadRequest, "expiresInSeconds 必须是整数", traceId);
                                co_return;
                            }
                            expiresInSeconds = iterator->get<std::int64_t>();
                        }
                        // 先按 int 能安全承载的范围校验，再交给服务做业务范围判断
                        if (expiresInSeconds < kEnrollmentMinimumExpirySeconds || expiresInSeconds > kEnrollmentMaximumExpirySeconds)
                        {
                            fail(response, Common::kCodeBadRequest, "有效期必须在 300 到 2592000 秒之间", traceId);
                            co_return;
                        }
                        std::int64_t targetTenant   = session->tenantId;
                        bool         explicitTenant = false;
                        if (body->contains("tenantId"))
                        {
                            const auto iterator = body->find("tenantId");
                            if (iterator == body->end() || !iterator->is_number_integer() || iterator->get<std::int64_t>() <= 0)
                            {
                                fail(response, Common::kCodeBadRequest, "tenantId 不合法", traceId);
                                co_return;
                            }
                            targetTenant   = iterator->get<std::int64_t>();
                            explicitTenant = true;
                        }
                        if (!session->platformAdmin)
                        {
                            // 租户管理员只能为本租户签发；显式指定别家租户按越权处理
                            if (explicitTenant && targetTenant != session->tenantId)
                            {
                                fail(response, Common::kCodeForbidden, "只能为本租户签发注册令牌", traceId);
                                co_return;
                            }
                            targetTenant = session->tenantId;
                        } else if (explicitTenant)
                        {
                            const auto tenant = co_await dependencies->tenants->findById(loop, targetTenant);
                            if (!tenant.has_value() || tenant->status != 1)
                            {
                                fail(response, Common::kCodeBadRequest, "目标租户不存在或已停用", traceId);
                                co_return;
                            }
                        }
                        const auto service = dependencies->enrollments->scoped(System::TenantScope{targetTenant, false});
                        const auto issued =
                                co_await service->create(loop, targetTenant, std::move(nodeName), std::move(action), static_cast<int>(expiresInSeconds), session->userId);
                        if (!issued.has_value())
                        {
                            fail(response, Common::kCodeBadRequest, issued.error(), traceId);
                            co_return;
                        }
                        // 明文只在这里出现一次；记录列表永远不带明文或哈希
                        ConfigValue payload = tokenJson(issued->record, Common::nowDateTimeUtc());
                        payload["token"]    = issued->plaintext;
                        Common::writeSuccess(response, std::move(payload), traceId);
                        co_return;
                    });

        router.del("/api/enrollment-tokens/:id",
                   [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*dependencies->auth, loop, request, response, "device:node:write", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const std::optional<std::int64_t> tokenId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!tokenId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "令牌标识不合法", traceId);
                           co_return;
                       }
                       const auto service = dependencies->enrollments->scoped(session->tenantScope());
                       if (!co_await service->revoke(loop, *tokenId))
                       {
                           fail(response, Common::kCodeNotFound, "令牌不存在、已使用或已吊销", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });
    }
} // namespace EdgePulse::Device
