#include "Plugins/SystemPlugin/SystemController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/CsvWriter.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/Pagination.h"
#include "Base/Api/QueryParameters.h"
#include "Base/Api/RequestParsing.h"
#include "Base/Database/Database.h"
#include "Base/Exception/EdgePulseException.h"
#include "Base/Json/JsonFields.h"
#include "Base/Runtime/Environment.h"
#include "Base/Security/PasswordHash.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/DataScopeService.h"
#include "Core/Auth/MfaService.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Core/System/DictionaryRepository.h"
#include "Core/System/MenuRepository.h"
#include "Core/System/OrgRepository.h"
#include "Core/System/RoleRepository.h"
#include "Core/System/UserRepository.h"
#include "Core/System/TenantRepository.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <Core/Coroutine/AsyncExecutor.h>
#include <Core/EventLoop/EventLoop.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::System
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;
        using Json::boolField;
        using Json::integerArrayField;
        using Json::optionalInteger;
        using Json::optionalString;
        using Json::stringField;

        [[nodiscard]] std::string scopeLabel(const std::string_view code)
        {
            if (code == "ALL")
            {
                return "全部数据权限";
            }
            if (code == "DEPT_AND_CHILD")
            {
                return "本部门及以下数据权限";
            }
            if (code == "DEPT")
            {
                return "本部门数据权限";
            }
            if (code == "CUSTOM")
            {
                return "自定义数据权限";
            }
            return "仅本人数据权限";
        }

        [[nodiscard]] bool isValidScope(const std::string_view code)
        {
            return code == "ALL" || code == "DEPT_AND_CHILD" || code == "DEPT" || code == "SELF" || code == "CUSTOM";
        }

        [[nodiscard]] bool matchesPattern(const std::string_view value, const bool upperFirst)
        {
            if (value.empty())
            {
                return false;
            }
            const unsigned char first = static_cast<unsigned char>(value.front());
            if (upperFirst ? !std::isalpha(first) : !(first >= 'a' && first <= 'z'))
            {
                return false;
            }
            for (const char character: value)
            {
                const unsigned char code = static_cast<unsigned char>(character);
                if (!(std::isalnum(code) != 0 || character == '_' || character == '-'))
                {
                    return false;
                }
            }
            return true;
        }

        void fail(AsynGyanis::Net::HttpResponse &response, const int status, const std::string &message, const std::string_view traceId)
        {
            Common::writeFailure(response, status, status, message, traceId);
        }

        void writeCsv(AsynGyanis::Net::HttpResponse &response, const Common::CsvWriter &writer, const std::string_view filename, const std::string_view traceId)
        {
            response.setStatus(200);
            response.setHeader("Content-Type", "text/csv; charset=utf-8");
            response.setHeader("Content-Disposition", "attachment; filename=\"" + std::string{filename} + "\"");
            if (!traceId.empty())
            {
                response.setHeader(Common::kRequestIdHeader, traceId);
            }
            response.setOwnedBody(writer.take());
        }

        [[nodiscard]] ConfigValue userJson(const UserRecord &user)
        {
            return ConfigValue::object({{"id", user.id},
                                        {"tenantId", user.tenantId},
                                        {"tenantCode", user.tenantCode},
                                        {"tenantName", user.tenantName},
                                        {"platformAdmin", user.platformAdmin},
                                        {"username", user.username},
                                        {"nickname", user.nickname},
                                        {"phone", user.phone},
                                        {"email", user.email},
                                        {"deptId", user.departmentId},
                                        {"deptName", user.departmentName},
                                        {"postName", user.postNames},
                                        {"roleName", user.roleNames},
                                        {"status", user.status},
                                        {"mfaEnabled", user.mfaEnabled},
                                        {"lockedUntil", user.lockedUntil},
                                        {"createTime", user.createTime},
                                        {"lastLogin", user.lastLoginTime},
                                        {"roleIds", Json::integerArray(user.roleIds)},
                                        {"postIds", Json::integerArray(user.postIds)}});
        }

        [[nodiscard]] ConfigValue roleJson(const RoleRecord &role, const bool labelScope)
        {
            return ConfigValue::object({{"id", role.id},
                                        {"name", role.name},
                                        {"code", role.code},
                                        {"dataScope", labelScope ? scopeLabel(role.dataScope) : role.dataScope},
                                        {"sort", role.sort},
                                        {"status", role.status},
                                        {"builtin", role.builtin},
                                        {"remark", role.remark},
                                        {"userCount", role.userCount},
                                        {"createTime", role.createTime}});
        }

        [[nodiscard]] ConfigValue menuJson(const MenuNode &node)
        {
            ConfigValue value    = ConfigValue::object({{"id", node.id},
                                                        {"parentId", node.parentId},
                                                        {"name", node.name},
                                                        {"type", node.type},
                                                        {"icon", node.icon},
                                                        {"path", node.path},
                                                        {"component", node.component},
                                                        {"permission", node.permission},
                                                        {"sort", node.sort},
                                                        {"visible", node.visible},
                                                        {"status", node.status}});
            ConfigValue children = ConfigValue::array();
            for (const MenuNode &child: node.children)
            {
                children.push_back(menuJson(child));
            }
            value["children"] = std::move(children);
            return value;
        }

        [[nodiscard]] ConfigValue deptJson(const DeptRecord &dept)
        {
            ConfigValue value    = ConfigValue::object({{"id", dept.id},
                                                        {"parentId", dept.parentId},
                                                        {"name", dept.name},
                                                        {"leader", dept.leader},
                                                        {"phone", dept.phone},
                                                        {"email", dept.email},
                                                        {"sort", dept.sort},
                                                        {"status", dept.status},
                                                        {"memberCount", dept.memberCount},
                                                        {"createTime", dept.createTime}});
            ConfigValue children = ConfigValue::array();
            for (const DeptRecord &child: dept.children)
            {
                children.push_back(deptJson(child));
            }
            value["children"] = std::move(children);
            return value;
        }

        [[nodiscard]] ConfigValue postJson(const PostRecord &post)
        {
            return ConfigValue::object({{"id", post.id},
                                        {"postCode", post.code},
                                        {"postName", post.name},
                                        {"sort", post.sort},
                                        {"status", post.status},
                                        {"remark", post.remark},
                                        {"userCount", post.userCount},
                                        {"createTime", post.createTime}});
        }

        [[nodiscard]] ConfigValue dictTypeJson(const DictTypeRecord &record)
        {
            return ConfigValue::object({{"id", record.id},
                                        {"dictName", record.name},
                                        {"dictType", record.type},
                                        {"status", record.status},
                                        {"remark", record.remark},
                                        {"createTime", record.createTime}});
        }

        [[nodiscard]] ConfigValue dictDataJson(const DictDataRecord &record)
        {
            return ConfigValue::object({{"id", record.id},
                                        {"dictType", record.dictType},
                                        {"label", record.label},
                                        {"value", record.value},
                                        {"sort", record.sort},
                                        {"status", record.status},
                                        {"tone", record.tone},
                                        {"def", record.isDefault},
                                        {"remark", record.remark}});
        }

        [[nodiscard]] ConfigValue configJson(const ConfigRecord &record)
        {
            return ConfigValue::object({{"id", record.id},
                                        {"name", record.name},
                                        {"key", record.key},
                                        {"value", record.value},
                                        {"type", record.type},
                                        {"builtin", record.builtin},
                                        {"remark", record.remark},
                                        {"createTime", record.createTime}});
        }

        [[nodiscard]] UserWrite parseUserWrite(const ConfigValue &body)
        {
            UserWrite write;
            write.username = optionalString(body, "username");
            write.nickname = optionalString(body, "nickname");
            write.phone    = optionalString(body, "phone");
            write.email    = optionalString(body, "email");
            if (const auto deptId = optionalInteger(body, "deptId"); deptId.has_value())
            {
                write.deptId = *deptId;
            }
            if (const auto status = optionalInteger(body, "status"); status.has_value())
            {
                write.status = static_cast<int>(*status);
            }
            if (body.contains("roleIds"))
            {
                write.roleIds = integerArrayField(body, "roleIds");
            }
            if (body.contains("postIds"))
            {
                write.postIds = integerArrayField(body, "postIds");
            }
            return write;
        }

        [[nodiscard]] RoleWrite parseRoleWrite(const ConfigValue &body)
        {
            RoleWrite write;
            write.name      = optionalString(body, "name");
            write.code      = optionalString(body, "code");
            write.dataScope = optionalString(body, "dataScope");
            write.remark    = optionalString(body, "remark");
            if (const auto sort = optionalInteger(body, "sort"); sort.has_value())
            {
                write.sort = static_cast<int>(*sort);
            }
            if (const auto status = optionalInteger(body, "status"); status.has_value())
            {
                write.status = static_cast<int>(*status);
            }
            if (body.contains("menuIds"))
            {
                write.menuIds = integerArrayField(body, "menuIds");
            }
            if (body.contains("deptIds"))
            {
                write.deptIds = integerArrayField(body, "deptIds");
            }
            return write;
        }

        [[nodiscard]] MenuWrite parseMenuWrite(const ConfigValue &body)
        {
            MenuWrite write;
            write.name       = optionalString(body, "name");
            write.type       = optionalString(body, "type");
            write.icon       = optionalString(body, "icon");
            write.path       = optionalString(body, "path");
            write.component  = optionalString(body, "component");
            write.permission = optionalString(body, "permission");
            if (const auto parentId = optionalInteger(body, "parentId"); parentId.has_value())
            {
                write.parentId = *parentId;
            }
            for (const auto &[key, target]: {std::pair<std::string_view, std::optional<int> *>{"sort", &write.sort}, {"visible", &write.visible}, {"status", &write.status}})
            {
                if (const auto value = optionalInteger(body, key); value.has_value())
                {
                    *target = static_cast<int>(*value);
                }
            }
            return write;
        }

        [[nodiscard]] DeptWrite parseDeptWrite(const ConfigValue &body)
        {
            DeptWrite write;
            write.name   = optionalString(body, "name");
            write.leader = optionalString(body, "leader");
            write.phone  = optionalString(body, "phone");
            write.email  = optionalString(body, "email");
            if (const auto parentId = optionalInteger(body, "parentId"); parentId.has_value())
            {
                write.parentId = *parentId;
            }
            if (const auto sort = optionalInteger(body, "sort"); sort.has_value())
            {
                write.sort = static_cast<int>(*sort);
            }
            if (const auto status = optionalInteger(body, "status"); status.has_value())
            {
                write.status = static_cast<int>(*status);
            }
            return write;
        }

        [[nodiscard]] PostWrite parsePostWrite(const ConfigValue &body)
        {
            PostWrite write;
            write.code   = optionalString(body, "postCode");
            write.name   = optionalString(body, "postName");
            write.remark = optionalString(body, "remark");
            if (const auto sort = optionalInteger(body, "sort"); sort.has_value())
            {
                write.sort = static_cast<int>(*sort);
            }
            if (const auto status = optionalInteger(body, "status"); status.has_value())
            {
                write.status = static_cast<int>(*status);
            }
            return write;
        }

        [[nodiscard]] DictTypeWrite parseDictTypeWrite(const ConfigValue &body)
        {
            DictTypeWrite write;
            write.name   = optionalString(body, "dictName");
            write.type   = optionalString(body, "dictType");
            write.remark = optionalString(body, "remark");
            if (const auto status = optionalInteger(body, "status"); status.has_value())
            {
                write.status = static_cast<int>(*status);
            }
            return write;
        }

        [[nodiscard]] DictDataWrite parseDictDataWrite(const ConfigValue &body)
        {
            DictDataWrite write;
            write.type   = optionalString(body, "dictType");
            write.label  = optionalString(body, "label");
            write.value  = optionalString(body, "value");
            write.tone   = optionalString(body, "tone");
            write.remark = optionalString(body, "remark");
            if (const auto sort = optionalInteger(body, "sort"); sort.has_value())
            {
                write.sort = static_cast<int>(*sort);
            }
            if (const auto status = optionalInteger(body, "status"); status.has_value())
            {
                write.status = static_cast<int>(*status);
            }
            if (const auto isDefault = boolField(body, "def", false); body.contains("def"))
            {
                write.isDefault = isDefault;
            }
            return write;
        }

        [[nodiscard]] ConfigWrite parseConfigWrite(const ConfigValue &body)
        {
            ConfigWrite write;
            write.name   = optionalString(body, "name");
            write.key    = optionalString(body, "key");
            write.value  = optionalString(body, "value");
            write.type   = optionalString(body, "type");
            write.remark = optionalString(body, "remark");
            if (body.contains("builtin"))
            {
                write.builtin = boolField(body, "builtin", false);
            }
            return write;
        }

        [[nodiscard]] bool validUserWrite(const UserWrite &write, std::string &message)
        {
            if (write.username.has_value() && (write.username->empty() || write.username->size() > 64))
            {
                message = "登录账号长度必须是 1 到 64 个字符";
                return false;
            }
            if (write.nickname.has_value() && (write.nickname->empty() || write.nickname->size() > 64))
            {
                message = "用户姓名长度必须是 1 到 64 个字符";
                return false;
            }
            if (write.status.has_value() && *write.status != 0 && *write.status != 1)
            {
                message = "状态只能是 0 或 1";
                return false;
            }
            if (write.email.has_value() && write.email->size() > 128)
            {
                message = "邮箱不能超过 128 个字符";
                return false;
            }
            return true;
        }

        [[nodiscard]] bool validRoleWrite(const RoleWrite &write, std::string &message, const bool isCreate)
        {
            if (isCreate && (!write.name.has_value() || !write.code.has_value()))
            {
                message = "角色名称与编码不能为空";
                return false;
            }
            if (write.code.has_value() && !matchesPattern(*write.code, false))
            {
                message = "角色编码必须以小写字母开头，只含小写字母、数字与下划线";
                return false;
            }
            if (write.dataScope.has_value() && !isValidScope(*write.dataScope))
            {
                message = "数据范围只能是 ALL、DEPT_AND_CHILD、DEPT、SELF 或 CUSTOM";
                return false;
            }
            if (write.dataScope.value_or("") == "CUSTOM" && (!write.deptIds.has_value() || write.deptIds->empty()))
            {
                message = "自定义数据范围至少要选择一个部门";
                return false;
            }
            return true;
        }

        [[nodiscard]] bool validMenuWrite(const MenuWrite &write, std::string &message, const bool isCreate)
        {
            if (isCreate && (!write.name.has_value() || !write.type.has_value()))
            {
                message = "菜单名称与类型不能为空";
                return false;
            }
            if (write.type.has_value() && *write.type != "dir" && *write.type != "menu" && *write.type != "btn")
            {
                message = "菜单类型只能是 dir、menu 或 btn";
                return false;
            }
            if (write.path.has_value() && !write.path->empty() && write.path->front() != '/' && write.type.value_or("menu") != "btn")
            {
                message = "路由路径必须以 / 开头";
                return false;
            }
            return true;
        }

    } // namespace

    void registerSystemRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const SystemDependencies &dependencyInput)
    {
        const auto deps   = std::make_shared<SystemDependencies>(dependencyInput);
        const auto authOf = [deps]() -> Auth::AuthService & { return *deps->auth; };

        // ===== 用户 =====
        router.get("/api/system/users",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:user:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap query = request.queryParams();
                       const Auth::DataScope  scope = co_await deps->dataScopes->resolve(loop, session->userId);
                       UserQuery              userQuery;
                       userQuery.keyword = Common::queryText(query, "keyword");
                       userQuery.status  = Common::queryText(query, "status");
                       userQuery.deptId  = Common::queryInteger(query, "deptId").value_or(0);
                       if (!scope.all)
                       {
                           userQuery.allowedDeptIds = scope.deptIds;
                           userQuery.selfOnly       = scope.selfOnly;
                           userQuery.userId         = session->userId;
                       }
                       const Common::PageParameters page   = Common::parsePageParameters(query);
                       const auto                   users  = deps->users->scoped(session->tenantScope());
                       const auto                   result = co_await users->list(loop, userQuery, page);
                       ConfigValue                  rows   = ConfigValue::array();
                       for (const UserRecord &user: result.rows)
                       {
                           rows.push_back(userJson(user));
                       }
                       Common::writeSuccess(response, Common::makePageResult(page, result.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.get("/api/system/users/options",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:user:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       ConfigValue tenantOptions = ConfigValue::array();
                       if (session->platformAdmin)
                       {
                           const auto tenants = co_await deps->tenants->list(loop, "", "", Common::PageParameters{1, 100});
                           for (const TenantRecord &tenant: tenants.rows)
                           {
                               tenantOptions.push_back(ConfigValue::object({{"id", tenant.id}, {"name", tenant.name}, {"code", tenant.code}}));
                           }
                       }
                       const auto  roles       = co_await deps->roles->scoped(session->tenantScope())->listOptions(loop);
                       const auto  posts       = co_await deps->org->scoped(session->tenantScope())->postOptions(loop);
                       const auto  depts       = co_await deps->org->scoped(session->tenantScope())->deptTree(loop, "", "");
                       ConfigValue roleOptions = ConfigValue::array();
                       for (const RoleRecord &role: roles)
                       {
                           roleOptions.push_back(ConfigValue::object({{"id", role.id}, {"name", role.name}, {"code", role.code}}));
                       }
                       ConfigValue postOptions = ConfigValue::array();
                       for (const PostRecord &post: posts)
                       {
                           postOptions.push_back(ConfigValue::object({{"id", post.id}, {"name", post.name}}));
                       }
                       ConfigValue deptOptions = ConfigValue::array();
                       for (const DeptRecord &dept: depts)
                       {
                           deptOptions.push_back(deptJson(dept));
                       }
                       const std::string initPassword = Common::environmentValue("EDGEPULSE_INITIAL_PASSWORD").value_or("");
                       Common::writeSuccess(response,
                                            ConfigValue::object({{"depts", std::move(deptOptions)},
                                                                 {"roles", std::move(roleOptions)},
                                                                 {"posts", std::move(postOptions)},
                                                                 {"tenants", std::move(tenantOptions)},
                                                                 {"initPassword", initPassword}}),
                                            traceId);
                       co_return;
                   });

        router.get("/api/system/users/export",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:user:export", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap query = request.queryParams();
                       const Auth::DataScope  scope = co_await deps->dataScopes->resolve(loop, session->userId);
                       UserQuery              userQuery;
                       userQuery.keyword = Common::queryText(query, "keyword");
                       userQuery.status  = Common::queryText(query, "status");
                       userQuery.deptId  = Common::queryInteger(query, "deptId").value_or(0);
                       if (!scope.all)
                       {
                           userQuery.allowedDeptIds = scope.deptIds;
                           userQuery.selfOnly       = scope.selfOnly;
                           userQuery.userId         = session->userId;
                       }
                       const auto        users = deps->users->scoped(session->tenantScope());
                       const auto        rows  = co_await users->exportAll(loop, userQuery);
                       Common::CsvWriter writer;
                       writer.addRow({"用户ID", "登录账号", "姓名", "部门", "岗位", "角色", "手机号", "邮箱", "状态", "创建时间", "最近登录"});
                       for (const UserRecord &user: rows)
                       {
                           writer.addRow({std::to_string(user.id), user.username, user.nickname, user.departmentName, user.postNames, user.roleNames, user.phone, user.email,
                                          user.status == 1 ? "正常" : "停用", user.createTime, user.lastLoginTime});
                       }
                       writeCsv(response, writer, "users.csv", traceId);
                       co_return;
                   });

        router.post(
                "/api/system/users",
                [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                {
                    const std::string traceId = Web::resolveRequestId(request);
                    const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:user:add", traceId);
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
                    UserWrite   write = parseUserWrite(*body);
                    std::string message;
                    if (!validUserWrite(write, message) || !write.username.has_value() || !write.nickname.has_value())
                    {
                        fail(response, Common::kCodeBadRequest, message.empty() ? "登录账号与用户姓名不能为空" : message, traceId);
                        co_return;
                    }
                    const std::string password = stringField(*body, "password");
                    if (const auto validation = deps->auth->validatePassword(password); !validation)
                    {
                        fail(response, Common::kCodeBadRequest, validation.error(), traceId);
                        co_return;
                    }
                    if (!session->platformAdmin && write.roleIds.has_value() && std::find(write.roleIds->begin(), write.roleIds->end(), std::int64_t{1}) != write.roleIds->end())
                    {
                        fail(response, Common::kCodeForbidden, "租户账号不能分配平台超级管理员角色", traceId);
                        co_return;
                    }
                    std::int64_t targetTenantId = session->tenantId;
                    if (session->platformAdmin && body->contains("tenantId"))
                    {
                        const auto tenantId = optionalInteger(*body, "tenantId");
                        if (!tenantId.has_value() || *tenantId <= 0)
                        {
                            fail(response, Common::kCodeBadRequest, "tenantId 不合法", traceId);
                            co_return;
                        }
                        const auto tenant = co_await deps->tenants->findById(loop, *tenantId);
                        if (!tenant.has_value() || tenant->status != 1)
                        {
                            fail(response, Common::kCodeBadRequest, "目标租户不存在或已停用", traceId);
                            co_return;
                        }
                        targetTenantId = *tenantId;
                    }
                    const auto globalUsers = deps->users->scoped({1, true});
                    if (co_await globalUsers->usernameExists(loop, *write.username))
                    {
                        fail(response, Common::kCodeConflict, "登录账号已存在", traceId);
                        co_return;
                    }
                    const std::string  hash = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::string>(loop, [password] { return Common::hashPassword(password); });
                    const auto         targetUsers = deps->users->scoped({targetTenantId, false});
                    const std::int64_t id          = co_await targetUsers->create(loop, write, hash, session->userId);
                    Common::writeSuccess(response, ConfigValue::object({{"id", id}}), traceId);
                    co_return;
                });

        router.put("/api/system/users/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:user:edit", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto userId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!userId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "用户标识不合法", traceId);
                           co_return;
                       }
                       const auto body = request.jsonBody();
                       if (!body.has_value() || !body->is_object())
                       {
                           fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                           co_return;
                       }
                       UserWrite   write = parseUserWrite(*body);
                       std::string message;
                       if (!validUserWrite(write, message))
                       {
                           fail(response, Common::kCodeBadRequest, message, traceId);
                           co_return;
                       }
                       if (!session->platformAdmin && write.roleIds.has_value() && std::find(write.roleIds->begin(), write.roleIds->end(), std::int64_t{1}) != write.roleIds->end())
                       {
                           fail(response, Common::kCodeForbidden, "租户账号不能分配平台超级管理员角色", traceId);
                           co_return;
                       }
                       const auto globalUsers = deps->users->scoped({1, true});
                       if (write.username.has_value() && co_await globalUsers->usernameExists(loop, *write.username, *userId))
                       {
                           fail(response, Common::kCodeConflict, "登录账号已存在", traceId);
                           co_return;
                       }
                       const auto users = deps->users->scoped(session->tenantScope());
                       if (!co_await users->update(loop, *userId, write, session->userId))
                       {
                           fail(response, Common::kCodeNotFound, "用户不存在或已删除", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.put("/api/system/users/:id/status",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:user:edit", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto userId = Common::parsePositiveIdentifier(request.param("id"));
                       const auto body   = request.jsonBody();
                       const auto status = body.has_value() ? optionalInteger(*body, "status") : std::nullopt;
                       if (!userId.has_value() || !status.has_value() || (*status != 0 && *status != 1))
                       {
                           fail(response, Common::kCodeBadRequest, "用户标识或状态不合法", traceId);
                           co_return;
                       }
                       if (*userId == session->userId && *status == 0)
                       {
                           fail(response, Common::kCodeBadRequest, "不能停用当前登录账号", traceId);
                           co_return;
                       }
                       const auto users = deps->users->scoped(session->tenantScope());
                       if (!co_await users->updateStatus(loop, *userId, static_cast<int>(*status), session->userId))
                       {
                           fail(response, Common::kCodeNotFound, "用户不存在或已删除", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.put("/api/system/users/:id/password",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:user:reset", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto        userId   = Common::parsePositiveIdentifier(request.param("id"));
                       const auto        body     = request.jsonBody();
                       const std::string password = body.has_value() ? stringField(*body, "password") : std::string{};
                       if (!userId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "用户标识不合法", traceId);
                           co_return;
                       }
                       if (const auto validation = deps->auth->validatePassword(password); !validation)
                       {
                           fail(response, Common::kCodeBadRequest, validation.error(), traceId);
                           co_return;
                       }
                       const std::string hash = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::string>(loop, [password] { return Common::hashPassword(password); });
                       const auto        users = deps->users->scoped(session->tenantScope());
                       if (!co_await users->resetPassword(loop, *userId, hash, session->userId))
                       {
                           fail(response, Common::kCodeNotFound, "用户不存在或已删除", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.post("/api/system/users/:id/mfa/reset",
                    [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:user:edit", traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const auto userId = Common::parsePositiveIdentifier(request.param("id"));
                        if (!userId.has_value())
                        {
                            fail(response, Common::kCodeBadRequest, "用户标识不合法", traceId);
                            co_return;
                        }
                        if (deps->mfa == nullptr)
                        {
                            fail(response, Common::kCodeServiceUnavailable, "两步验证服务未装配", traceId);
                            co_return;
                        }
                        // 租户边界先于重置：越界目标与不存在同样回 404，不暴露其他租户的账号
                        const auto target = co_await deps->users->scoped(session->tenantScope())->findById(loop, *userId);
                        if (!target.has_value())
                        {
                            fail(response, Common::kCodeNotFound, "用户不存在或已删除", traceId);
                            co_return;
                        }
                        // 已启用与未启用的账号都按成功处理，管理员重复重置是幂等的
                        static_cast<void>(co_await deps->mfa->reset(loop, *userId));
                        Common::writeSuccess(response, nullptr, traceId);
                        co_return;
                    });

        router.del("/api/system/users/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:user:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto userId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!userId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "用户标识不合法", traceId);
                           co_return;
                       }
                       if (*userId == session->userId || *userId == 1)
                       {
                           fail(response, Common::kCodeBadRequest, "不能删除当前账号或内置管理员", traceId);
                           co_return;
                       }
                       const auto users = deps->users->scoped(session->tenantScope());
                       if (!co_await users->remove(loop, *userId))
                       {
                           fail(response, Common::kCodeNotFound, "用户不存在或已删除", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        // ===== 角色 =====
        router.get("/api/system/roles",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:role:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap       query = request.queryParams();
                       const Common::PageParameters page  = Common::parsePageParameters(query);
                       const auto                   result =
                               co_await deps->roles->scoped(session->tenantScope())->list(loop, Common::queryText(query, "keyword"), Common::queryText(query, "status"), page);
                       ConfigValue rows = ConfigValue::array();
                       for (const RoleRecord &role: result.rows)
                       {
                           rows.push_back(roleJson(role, true));
                       }
                       Common::writeSuccess(response, Common::makePageResult(page, result.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.get("/api/system/roles/export",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:role:export", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap query = request.queryParams();
                       const auto             rows =
                               co_await deps->roles->scoped(session->tenantScope())->exportAll(loop, Common::queryText(query, "keyword"), Common::queryText(query, "status"));
                       Common::CsvWriter writer;
                       writer.addRow({"角色ID", "角色名称", "角色编码", "数据范围", "用户数", "排序", "状态", "备注", "创建时间"});
                       for (const RoleRecord &role: rows)
                       {
                           writer.addRow({std::to_string(role.id), role.name, role.code, scopeLabel(role.dataScope), std::to_string(role.userCount), std::to_string(role.sort),
                                          role.status == 1 ? "正常" : "停用", role.remark, role.createTime});
                       }
                       writeCsv(response, writer, "roles.csv", traceId);
                       co_return;
                   });

        router.post("/api/system/roles",
                    [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:role:add", traceId);
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
                        const RoleWrite write = parseRoleWrite(*body);
                        std::string     message;
                        if (!validRoleWrite(write, message, true))
                        {
                            fail(response, Common::kCodeBadRequest, message, traceId);
                            co_return;
                        }
                        if (!session->platformAdmin)
                        {
                            if (write.menuIds.has_value())
                            {
                                const auto permissions = co_await deps->menus->permissionsFor(loop, *write.menuIds);
                                for (const std::string &permission: permissions)
                                {
                                    if (!deps->auth->hasPermission(*session, permission))
                                    {
                                        fail(response, Common::kCodeForbidden, "不能授予当前账号没有的权限", traceId);
                                        co_return;
                                    }
                                }
                            }
                            if (write.deptIds.has_value())
                            {
                                const auto org = deps->org->scoped(session->tenantScope());
                                for (const std::int64_t deptId: *write.deptIds)
                                {
                                    if (!(co_await org->findDept(loop, deptId)).has_value())
                                    {
                                        fail(response, Common::kCodeBadRequest, "数据范围包含其他租户的部门", traceId);
                                        co_return;
                                    }
                                }
                            }
                        }
                        if (co_await deps->roles->scoped(session->tenantScope())->codeExists(loop, *write.code))
                        {
                            fail(response, Common::kCodeConflict, "角色编码已存在", traceId);
                            co_return;
                        }
                        const std::int64_t id = co_await deps->roles->scoped(session->tenantScope())->create(loop, write, session->userId);
                        Common::writeSuccess(response, ConfigValue::object({{"id", id}}), traceId);
                        co_return;
                    });

        router.put("/api/system/roles/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:role:edit", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto roleId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!roleId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "角色标识不合法", traceId);
                           co_return;
                       }
                       const auto body = request.jsonBody();
                       if (!body.has_value() || !body->is_object())
                       {
                           fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                           co_return;
                       }
                       const RoleWrite write = parseRoleWrite(*body);
                       std::string     message;
                       if (!validRoleWrite(write, message, false))
                       {
                           fail(response, Common::kCodeBadRequest, message, traceId);
                           co_return;
                       }
                       if (!session->platformAdmin)
                       {
                           if (write.menuIds.has_value())
                           {
                               const auto permissions = co_await deps->menus->permissionsFor(loop, *write.menuIds);
                               for (const std::string &permission: permissions)
                               {
                                   if (!deps->auth->hasPermission(*session, permission))
                                   {
                                       fail(response, Common::kCodeForbidden, "不能授予当前账号没有的权限", traceId);
                                       co_return;
                                   }
                               }
                           }
                           if (write.deptIds.has_value())
                           {
                               const auto org = deps->org->scoped(session->tenantScope());
                               for (const std::int64_t deptId: *write.deptIds)
                               {
                                   if (!(co_await org->findDept(loop, deptId)).has_value())
                                   {
                                       fail(response, Common::kCodeBadRequest, "数据范围包含其他租户的部门", traceId);
                                       co_return;
                                   }
                               }
                           }
                       }
                       if (write.code.has_value() && co_await deps->roles->scoped(session->tenantScope())->codeExists(loop, *write.code, *roleId))
                       {
                           fail(response, Common::kCodeConflict, "角色编码已存在", traceId);
                           co_return;
                       }
                       if (!co_await deps->roles->scoped(session->tenantScope())->update(loop, *roleId, write, session->userId))
                       {
                           fail(response, Common::kCodeNotFound, "角色不存在或已删除", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.get("/api/system/roles/:id/menus",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:role:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto roleId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!roleId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "角色标识不合法", traceId);
                           co_return;
                       }
                       const auto roles = deps->roles->scoped(session->tenantScope());
                       if (!(co_await roles->findById(loop, *roleId)).has_value())
                       {
                           fail(response, Common::kCodeNotFound, "角色不存在或已删除", traceId);
                           co_return;
                       }
                       const auto menuIds = co_await roles->menuIdsFor(loop, *roleId);
                       Common::writeSuccess(response, ConfigValue::object({{"menuIds", Json::integerArray(menuIds)}}), traceId);
                       co_return;
                   });

        router.get("/api/system/roles/:id/depts",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:role:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto roleId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!roleId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "角色标识不合法", traceId);
                           co_return;
                       }
                       const auto roles = deps->roles->scoped(session->tenantScope());
                       if (!(co_await roles->findById(loop, *roleId)).has_value())
                       {
                           fail(response, Common::kCodeNotFound, "角色不存在或已删除", traceId);
                           co_return;
                       }
                       const auto deptIds = co_await roles->deptIdsFor(loop, *roleId);
                       Common::writeSuccess(response, ConfigValue::object({{"deptIds", Json::integerArray(deptIds)}}), traceId);
                       co_return;
                   });

        router.del("/api/system/roles/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:role:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto roleId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!roleId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "角色标识不合法", traceId);
                           co_return;
                       }
                       const auto role = co_await deps->roles->scoped(session->tenantScope())->findById(loop, *roleId);
                       if (!role.has_value())
                       {
                           fail(response, Common::kCodeNotFound, "角色不存在或已删除", traceId);
                           co_return;
                       }
                       if (role->builtin || *roleId == 1)
                       {
                           fail(response, Common::kCodeConflict, "内置角色不能删除", traceId);
                           co_return;
                       }
                       if (!session->platformAdmin)
                       {
                           const auto ownRoles = co_await deps->users->roleIdsFor(loop, session->userId);
                           if (std::find(ownRoles.begin(), ownRoles.end(), *roleId) != ownRoles.end())
                           {
                               fail(response, Common::kCodeConflict, "不能删除自己正在使用的角色", traceId);
                               co_return;
                           }
                       }
                       if (!co_await deps->roles->scoped(session->tenantScope())->remove(loop, *roleId))
                       {
                           fail(response, Common::kCodeNotFound, "角色不存在或已删除", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.get("/api/system/permissions/graph",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:role:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::PageParameters page{1, 100};
                       const auto                   usersPage = co_await deps->users->scoped(session->tenantScope())->list(loop, UserQuery{}, page);
                       const auto                   rolesPage = co_await deps->roles->scoped(session->tenantScope())->list(loop, "", "", page);
                       const auto                   menuTree  = co_await deps->menus->tree(loop, "", "");
                       ConfigValue                  users     = ConfigValue::array();
                       for (const UserRecord &user: usersPage.rows)
                       {
                           users.push_back(ConfigValue::object(
                                   {{"id", user.id}, {"username", user.username}, {"nickname", user.nickname}, {"deptName", user.departmentName}, {"status", user.status}}));
                       }
                       ConfigValue roles = ConfigValue::array();
                       for (const RoleRecord &role: rolesPage.rows)
                       {
                           const auto menuIds = co_await deps->roles->scoped(session->tenantScope())->menuIdsFor(loop, role.id);
                           roles.push_back(ConfigValue::object({{"id", role.id},
                                                                {"name", role.name},
                                                                {"code", role.code},
                                                                {"dataScope", role.dataScope},
                                                                {"status", role.status},
                                                                {"builtin", role.builtin},
                                                                {"userCount", role.userCount},
                                                                {"menuIds", Json::integerArray(menuIds)}}));
                       }
                       std::vector<MenuNode> flatMenus;
                       const auto            flatten = [&flatMenus](const auto &self, const MenuNode &node) -> void
                       {
                           flatMenus.push_back(node);
                           for (const MenuNode &child: node.children)
                           {
                               self(self, child);
                           }
                       };
                       for (const MenuNode &root: menuTree)
                       {
                           flatten(flatten, root);
                       }
                       ConfigValue menus = ConfigValue::array();
                       for (const MenuNode &node: flatMenus)
                       {
                           menus.push_back(ConfigValue::object({{"id", node.id},
                                                                {"parentId", node.parentId},
                                                                {"name", node.name},
                                                                {"type", node.type},
                                                                {"icon", node.icon},
                                                                {"path", node.path},
                                                                {"permission", node.permission},
                                                                {"sort", node.sort},
                                                                {"visible", node.visible},
                                                                {"status", node.status}}));
                       }
                       const auto  pairs     = co_await deps->users->scoped(session->tenantScope())->userRolePairs(loop, 10000);
                       ConfigValue userRoles = ConfigValue::array();
                       for (const auto &[userId, roleId]: pairs)
                       {
                           userRoles.push_back(ConfigValue::array({userId, roleId}));
                       }
                       Common::writeSuccess(response,
                                            ConfigValue::object({{"users", std::move(users)},
                                                                 {"roles", std::move(roles)},
                                                                 {"menus", std::move(menus)},
                                                                 {"userRoles", std::move(userRoles)},
                                                                 {"usersVisible", true}}),
                                            traceId);
                       co_return;
                   });

        // ===== 菜单 =====
        router.get("/api/system/menus/tree",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:menu:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap query = request.queryParams();
                       const auto             tree  = co_await deps->menus->tree(loop, Common::queryText(query, "keyword"), Common::queryText(query, "status"));
                       ConfigValue            rows  = ConfigValue::array();
                       for (const MenuNode &node: tree)
                       {
                           rows.push_back(menuJson(node));
                       }
                       Common::writeSuccess(response, std::move(rows), traceId);
                       co_return;
                   });

        router.get("/api/system/menus/export",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:menu:export", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap query = request.queryParams();
                       const auto             tree  = co_await deps->menus->tree(loop, Common::queryText(query, "keyword"), Common::queryText(query, "status"));
                       Common::CsvWriter      writer;
                       writer.addRow({"菜单ID", "父节点", "名称", "类型", "权限标识", "路由", "组件", "排序", "显示", "状态"});
                       const auto flatten = [&writer](const auto &self, const MenuNode &node) -> void
                       {
                           writer.addRow({std::to_string(node.id), std::to_string(node.parentId), node.name, node.type, node.permission, node.path, node.component,
                                          std::to_string(node.sort), node.visible == 1 ? "显示" : "隐藏", node.status == 1 ? "正常" : "停用"});
                           for (const MenuNode &child: node.children)
                           {
                               self(self, child);
                           }
                       };
                       for (const MenuNode &node: tree)
                       {
                           flatten(flatten, node);
                       }
                       writeCsv(response, writer, "menus.csv", traceId);
                       co_return;
                   });

        router.post("/api/system/menus",
                    [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:menu:add", traceId);
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
                        const MenuWrite write = parseMenuWrite(*body);
                        std::string     message;
                        if (!validMenuWrite(write, message, true))
                        {
                            fail(response, Common::kCodeBadRequest, message, traceId);
                            co_return;
                        }
                        const std::int64_t id = co_await deps->menus->create(loop, write, session->userId);
                        Common::writeSuccess(response, ConfigValue::object({{"id", id}}), traceId);
                        co_return;
                    });

        router.put("/api/system/menus/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:menu:edit", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto menuId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!menuId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "菜单标识不合法", traceId);
                           co_return;
                       }
                       const auto body = request.jsonBody();
                       if (!body.has_value() || !body->is_object())
                       {
                           fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                           co_return;
                       }
                       const MenuWrite write = parseMenuWrite(*body);
                       std::string     message;
                       if (!validMenuWrite(write, message, false) || (write.parentId.has_value() && *write.parentId == *menuId))
                       {
                           fail(response, Common::kCodeBadRequest, message.empty() ? "上级菜单不能是自身" : message, traceId);
                           co_return;
                       }
                       if (!co_await deps->menus->update(loop, *menuId, write, session->userId))
                       {
                           fail(response, Common::kCodeNotFound, "菜单不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.del("/api/system/menus/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:menu:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto menuId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!menuId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "菜单标识不合法", traceId);
                           co_return;
                       }
                       if (co_await deps->menus->hasChildren(loop, *menuId))
                       {
                           fail(response, Common::kCodeConflict, "请先删除子菜单", traceId);
                           co_return;
                       }
                       if (!co_await deps->menus->remove(loop, *menuId))
                       {
                           fail(response, Common::kCodeNotFound, "菜单不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        // ===== 部门 =====
        router.get("/api/system/depts/tree",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:dept:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap query = request.queryParams();
                       const auto             tree =
                               co_await deps->org->scoped(session->tenantScope())->deptTree(loop, Common::queryText(query, "keyword"), Common::queryText(query, "status"));
                       ConfigValue rows = ConfigValue::array();
                       for (const DeptRecord &dept: tree)
                       {
                           rows.push_back(deptJson(dept));
                       }
                       Common::writeSuccess(response, std::move(rows), traceId);
                       co_return;
                   });

        router.get("/api/system/depts/export",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:dept:export", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap query = request.queryParams();
                       const auto             tree =
                               co_await deps->org->scoped(session->tenantScope())->deptTree(loop, Common::queryText(query, "keyword"), Common::queryText(query, "status"));
                       Common::CsvWriter writer;
                       writer.addRow({"部门ID", "上级部门", "部门名称", "负责人", "联系电话", "邮箱", "排序", "状态", "人数"});
                       const auto flatten = [&writer](const auto &self, const DeptRecord &dept) -> void
                       {
                           writer.addRow({std::to_string(dept.id), std::to_string(dept.parentId), dept.name, dept.leader, dept.phone, dept.email, std::to_string(dept.sort),
                                          dept.status == 1 ? "正常" : "停用", std::to_string(dept.memberCount)});
                           for (const DeptRecord &child: dept.children)
                           {
                               self(self, child);
                           }
                       };
                       for (const DeptRecord &dept: tree)
                       {
                           flatten(flatten, dept);
                       }
                       writeCsv(response, writer, "depts.csv", traceId);
                       co_return;
                   });

        router.post("/api/system/depts",
                    [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:dept:add", traceId);
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
                        const DeptWrite write = parseDeptWrite(*body);
                        if (!write.name.has_value() || write.name->empty())
                        {
                            fail(response, Common::kCodeBadRequest, "部门名称不能为空", traceId);
                            co_return;
                        }
                        if (co_await deps->org->scoped(session->tenantScope())->deptNameExists(loop, write.parentId.value_or(0), *write.name))
                        {
                            fail(response, Common::kCodeConflict, "同级部门名称已存在", traceId);
                            co_return;
                        }
                        if (write.parentId.value_or(0) != 0 && !(co_await deps->org->scoped(session->tenantScope())->findDept(loop, *write.parentId)).has_value())
                        {
                            fail(response, Common::kCodeBadRequest, "上级部门不存在", traceId);
                            co_return;
                        }
                        const std::int64_t id = co_await deps->org->scoped(session->tenantScope())->createDept(loop, write, session->userId);
                        Common::writeSuccess(response, ConfigValue::object({{"id", id}}), traceId);
                        co_return;
                    });

        router.put("/api/system/depts/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:dept:edit", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto deptId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!deptId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "部门标识不合法", traceId);
                           co_return;
                       }
                       const auto body = request.jsonBody();
                       if (!body.has_value() || !body->is_object())
                       {
                           fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                           co_return;
                       }
                       const DeptWrite write = parseDeptWrite(*body);
                       if (write.parentId.has_value() && *write.parentId == *deptId)
                       {
                           fail(response, Common::kCodeBadRequest, "上级部门不能是自身", traceId);
                           co_return;
                       }
                       if (write.parentId.has_value() && *write.parentId != 0 && !(co_await deps->org->scoped(session->tenantScope())->findDept(loop, *write.parentId)).has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "上级部门不存在", traceId);
                           co_return;
                       }
                       if (write.name.has_value() && co_await deps->org->scoped(session->tenantScope())->deptNameExists(loop, write.parentId.value_or(0), *write.name, *deptId))
                       {
                           fail(response, Common::kCodeConflict, "同级部门名称已存在", traceId);
                           co_return;
                       }
                       if (!co_await deps->org->scoped(session->tenantScope())->updateDept(loop, *deptId, write, session->userId))
                       {
                           fail(response, Common::kCodeNotFound, "部门不存在或已删除", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.del("/api/system/depts/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:dept:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto deptId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!deptId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "部门标识不合法", traceId);
                           co_return;
                       }
                       if (co_await deps->org->scoped(session->tenantScope())->deptHasChildren(loop, *deptId))
                       {
                           fail(response, Common::kCodeConflict, "请先删除子部门", traceId);
                           co_return;
                       }
                       if (co_await deps->org->scoped(session->tenantScope())->deptHasUsers(loop, *deptId))
                       {
                           fail(response, Common::kCodeConflict, "部门下仍有用户，不能删除", traceId);
                           co_return;
                       }
                       if (!co_await deps->org->scoped(session->tenantScope())->removeDept(loop, *deptId))
                       {
                           fail(response, Common::kCodeNotFound, "部门不存在或已删除", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        // ===== 岗位 =====
        router.get("/api/system/posts",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:post:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap       query = request.queryParams();
                       const Common::PageParameters page  = Common::parsePageParameters(query);
                       const auto                   result =
                               co_await deps->org->scoped(session->tenantScope())->listPosts(loop, Common::queryText(query, "keyword"), Common::queryText(query, "status"), page);
                       ConfigValue rows = ConfigValue::array();
                       for (const PostRecord &post: result.rows)
                       {
                           rows.push_back(postJson(post));
                       }
                       Common::writeSuccess(response, Common::makePageResult(page, result.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.get("/api/system/posts/export",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:post:export", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap query = request.queryParams();
                       const auto             rows =
                               co_await deps->org->scoped(session->tenantScope())->exportPosts(loop, Common::queryText(query, "keyword"), Common::queryText(query, "status"));
                       Common::CsvWriter writer;
                       writer.addRow({"岗位ID", "岗位编码", "岗位名称", "在岗人数", "排序", "状态", "备注", "创建时间"});
                       for (const PostRecord &post: rows)
                       {
                           writer.addRow({std::to_string(post.id), post.code, post.name, std::to_string(post.userCount), std::to_string(post.sort),
                                          post.status == 1 ? "正常" : "停用", post.remark, post.createTime});
                       }
                       writeCsv(response, writer, "posts.csv", traceId);
                       co_return;
                   });

        router.post("/api/system/posts",
                    [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:post:add", traceId);
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
                        const PostWrite write = parsePostWrite(*body);
                        if (!write.name.has_value() || !write.code.has_value() || !matchesPattern(*write.code, true))
                        {
                            fail(response, Common::kCodeBadRequest, "岗位名称不能为空，编码必须以字母开头且只含字母、数字、下划线或短横线", traceId);
                            co_return;
                        }
                        if (co_await deps->org->scoped(session->tenantScope())->postCodeExists(loop, *write.code))
                        {
                            fail(response, Common::kCodeConflict, "岗位编码已存在", traceId);
                            co_return;
                        }
                        const std::int64_t id = co_await deps->org->scoped(session->tenantScope())->createPost(loop, write, session->userId);
                        Common::writeSuccess(response, ConfigValue::object({{"id", id}}), traceId);
                        co_return;
                    });

        router.put("/api/system/posts/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:post:edit", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto postId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!postId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "岗位标识不合法", traceId);
                           co_return;
                       }
                       const auto body = request.jsonBody();
                       if (!body.has_value() || !body->is_object())
                       {
                           fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                           co_return;
                       }
                       const PostWrite write = parsePostWrite(*body);
                       if (write.code.has_value() && !matchesPattern(*write.code, true))
                       {
                           fail(response, Common::kCodeBadRequest, "岗位编码格式不合法", traceId);
                           co_return;
                       }
                       if (write.code.has_value() && co_await deps->org->scoped(session->tenantScope())->postCodeExists(loop, *write.code, *postId))
                       {
                           fail(response, Common::kCodeConflict, "岗位编码已存在", traceId);
                           co_return;
                       }
                       if (!co_await deps->org->scoped(session->tenantScope())->updatePost(loop, *postId, write, session->userId))
                       {
                           fail(response, Common::kCodeNotFound, "岗位不存在或已删除", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.del("/api/system/posts/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:post:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto postId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!postId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "岗位标识不合法", traceId);
                           co_return;
                       }
                       if (co_await deps->org->scoped(session->tenantScope())->postHasUsers(loop, *postId))
                       {
                           fail(response, Common::kCodeConflict, "岗位下仍有用户，不能删除", traceId);
                           co_return;
                       }
                       if (!co_await deps->org->scoped(session->tenantScope())->removePost(loop, *postId))
                       {
                           fail(response, Common::kCodeNotFound, "岗位不存在或已删除", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        // ===== 字典 =====
        router.get("/api/system/dict/types",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:dict:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap       query  = request.queryParams();
                       const Common::PageParameters page   = Common::parsePageParameters(query);
                       const auto                   result = co_await deps->dictionaries->scoped(session->tenantScope())
                                                                     ->listDictTypes(loop, Common::queryText(query, "keyword"), Common::queryText(query, "status"), page);
                       ConfigValue                  rows   = ConfigValue::array();
                       for (const DictTypeRecord &record: result.rows)
                       {
                           rows.push_back(dictTypeJson(record));
                       }
                       Common::writeSuccess(response, Common::makePageResult(page, result.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.get("/api/system/dict/types/export",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:dict:export", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap query = request.queryParams();
                       const auto             rows  = co_await deps->dictionaries->scoped(session->tenantScope())
                                                              ->exportDictTypes(loop, Common::queryText(query, "keyword"), Common::queryText(query, "status"));
                       Common::CsvWriter      writer;
                       writer.addRow({"字典ID", "字典名称", "字典类型", "状态", "备注", "创建时间"});
                       for (const DictTypeRecord &record: rows)
                       {
                           writer.addRow({std::to_string(record.id), record.name, record.type, record.status == 1 ? "正常" : "停用", record.remark, record.createTime});
                       }
                       writeCsv(response, writer, "dict-types.csv", traceId);
                       co_return;
                   });

        router.post("/api/system/dict/types",
                    [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:dict:add", traceId);
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
                        const DictTypeWrite write = parseDictTypeWrite(*body);
                        if (!write.name.has_value() || !write.type.has_value() || !matchesPattern(*write.type, false))
                        {
                            fail(response, Common::kCodeBadRequest, "字典名称不能为空，类型必须小写字母开头且只含小写字母、数字与下划线", traceId);
                            co_return;
                        }
                        if (co_await deps->dictionaries->scoped(session->tenantScope())->dictTypeExists(loop, *write.type))
                        {
                            fail(response, Common::kCodeConflict, "字典类型已存在", traceId);
                            co_return;
                        }
                        const std::int64_t id = co_await deps->dictionaries->scoped(session->tenantScope())->createDictType(loop, write, session->userId);
                        Common::writeSuccess(response, ConfigValue::object({{"id", id}}), traceId);
                        co_return;
                    });

        router.put("/api/system/dict/types/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:dict:edit", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto typeId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!typeId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "字典类型标识不合法", traceId);
                           co_return;
                       }
                       const auto body = request.jsonBody();
                       if (!body.has_value() || !body->is_object())
                       {
                           fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                           co_return;
                       }
                       const DictTypeWrite write = parseDictTypeWrite(*body);
                       if (write.type.has_value() && !matchesPattern(*write.type, false))
                       {
                           fail(response, Common::kCodeBadRequest, "字典类型格式不合法", traceId);
                           co_return;
                       }
                       if (write.type.has_value() && co_await deps->dictionaries->scoped(session->tenantScope())->dictTypeExists(loop, *write.type, *typeId))
                       {
                           fail(response, Common::kCodeConflict, "字典类型已存在", traceId);
                           co_return;
                       }
                       if (!co_await deps->dictionaries->scoped(session->tenantScope())->updateDictType(loop, *typeId, write, session->userId))
                       {
                           fail(response, Common::kCodeNotFound, "字典类型不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.del("/api/system/dict/types/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:dict:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto typeId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!typeId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "字典类型标识不合法", traceId);
                           co_return;
                       }
                       if (!co_await deps->dictionaries->scoped(session->tenantScope())->removeDictType(loop, *typeId))
                       {
                           fail(response, Common::kCodeNotFound, "字典类型不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.get("/api/system/dict/data/:dictType",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:dict:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto dictType = request.param("dictType");
                       if (!dictType.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "字典类型不能为空", traceId);
                           co_return;
                       }
                       const auto  rows   = co_await deps->dictionaries->scoped(session->tenantScope())->dictData(loop, *dictType);
                       ConfigValue result = ConfigValue::array();
                       for (const DictDataRecord &record: rows)
                       {
                           result.push_back(dictDataJson(record));
                       }
                       Common::writeSuccess(response, std::move(result), traceId);
                       co_return;
                   });

        router.get("/api/system/dict/options/:dictType",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireBrowserSession(authOf(), loop, request, response, traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto dictType = request.param("dictType");
                       if (!dictType.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "字典类型不能为空", traceId);
                           co_return;
                       }
                       const auto  rows   = co_await deps->dictionaries->scoped(session->tenantScope())->dictOptions(loop, *dictType);
                       ConfigValue result = ConfigValue::array();
                       for (const DictDataRecord &record: rows)
                       {
                           result.push_back(dictDataJson(record));
                       }
                       Common::writeSuccess(response, std::move(result), traceId);
                       co_return;
                   });

        router.post("/api/system/dict/data",
                    [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:dict:add", traceId);
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
                        const DictDataWrite write = parseDictDataWrite(*body);
                        if (!write.type.has_value() || !write.label.has_value() || !write.value.has_value())
                        {
                            fail(response, Common::kCodeBadRequest, "字典类型、标签与键值不能为空", traceId);
                            co_return;
                        }
                        const std::int64_t id = co_await deps->dictionaries->scoped(session->tenantScope())->createDictData(loop, write, session->userId);
                        Common::writeSuccess(response, ConfigValue::object({{"id", id}}), traceId);
                        co_return;
                    });

        router.put("/api/system/dict/data/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:dict:edit", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto dataId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!dataId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "字典数据标识不合法", traceId);
                           co_return;
                       }
                       const auto body = request.jsonBody();
                       if (!body.has_value() || !body->is_object())
                       {
                           fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                           co_return;
                       }
                       const DictDataWrite write = parseDictDataWrite(*body);
                       if (!co_await deps->dictionaries->scoped(session->tenantScope())->updateDictData(loop, *dataId, write, session->userId))
                       {
                           fail(response, Common::kCodeNotFound, "字典数据不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.del("/api/system/dict/data/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:dict:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto dataId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!dataId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "字典数据标识不合法", traceId);
                           co_return;
                       }
                       if (!co_await deps->dictionaries->scoped(session->tenantScope())->removeDictData(loop, *dataId))
                       {
                           fail(response, Common::kCodeNotFound, "字典数据不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        // ===== 参数 =====
        router.get("/api/system/configs",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:config:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap       query = request.queryParams();
                       const Common::PageParameters page  = Common::parsePageParameters(query);
                       const auto  result = co_await deps->dictionaries->scoped(session->tenantScope())->listConfigs(loop, Common::queryText(query, "keyword"), page);
                       ConfigValue rows   = ConfigValue::array();
                       for (const ConfigRecord &record: result.rows)
                       {
                           rows.push_back(configJson(record));
                       }
                       Common::writeSuccess(response, Common::makePageResult(page, result.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.get("/api/system/configs/export",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:config:export", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap query = request.queryParams();
                       const auto             rows  = co_await deps->dictionaries->scoped(session->tenantScope())->exportConfigs(loop, Common::queryText(query, "keyword"));
                       Common::CsvWriter      writer;
                       writer.addRow({"参数ID", "参数名称", "参数键名", "参数值", "类型", "备注", "创建时间"});
                       for (const ConfigRecord &record: rows)
                       {
                           writer.addRow({std::to_string(record.id), record.name, record.key, record.value, record.type, record.remark, record.createTime});
                       }
                       writeCsv(response, writer, "configs.csv", traceId);
                       co_return;
                   });

        router.post("/api/system/configs",
                    [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:config:add", traceId);
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
                        const ConfigWrite write = parseConfigWrite(*body);
                        if (!write.name.has_value() || !write.key.has_value() || write.key->empty())
                        {
                            fail(response, Common::kCodeBadRequest, "参数名称与键名不能为空", traceId);
                            co_return;
                        }
                        if (co_await deps->dictionaries->scoped(session->tenantScope())->configKeyExists(loop, *write.key))
                        {
                            fail(response, Common::kCodeConflict, "参数键名已存在", traceId);
                            co_return;
                        }
                        const std::int64_t id = co_await deps->dictionaries->scoped(session->tenantScope())->createConfig(loop, write, session->userId);
                        Common::writeSuccess(response, ConfigValue::object({{"id", id}}), traceId);
                        co_return;
                    });

        router.put("/api/system/configs/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:config:edit", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto configId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!configId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "参数标识不合法", traceId);
                           co_return;
                       }
                       const auto body = request.jsonBody();
                       if (!body.has_value() || !body->is_object())
                       {
                           fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                           co_return;
                       }
                       const ConfigWrite write = parseConfigWrite(*body);
                       if (write.key.has_value() && co_await deps->dictionaries->scoped(session->tenantScope())->configKeyExists(loop, *write.key, *configId))
                       {
                           fail(response, Common::kCodeConflict, "参数键名已存在", traceId);
                           co_return;
                       }
                       if (!co_await deps->dictionaries->scoped(session->tenantScope())->updateConfig(loop, *configId, write, session->userId))
                       {
                           fail(response, Common::kCodeNotFound, "参数不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.del("/api/system/configs/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "system:config:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto configId = Common::parsePositiveIdentifier(request.param("id"));
                       if (!configId.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "参数标识不合法", traceId);
                           co_return;
                       }
                       const auto config = co_await deps->dictionaries->scoped(session->tenantScope())->findConfig(loop, *configId);
                       if (!config.has_value())
                       {
                           fail(response, Common::kCodeNotFound, "参数不存在", traceId);
                           co_return;
                       }
                       if (config->builtin)
                       {
                           fail(response, Common::kCodeConflict, "内置参数不能删除", traceId);
                           co_return;
                       }
                       if (!co_await deps->dictionaries->scoped(session->tenantScope())->removeConfig(loop, *configId))
                       {
                           fail(response, Common::kCodeNotFound, "参数不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });
    }
} // namespace EdgePulse::System
