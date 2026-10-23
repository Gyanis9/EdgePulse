#include "Plugins/DevicePlugin/DeviceController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/Pagination.h"
#include "Base/Api/QueryParameters.h"
#include "Plugins/DevicePlugin/NodeJson.h"
#include "Core/Device/NodeService.h"
#include "Core/Auth/DataScopeService.h"
#include "Core/System/TenantRepository.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "Core/Http/RequestId.h"

#include <Core/EventLoop/EventLoop.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::Device
{
    namespace
    {
        constexpr std::string_view kListPermission  = "device:node:list";
        constexpr std::string_view kWritePermission = "device:node:write";

        [[nodiscard]] bool hasPermission(const AccessContext &access, const std::string_view permission)
        {
            return std::find(access.permissions.begin(), access.permissions.end(), permission) != access.permissions.end();
        }

        [[nodiscard]] std::optional<std::int64_t> parseIdentifier(const std::optional<std::string> &text)
        {
            if (!text.has_value() || text->empty())
            {
                return std::nullopt;
            }
            std::int64_t value      = 0;
            const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
            if (error != std::errc{} || end != text->data() + text->size() || value <= 0)
            {
                return std::nullopt;
            }
            return value;
        }

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
         * @brief 把 labels 字段归一成 JSON 文本
         * @return 字段不存在时为空；null 返回空串；其余返回紧凑 JSON
         */
        [[nodiscard]] std::optional<std::string> labelsField(const AsynGyanis::Base::ConfigValue &body)
        {
            if (!body.is_object())
            {
                return std::nullopt;
            }
            const auto iterator = body.find("labels");
            if (iterator == body.end() || iterator->is_null())
            {
                return std::string{};
            }
            if (iterator->is_object())
            {
                return iterator->dump();
            }
            if (iterator->is_string())
            {
                return iterator->get<std::string>();
            }
            return std::nullopt;
        }

        void writeNodeError(AsynGyanis::Net::HttpResponse &response, const NodeServiceError &error, const std::string_view traceId)
        {
            switch (error.reason)
            {
                case NodeServiceError::Reason::Invalid:
                    Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, error.message, traceId);
                    break;
                case NodeServiceError::Reason::NotFound:
                    Common::writeFailure(response, Common::kCodeNotFound, Common::kCodeNotFound, error.message, traceId);
                    break;
                case NodeServiceError::Reason::Conflict:
                    Common::writeFailure(response, Common::kCodeConflict, Common::kCodeConflict, error.message, traceId);
                    break;
            }
        }

        /**
         * @brief 解析身份与权限；失败时已经写好响应，调用方直接返回
         */
        [[nodiscard]] AsynGyanis::Core::Task<std::optional<AccessContext>> requireAccess(const AccessResolver &resolver, AsynGyanis::Core::EventLoop &loop,
                                                                                         AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response,
                                                                                         const std::string_view permission, const std::string_view traceId)
        {
            const std::optional<AccessContext> access = co_await resolver(loop, request);
            // 未认证与无权限是两件事：401 让前端回登录页，403 让前端提示无权访问
            if (!access.has_value())
            {
                Common::writeFailure(response, Common::kCodeUnauthorized, Common::kCodeUnauthorized, "未登录或会话已过期", traceId);
                co_return std::nullopt;
            }
            if (!hasPermission(*access, permission))
            {
                Common::writeFailure(response, Common::kCodeForbidden, Common::kCodeForbidden, "无权执行该操作", traceId);
                co_return std::nullopt;
            }
            co_return access;
        }

        [[nodiscard]] AsynGyanis::Core::Task<Auth::DataScope> resolveScope(Auth::DataScopeService *dataScopes, AsynGyanis::Core::EventLoop &loop, const AccessContext &access)
        {
            if (dataScopes == nullptr)
            {
                co_return Auth::DataScope{true, false, access.userId, {}};
            }
            co_return co_await dataScopes->resolve(loop, access.userId);
        }

        [[nodiscard]] bool nodeInScope(const NodeRecord &node, const Auth::DataScope &scope)
        {
            if (scope.all)
            {
                return true;
            }
            if (scope.selfOnly)
            {
                return node.createdBy == scope.userId;
            }
            return std::find(scope.deptIds.begin(), scope.deptIds.end(), node.ownerDeptId) != scope.deptIds.end();
        }
    } // namespace

    void registerRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, NodeService &nodeService, AccessResolver accessResolver,
                        Auth::DataScopeService *dataScopes, System::TenantRepository *tenants)
    {
        router.get("/api/nodes",
                   [&loop, &nodeService, accessResolver, dataScopes](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        access  = co_await requireAccess(accessResolver, loop, request, response, kListPermission, traceId);
                       if (!access.has_value())
                       {
                           co_return;
                       }
                       const Auth::DataScope                              scope = co_await resolveScope(dataScopes, loop, *access);
                       const NodeListScope                                nodeScope{scope.all, scope.deptIds, scope.selfOnly, scope.userId};
                       const auto                                         service = nodeService.scoped({access->tenantId, access->platformAdmin});
                       const std::unordered_map<std::string, std::string> query   = request.queryParams();
                       const std::string                                  keyword = query.contains("keyword") ? query.at("keyword") : std::string{};
                       const std::string                                  status  = query.contains("status") ? query.at("status") : std::string{};
                       const std::int64_t                                 groupId = Common::queryInteger(query, "groupId").value_or(0);
                       const NodePage                page = co_await service->list(loop, Common::parsePageParameters(query), keyword, status, groupId, nodeScope);
                       AsynGyanis::Base::ConfigValue rows = AsynGyanis::Base::ConfigValue::array();
                       for (const NodeRecord &node: page.rows)
                       {
                           rows.push_back(nodeToJson(node));
                       }
                       const Common::PageParameters parameters = Common::parsePageParameters(query);
                       Common::writeSuccess(response, Common::makePageResult(parameters, page.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.post("/api/nodes",
                    [&loop, &nodeService, accessResolver, tenants](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        access  = co_await requireAccess(accessResolver, loop, request, response, kWritePermission, traceId);
                        if (!access.has_value())
                        {
                            co_return;
                        }
                        const std::optional<AsynGyanis::Base::ConfigValue> body = request.jsonBody();
                        if (!body.has_value())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                            co_return;
                        }
                        const std::optional<std::string> name   = stringField(*body, "name");
                        const std::optional<std::string> labels = labelsField(*body);
                        if (!name.has_value() || !labels.has_value())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少 name，或 labels 既不是对象也不是字符串", traceId);
                            co_return;
                        }
                        System::TenantScope tenantScope{access->tenantId, access->platformAdmin};
                        if (access->platformAdmin && body->contains("tenantId"))
                        {
                            const auto tenantId = body->find("tenantId");
                            if (tenantId == body->end() || !tenantId->is_number_integer() || tenantId->get<std::int64_t>() <= 0)
                            {
                                Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "tenantId 不合法", traceId);
                                co_return;
                            }
                            const auto tenant = tenants != nullptr ? co_await tenants->findById(loop, tenantId->get<std::int64_t>()) : std::nullopt;
                            if (!tenant.has_value() || tenant->status != 1)
                            {
                                Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "目标租户不存在或已停用", traceId);
                                co_return;
                            }
                            tenantScope = System::TenantScope{tenant->id, false};
                        }
                        const auto         service = nodeService.scoped(tenantScope);
                        const std::int64_t groupId = body->contains("groupId") ? body->value("groupId", std::int64_t{200}) : 200;
                        const auto         result  = co_await service->create(loop, *name, *labels, groupId, access->departmentId, access->userId);
                        if (!result.has_value())
                        {
                            writeNodeError(response, result.error(), traceId);
                            co_return;
                        }
                        Common::writeSuccess(response, nodeToJson(*result), traceId);
                        co_return;
                    });

        router.put("/api/nodes/:id",
                   [&loop, &nodeService, accessResolver, dataScopes](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        access  = co_await requireAccess(accessResolver, loop, request, response, kWritePermission, traceId);
                       if (!access.has_value())
                       {
                           co_return;
                       }
                       const auto                        service = nodeService.scoped({access->tenantId, access->platformAdmin});
                       const std::optional<std::int64_t> nodeId  = parseIdentifier(request.param("id"));
                       if (!nodeId.has_value())
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "节点标识不合法", traceId);
                           co_return;
                       }
                       const Auth::DataScope scope    = co_await resolveScope(dataScopes, loop, *access);
                       const auto            existing = co_await service->find(loop, *nodeId);
                       if (!existing.has_value() || !nodeInScope(*existing, scope))
                       {
                           Common::writeFailure(response, Common::kCodeNotFound, Common::kCodeNotFound, "节点不存在或已删除", traceId);
                           co_return;
                       }
                       const std::optional<AsynGyanis::Base::ConfigValue> body = request.jsonBody();
                       if (!body.has_value())
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                           co_return;
                       }
                       const auto versionIterator = body->is_object() ? body->find("version") : body->end();
                       if (versionIterator == body->end() || !versionIterator->is_number_integer())
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少整数 version", traceId);
                           co_return;
                       }
                       const std::int64_t          version = versionIterator->get<std::int64_t>();
                       std::optional<std::string>  name    = stringField(*body, "name");
                       std::optional<std::int64_t> groupId;
                       if (body->is_object() && body->contains("groupId") && (*body)["groupId"].is_number_integer())
                       {
                           groupId = (*body)["groupId"].get<std::int64_t>();
                       }
                       std::optional<std::string> labels;
                       if (body->is_object() && body->contains("labels"))
                       {
                           labels = labelsField(*body);
                           // 字段存在但既不是对象也不是字符串：不能当成“没改”继续更新
                           if (!labels.has_value())
                           {
                               Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "labels 必须是对象或 JSON 字符串", traceId);
                               co_return;
                           }
                       }
                       if (!name.has_value() && !labels.has_value() && !groupId.has_value())
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "name、labels 与 groupId 至少给一个", traceId);
                           co_return;
                       }
                       const auto result = co_await service->update(loop, *nodeId, std::move(name), labels, groupId, version);
                       if (!result.has_value())
                       {
                           writeNodeError(response, result.error(), traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nodeToJson(*result), traceId);
                       co_return;
                   });

        router.del("/api/nodes/:id",
                   [&loop, &nodeService, accessResolver, dataScopes](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        access  = co_await requireAccess(accessResolver, loop, request, response, kWritePermission, traceId);
                       if (!access.has_value())
                       {
                           co_return;
                       }
                       const auto                        service = nodeService.scoped({access->tenantId, access->platformAdmin});
                       const std::optional<std::int64_t> nodeId  = parseIdentifier(request.param("id"));
                       if (!nodeId.has_value())
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "节点标识不合法", traceId);
                           co_return;
                       }
                       const Auth::DataScope scope    = co_await resolveScope(dataScopes, loop, *access);
                       const auto            existing = co_await service->find(loop, *nodeId);
                       if (!existing.has_value() || !nodeInScope(*existing, scope))
                       {
                           Common::writeFailure(response, Common::kCodeNotFound, Common::kCodeNotFound, "节点不存在或已删除", traceId);
                           co_return;
                       }
                       if (!co_await service->remove(loop, *nodeId))
                       {
                           Common::writeFailure(response, Common::kCodeNotFound, Common::kCodeNotFound, "节点不存在或已删除", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });
    }
} // namespace EdgePulse::Device
