#include "Core/Auth/DataScopeService.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace EdgePulse::Auth
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseValue;
    } // namespace

    DataScopeService::DataScopeService(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<DataScope> DataScopeService::resolve(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        co_return co_await m_database->withConnection<DataScope>(
                loop,
                [userId](DatabaseConnection &connection) -> DataScope
                {
                    DataScope scope;
                    scope.userId = userId;

                    const std::array<DatabaseValue, 1> userParameter{userId};
                    const auto                         userRows     = Sql::executeOrThrow(connection, "SELECT dept_id FROM sys_user WHERE id = ? AND deleted = 0", userParameter);
                    std::int64_t                       departmentId = 0;
                    if (userRows->next())
                    {
                        departmentId = Sql::integer(*userRows, "dept_id");
                    }

                    const auto                roleRows = Sql::executeOrThrow(connection,
                                                                             "SELECT r.id AS role_id, r.data_scope FROM sys_role r JOIN sys_user_role ur ON ur.role_id = r.id "
                                                                             "WHERE ur.user_id = ? AND r.status = 1 AND r.deleted = 0",
                                                                             userParameter);
                    std::vector<std::int64_t> customRoleIds;
                    bool                      hasDepartment = false;
                    bool                      hasSelf       = false;
                    while (roleRows->next())
                    {
                        const std::string dataScope = Sql::text(*roleRows, "data_scope");
                        if (dataScope == "ALL")
                        {
                            scope.all = true;
                            return scope;
                        }
                        if (dataScope == "DEPT" || dataScope == "DEPT_AND_CHILD")
                        {
                            hasDepartment = true;
                        } else if (dataScope == "CUSTOM")
                        {
                            customRoleIds.push_back(Sql::integer(*roleRows, "role_id"));
                        } else
                        {
                            hasSelf = true;
                        }
                    }
                    if (!hasDepartment && customRoleIds.empty())
                    {
                        // 没有任何角色或只有 SELF：按仅本人处理，绝不回退成全部
                        scope.selfOnly = true;
                        static_cast<void>(hasSelf);
                        return scope;
                    }

                    // 部门树在内存里展开，避免依赖两家数据库各不相同的递归语法
                    std::unordered_map<std::int64_t, std::vector<std::int64_t>> children;
                    const auto                                                  deptRows = Sql::executeOrThrow(connection, "SELECT id, parent_id FROM sys_dept WHERE deleted = 0");
                    while (deptRows->next())
                    {
                        children[Sql::integer(*deptRows, "parent_id")].push_back(Sql::integer(*deptRows, "id"));
                    }
                    std::vector<std::int64_t> roots;
                    if (hasDepartment)
                    {
                        roots.push_back(departmentId);
                    }
                    for (const std::int64_t roleId: customRoleIds)
                    {
                        const std::array<DatabaseValue, 1> roleParameter{roleId};
                        const auto                         customRows = Sql::executeOrThrow(connection, "SELECT dept_id FROM sys_role_dept WHERE role_id = ?", roleParameter);
                        while (customRows->next())
                        {
                            roots.push_back(Sql::integer(*customRows, "dept_id"));
                        }
                    }
                    std::vector<std::int64_t>    pending = roots;
                    std::map<std::int64_t, bool> visited;
                    while (!pending.empty())
                    {
                        const std::int64_t id = pending.back();
                        pending.pop_back();
                        if (visited.contains(id))
                        {
                            continue;
                        }
                        visited[id] = true;
                        scope.deptIds.push_back(id);
                        if (const auto iterator = children.find(id); iterator != children.end())
                        {
                            pending.insert(pending.end(), iterator->second.begin(), iterator->second.end());
                        }
                    }
                    if (scope.deptIds.empty() && hasSelf)
                    {
                        scope.selfOnly = true;
                    }
                    // 部门范围覆盖当前账号自身的行；只有 SELF 才需要额外的本人条件
                    if (scope.deptIds.empty())
                    {
                        scope.selfOnly = true;
                    }
                    return scope;
                });
    }
} // namespace EdgePulse::Auth
