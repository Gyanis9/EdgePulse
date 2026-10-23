#include "Plugins/SystemPlugin/TenantController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/Pagination.h"
#include "Base/Api/QueryParameters.h"
#include "Base/Api/RequestParsing.h"
#include "Base/Json/JsonFields.h"
#include "Base/Security/PasswordHash.h"
#include "Core/Auth/AuthService.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Core/System/TenantRepository.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace EdgePulse::System
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        [[nodiscard]] ConfigValue tenantJson(const TenantRecord &tenant)
        {
            return ConfigValue::object(
                    {{"id", tenant.id}, {"code", tenant.code}, {"name", tenant.name}, {"status", tenant.status}, {"remark", tenant.remark}, {"createTime", tenant.createTime}});
        }

        void fail(AsynGyanis::Net::HttpResponse &response, const int status, const std::string &message, const std::string_view traceId)
        {
            Common::writeFailure(response, status, status, message, traceId);
        }

        [[nodiscard]] bool isPlatformAdmin(const Auth::SessionRecord &session)
        {
            return session.platformAdmin;
        }
    } // namespace

    void registerTenantRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const TenantDependencies &dependencyInput)
    {
        const auto dependencies = std::make_shared<TenantDependencies>(dependencyInput);
        const auto sessionOf    = [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response, const std::string_view permission,
                                                        const std::string_view traceId) -> AsynGyanis::Core::Task<std::optional<Auth::SessionRecord>>
        {
            std::optional<Auth::SessionRecord> session = co_await Web::requireSession(*dependencies->auth, loop, request, response, permission, traceId);
            if (session.has_value() && !isPlatformAdmin(*session))
            {
                fail(response, Common::kCodeForbidden, "仅平台管理员可以管理租户", traceId);
                session.reset();
            }
            co_return session;
        };

        router.get("/api/system/tenants",
                   [&loop, dependencies, sessionOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await sessionOf(request, response, "system:tenant:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap       query  = request.queryParams();
                       const Common::PageParameters page   = Common::parsePageParameters(query);
                       const std::string            status = Common::queryText(query, "status");
                       if (!status.empty() && status != "0" && status != "1")
                       {
                           fail(response, Common::kCodeBadRequest, "status 只能是 0 或 1", traceId);
                           co_return;
                       }
                       const auto  result = co_await dependencies->tenants->list(loop, Common::queryText(query, "keyword"), status, page);
                       ConfigValue rows   = ConfigValue::array();
                       for (const TenantRecord &tenant: result.rows)
                       {
                           rows.push_back(tenantJson(tenant));
                       }
                       Common::writeSuccess(response, Common::makePageResult(page, result.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.post("/api/system/tenants",
                    [&loop, dependencies, sessionOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await sessionOf(request, response, "system:tenant:add", traceId);
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
                        TenantProvisionRequest provision;
                        provision.code             = Json::stringField(*body, "code");
                        provision.name             = Json::stringField(*body, "name");
                        provision.remark           = Json::stringField(*body, "remark");
                        provision.adminUsername    = Json::stringField(*body, "adminUsername");
                        provision.adminNickname    = Json::stringField(*body, "adminNickname", provision.adminUsername);
                        const std::string password = Json::stringField(*body, "adminPassword");
                        if (provision.code.empty() || provision.name.empty() || provision.adminUsername.empty() || password.size() < 6 || password.size() > 64)
                        {
                            fail(response, Common::kCodeBadRequest, "code、name、adminUsername 不能为空，adminPassword 长度必须在 6 到 64 之间", traceId);
                            co_return;
                        }
                        provision.adminPasswordHash = Common::hashPassword(password);
                        const auto created          = co_await dependencies->tenants->provision(loop, std::move(provision), session->userId);
                        if (!created.has_value())
                        {
                            const int status = created.error().find("已存在") != std::string::npos ? Common::kCodeConflict : Common::kCodeBadRequest;
                            fail(response, status, created.error(), traceId);
                            co_return;
                        }
                        Common::writeSuccess(response, ConfigValue::object({{"tenantId", created->tenantId}, {"adminUserId", created->adminUserId}}), traceId);
                        co_return;
                    });

        router.put("/api/system/tenants/:id",
                   [&loop, dependencies, sessionOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await sessionOf(request, response, "system:tenant:edit", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto id = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "租户标识不合法", traceId);
                           co_return;
                       }
                       const auto body = request.jsonBody();
                       if (!body.has_value() || !body->is_object())
                       {
                           fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                           co_return;
                       }
                       const std::string name   = Json::stringField(*body, "name");
                       const std::string remark = Json::stringField(*body, "remark");
                       if (name.size() > 128)
                       {
                           fail(response, Common::kCodeBadRequest, "租户名称不能超过 128 个字符", traceId);
                           co_return;
                       }
                       const auto status       = Json::optionalInteger(*body, "status");
                       const int  targetStatus = status.has_value() ? static_cast<int>(*status) : -1;
                       if (status.has_value() && *status != 0 && *status != 1)
                       {
                           fail(response, Common::kCodeBadRequest, "status 只能是 0 或 1", traceId);
                           co_return;
                       }
                       if (*id == 1 && targetStatus == 0)
                       {
                           fail(response, Common::kCodeBadRequest, "默认租户不能停用", traceId);
                           co_return;
                       }
                       if (!co_await dependencies->tenants->update(loop, *id, name, targetStatus, remark))
                       {
                           fail(response, Common::kCodeNotFound, "租户不存在", traceId);
                           co_return;
                       }
                       co_await dependencies->auth->invalidateTenant(loop, *id);
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });
    }
} // namespace EdgePulse::System
