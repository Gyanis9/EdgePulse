#include "Plugins/DevicePlugin/NodeGroupController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/RequestParsing.h"
#include "Base/Json/JsonFields.h"
#include "Core/Auth/AuthService.h"
#include "Core/Device/NodeGroupRepository.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace EdgePulse::Device
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        [[nodiscard]] ConfigValue groupJson(const NodeGroupRecord &group)
        {
            ConfigValue children = ConfigValue::array();
            for (const NodeGroupRecord &child: group.children)
            {
                children.push_back(groupJson(child));
            }
            return ConfigValue::object({{"id", group.id},
                                        {"parentId", group.parentId},
                                        {"name", group.name},
                                        {"sort", group.sort},
                                        {"remark", group.remark},
                                        {"nodeCount", group.nodeCount},
                                        {"createTime", group.createTime},
                                        {"children", std::move(children)}});
        }

        void fail(AsynGyanis::Net::HttpResponse &response, const int status, const std::string &message, const std::string_view traceId)
        {
            Common::writeFailure(response, status, status, message, traceId);
        }
    } // namespace

    void registerNodeGroupRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const NodeGroupDependencies &dependencyInput)
    {
        const auto deps = std::make_shared<NodeGroupDependencies>(dependencyInput);

        router.get("/api/node-groups",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:node:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto  tree = co_await deps->groups->scoped(session->tenantScope())->tree(loop);
                       ConfigValue rows = ConfigValue::array();
                       for (const NodeGroupRecord &group: tree)
                       {
                           rows.push_back(groupJson(group));
                       }
                       Common::writeSuccess(response, std::move(rows), traceId);
                       co_return;
                   });

        router.post("/api/node-groups",
                    [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:node:write", traceId);
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
                        const std::string name = Json::stringField(*body, "name");
                        if (name.empty() || name.size() > 64)
                        {
                            fail(response, Common::kCodeBadRequest, "分组名称长度必须是 1 到 64 个字符", traceId);
                            co_return;
                        }
                        const std::int64_t  parentId = Json::integerField(*body, "parentId", 0);
                        System::TenantScope scope    = session->tenantScope();
                        if (session->platformAdmin)
                        {
                            // 平台管理员建组必须落在具体租户下，缺省为默认租户
                            const auto requestedTenant = Json::optionalInteger(*body, "tenantId");
                            scope                      = System::TenantScope{requestedTenant.value_or(session->tenantId), false};
                        }
                        const auto groups = deps->groups->scoped(scope);
                        if (parentId != 0 && !(co_await groups->exists(loop, parentId)))
                        {
                            fail(response, Common::kCodeBadRequest, "上级分组不存在", traceId);
                            co_return;
                        }
                        const std::int64_t id = co_await groups->create(loop, parentId, name, static_cast<int>(Json::integerField(*body, "sort", 0)),
                                                                        Json::stringField(*body, "remark"), session->userId);
                        Common::writeSuccess(response, ConfigValue::object({{"id", id}}), traceId);
                        co_return;
                    });

        router.put("/api/node-groups/:id",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:node:write", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto groupId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!groupId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "分组标识不合法", traceId);
                           co_return;
                       }
                       const auto body = request.jsonBody();
                       if (!body.has_value() || !body->is_object())
                       {
                           fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                           co_return;
                       }
                       std::optional<std::int64_t> parentId = Json::optionalInteger(*body, "parentId");
                       const auto                  groups   = deps->groups->scoped(session->tenantScope());
                       if (parentId.has_value() && (*parentId == *groupId || (*parentId != 0 && !(co_await groups->exists(loop, *parentId)))))
                       {
                           fail(response, Common::kCodeBadRequest, "上级分组不合法", traceId);
                           co_return;
                       }
                       std::optional<std::string> name;
                       if (body->contains("name"))
                       {
                           name = Json::stringField(*body, "name");
                       }
                       std::optional<int> sort;
                       if (const auto value = Json::optionalInteger(*body, "sort"); value.has_value())
                       {
                           sort = static_cast<int>(*value);
                       }
                       std::optional<std::string> remark;
                       if (body->contains("remark"))
                       {
                           remark = Json::stringField(*body, "remark");
                       }
                       if (!co_await groups->update(loop, *groupId, parentId, std::move(name), sort, std::move(remark), session->userId))
                       {
                           fail(response, Common::kCodeNotFound, "分组不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.del("/api/node-groups/:id",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:node:write", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto groupId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!groupId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "分组标识不合法", traceId);
                           co_return;
                       }
                       if (co_await deps->groups->scoped(session->tenantScope())->isInUse(loop, *groupId))
                       {
                           fail(response, Common::kCodeConflict, "分组下仍有子分组或节点，不能删除", traceId);
                           co_return;
                       }
                       if (!co_await deps->groups->scoped(session->tenantScope())->remove(loop, *groupId))
                       {
                           fail(response, Common::kCodeNotFound, "分组不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });
    }
} // namespace EdgePulse::Device
