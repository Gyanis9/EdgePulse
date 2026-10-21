#include "Core/System/OrgRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace EdgePulse::System
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        constexpr std::string_view kSelectDept = "SELECT d.id, d.parent_id, d.name, d.leader, d.phone, d.email, d.sort, d.status, d.created_at, "
                                                 "COALESCE((SELECT COUNT(*) FROM sys_user u WHERE u.dept_id = d.id AND u.deleted = 0), 0) AS member_count "
                                                 "FROM sys_dept d ";

        [[nodiscard]] DeptRecord readDept(const DatabaseResult &result)
        {
            DeptRecord dept;
            dept.id          = Sql::integer(result, "id");
            dept.parentId    = Sql::integer(result, "parent_id");
            dept.name        = Sql::text(result, "name");
            dept.leader      = Sql::text(result, "leader");
            dept.phone       = Sql::text(result, "phone");
            dept.email       = Sql::text(result, "email");
            dept.sort        = static_cast<int>(Sql::integer(result, "sort"));
            dept.status      = static_cast<int>(Sql::integer(result, "status"));
            dept.memberCount = Sql::integer(result, "member_count");
            dept.createTime  = Sql::dateTimeText(result, "created_at");
            return dept;
        }

        [[nodiscard]] std::vector<DeptRecord> buildDeptTree(std::vector<DeptRecord> nodes)
        {
            std::unordered_map<std::int64_t, DeptRecord>                byId;
            std::unordered_map<std::int64_t, std::vector<std::int64_t>> childrenByParent;
            for (DeptRecord &node: nodes)
            {
                const std::int64_t id = node.id;
                childrenByParent[node.parentId].push_back(id);
                byId.emplace(id, std::move(node));
            }
            std::function<DeptRecord(std::int64_t)> build = [&](const std::int64_t id) -> DeptRecord
            {
                DeptRecord node = byId.at(id);
                if (const auto children = childrenByParent.find(id); children != childrenByParent.end())
                {
                    for (const std::int64_t childId: children->second)
                    {
                        node.children.push_back(build(childId));
                    }
                }
                return node;
            };
            std::vector<DeptRecord> roots;
            if (const auto rootChildren = childrenByParent.find(0); rootChildren != childrenByParent.end())
            {
                for (const std::int64_t id: rootChildren->second)
                {
                    roots.push_back(build(id));
                }
            }
            return roots;
        }

        [[nodiscard]] bool filterDept(DeptRecord &node, const std::string &keyword, const std::string &status)
        {
            const bool              selfMatch = (keyword.empty() || node.name.find(keyword) != std::string::npos || node.leader.find(keyword) != std::string::npos) &&
                                                (status.empty() || std::to_string(node.status) == status);
            std::vector<DeptRecord> kept;
            for (DeptRecord &child: node.children)
            {
                if (filterDept(child, keyword, status))
                {
                    kept.push_back(std::move(child));
                }
            }
            node.children = std::move(kept);
            return selfMatch || !node.children.empty();
        }

        constexpr std::string_view kSelectPost =
                "SELECT p.id, p.post_code, p.post_name, p.sort, p.status, p.remark, p.created_at, "
                "COALESCE((SELECT COUNT(*) FROM sys_user_post up JOIN sys_user u ON u.id = up.user_id AND u.deleted = 0 WHERE up.post_id = p.id), 0) AS user_count "
                "FROM sys_post p ";

        [[nodiscard]] PostRecord readPost(const DatabaseResult &result)
        {
            PostRecord post;
            post.id         = Sql::integer(result, "id");
            post.code       = Sql::text(result, "post_code");
            post.name       = Sql::text(result, "post_name");
            post.sort       = static_cast<int>(Sql::integer(result, "sort"));
            post.status     = static_cast<int>(Sql::integer(result, "status"));
            post.remark     = Sql::text(result, "remark");
            post.userCount  = Sql::integer(result, "user_count");
            post.createTime = Sql::dateTimeText(result, "created_at");
            return post;
        }
    } // namespace

    OrgRepository::OrgRepository(std::shared_ptr<Common::Database> database, TenantScope tenant) : m_database(std::move(database)), m_tenant(tenant)
    {
    }

    std::shared_ptr<OrgRepository> OrgRepository::scoped(const TenantScope tenant) const
    {
        return std::make_shared<OrgRepository>(m_database, tenant);
    }

    namespace
    {
        [[nodiscard]] std::string tenantClause(const TenantScope &tenant, std::vector<DatabaseValue> &parameters, const std::string_view column)
        {
            if (tenant.allTenants)
            {
                return {};
            }
            parameters.emplace_back(tenant.tenantId);
            return " AND " + std::string{column} + " = ?";
        }
    } // namespace

    AsynGyanis::Core::Task<std::vector<DeptRecord>> OrgRepository::deptTree(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status)
    {
        co_return co_await m_database->withConnection<std::vector<DeptRecord>>(
                loop,
                [keyword = std::move(keyword), status = std::move(status), tenant = m_tenant](DatabaseConnection &connection) -> std::vector<DeptRecord>
                {
                    std::vector<DatabaseValue> parameters;
                    const std::string          clause = tenantClause(tenant, parameters, "d.tenant_id");
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectDept} + "WHERE d.deleted = 0" + clause + " ORDER BY d.sort, d.id", parameters);
                    std::vector<DeptRecord> nodes;
                    while (rows->next())
                    {
                        nodes.push_back(readDept(*rows));
                    }
                    std::vector<DeptRecord> roots = buildDeptTree(std::move(nodes));
                    roots.erase(std::remove_if(roots.begin(), roots.end(), [&](DeptRecord &node) { return !filterDept(node, keyword, status); }), roots.end());
                    return roots;
                });
    }

    AsynGyanis::Core::Task<std::vector<DeptRecord>> OrgRepository::allDepts(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await m_database->withConnection<std::vector<DeptRecord>>(
                loop,
                [tenant = m_tenant](DatabaseConnection &connection) -> std::vector<DeptRecord>
                {
                    std::vector<DatabaseValue> parameters;
                    const std::string          clause = tenantClause(tenant, parameters, "d.tenant_id");
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectDept} + "WHERE d.deleted = 0" + clause + " ORDER BY d.sort, d.id", parameters);
                    std::vector<DeptRecord> depts;
                    while (rows->next())
                    {
                        depts.push_back(readDept(*rows));
                    }
                    return depts;
                });
    }

    AsynGyanis::Core::Task<std::optional<DeptRecord>> OrgRepository::findDept(AsynGyanis::Core::EventLoop &loop, const std::int64_t deptId)
    {
        co_return co_await m_database->withConnection<std::optional<DeptRecord>>(
                loop,
                [deptId, tenant = m_tenant](DatabaseConnection &connection) -> std::optional<DeptRecord>
                {
                    std::vector<DatabaseValue> parameters{deptId};
                    const std::string          clause = tenantClause(tenant, parameters, "d.tenant_id");
                    const auto                 rows   = Sql::executeOrThrow(connection, std::string{kSelectDept} + "WHERE d.id = ? AND d.deleted = 0" + clause, parameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    return readDept(*rows);
                });
    }

    AsynGyanis::Core::Task<bool> OrgRepository::deptNameExists(AsynGyanis::Core::EventLoop &loop, const std::int64_t parentId, std::string name, const std::int64_t excludeId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [parentId, name = std::move(name), excludeId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{parentId, name, excludeId};
                    const std::string          clause = tenantClause(tenant, parameters, "tenant_id");
                    const auto                 rows   = Sql::executeOrThrow(
                            connection, "SELECT COUNT(*) AS total FROM sys_dept WHERE parent_id = ? AND name = ? AND id <> ? AND deleted = 0" + clause, parameters);
                    return rows->next() && Sql::integer(*rows, "total") > 0;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> OrgRepository::createDept(AsynGyanis::Core::EventLoop &loop, const DeptWrite &write, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [write, operatorId, tenant = m_tenant](DatabaseConnection &connection) -> std::int64_t
                {
                    const std::int64_t                  id  = Common::nextIdentifier();
                    const std::string                   now = Common::nowDateTimeUtc();
                    const std::array<DatabaseValue, 13> parameters{id,
                                                                   tenant.tenantId,
                                                                   write.parentId.value_or(0),
                                                                   write.name.value_or(std::string{}),
                                                                   write.leader.value_or(std::string{}),
                                                                   write.phone.value_or(std::string{}),
                                                                   write.email.value_or(std::string{}),
                                                                   static_cast<std::int64_t>(write.sort.value_or(0)),
                                                                   static_cast<std::int64_t>(write.status.value_or(1)),
                                                                   now,
                                                                   now,
                                                                   operatorId,
                                                                   operatorId};
                    Sql::executeOrThrowVoid(
                            connection,
                            "INSERT INTO sys_dept (id, tenant_id, parent_id, name, leader, phone, email, sort, status, created_at, updated_at, created_by, updated_by) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                            parameters);
                    return id;
                });
    }

    AsynGyanis::Core::Task<bool> OrgRepository::updateDept(AsynGyanis::Core::EventLoop &loop, const std::int64_t deptId, const DeptWrite &write, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [deptId, write, operatorId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                                                            {
                                                                std::string                sql = "UPDATE sys_dept SET updated_at = ?, updated_by = ?";
                                                                std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), operatorId};
                                                                if (write.parentId.has_value())
                                                                {
                                                                    sql += ", parent_id = ?";
                                                                    parameters.emplace_back(*write.parentId);
                                                                }
                                                                if (write.name.has_value())
                                                                {
                                                                    sql += ", name = ?";
                                                                    parameters.emplace_back(*write.name);
                                                                }
                                                                if (write.leader.has_value())
                                                                {
                                                                    sql += ", leader = ?";
                                                                    parameters.emplace_back(*write.leader);
                                                                }
                                                                if (write.phone.has_value())
                                                                {
                                                                    sql += ", phone = ?";
                                                                    parameters.emplace_back(*write.phone);
                                                                }
                                                                if (write.email.has_value())
                                                                {
                                                                    sql += ", email = ?";
                                                                    parameters.emplace_back(*write.email);
                                                                }
                                                                if (write.sort.has_value())
                                                                {
                                                                    sql += ", sort = ?";
                                                                    parameters.emplace_back(static_cast<std::int64_t>(*write.sort));
                                                                }
                                                                if (write.status.has_value())
                                                                {
                                                                    sql += ", status = ?";
                                                                    parameters.emplace_back(static_cast<std::int64_t>(*write.status));
                                                                }
                                                                sql += " WHERE id = ? AND deleted = 0";
                                                                parameters.emplace_back(deptId);
                                                                sql += tenantClause(tenant, parameters, "tenant_id");
                                                                const auto rows = Sql::executeOrThrow(connection, sql, parameters);
                                                                return rows->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<bool> OrgRepository::removeDept(AsynGyanis::Core::EventLoop &loop, const std::int64_t deptId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [deptId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), deptId};
                    const std::string          clause = tenantClause(tenant, parameters, "tenant_id");
                    const auto rows = Sql::executeOrThrow(connection, "UPDATE sys_dept SET deleted = 1, updated_at = ? WHERE id = ? AND deleted = 0" + clause, parameters);
                    return rows->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<bool> OrgRepository::deptHasChildren(AsynGyanis::Core::EventLoop &loop, const std::int64_t deptId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [deptId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{deptId};
                    const std::string          clause = tenantClause(tenant, parameters, "tenant_id");
                    const auto rows = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_dept WHERE parent_id = ? AND deleted = 0" + clause, parameters);
                    return rows->next() && Sql::integer(*rows, "total") > 0;
                });
    }

    AsynGyanis::Core::Task<bool> OrgRepository::deptHasUsers(AsynGyanis::Core::EventLoop &loop, const std::int64_t deptId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [deptId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{deptId};
                    const std::string          clause = tenantClause(tenant, parameters, "tenant_id");
                    const auto rows = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_user WHERE dept_id = ? AND deleted = 0" + clause, parameters);
                    return rows->next() && Sql::integer(*rows, "total") > 0;
                });
    }

    AsynGyanis::Core::Task<Common::PageRows<PostRecord>> OrgRepository::listPosts(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status,
                                                                                  const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<PostRecord>>(
                loop,
                [keyword = std::move(keyword), status = std::move(status), page, tenant = m_tenant](DatabaseConnection &connection) -> Common::PageRows<PostRecord>
                {
                    std::string                filter = "WHERE p.deleted = 0";
                    std::vector<DatabaseValue> parameters;
                    filter += tenantClause(tenant, parameters, "p.tenant_id");
                    if (!keyword.empty())
                    {
                        const std::string pattern = "%" + keyword + "%";
                        filter += " AND (p.post_name LIKE ? OR p.post_code LIKE ?)";
                        parameters.emplace_back(pattern);
                        parameters.emplace_back(pattern);
                    }
                    if (status == "0" || status == "1")
                    {
                        filter += " AND p.status = ?";
                        parameters.emplace_back(static_cast<std::int64_t>(status == "1" ? 1 : 0));
                    }
                    const auto                   count = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_post p " + filter, parameters);
                    Common::PageRows<PostRecord> pageRows;
                    if (count->next())
                    {
                        pageRows.total = Sql::integer(*count, "total");
                    }
                    if (pageRows.total == 0)
                    {
                        return pageRows;
                    }
                    parameters.emplace_back(static_cast<std::int64_t>(page.pageSize));
                    parameters.emplace_back(static_cast<std::int64_t>(page.offset()));
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectPost} + filter + " ORDER BY p.sort, p.id LIMIT ? OFFSET ?", parameters);
                    while (rows->next())
                    {
                        pageRows.rows.push_back(readPost(*rows));
                    }
                    return pageRows;
                });
    }

    AsynGyanis::Core::Task<std::vector<PostRecord>> OrgRepository::exportPosts(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status)
    {
        co_return co_await m_database->withConnection<std::vector<PostRecord>>(
                loop,
                [keyword = std::move(keyword), status = std::move(status), tenant = m_tenant](DatabaseConnection &connection) -> std::vector<PostRecord>
                {
                    std::string                filter = "WHERE p.deleted = 0";
                    std::vector<DatabaseValue> parameters;
                    filter += tenantClause(tenant, parameters, "p.tenant_id");
                    if (!keyword.empty())
                    {
                        const std::string pattern = "%" + keyword + "%";
                        filter += " AND (p.post_name LIKE ? OR p.post_code LIKE ?)";
                        parameters.emplace_back(pattern);
                        parameters.emplace_back(pattern);
                    }
                    if (status == "0" || status == "1")
                    {
                        filter += " AND p.status = ?";
                        parameters.emplace_back(static_cast<std::int64_t>(status == "1" ? 1 : 0));
                    }
                    const auto              rows = Sql::executeOrThrow(connection, std::string{kSelectPost} + filter + " ORDER BY p.sort, p.id", parameters);
                    std::vector<PostRecord> result;
                    while (rows->next())
                    {
                        result.push_back(readPost(*rows));
                    }
                    return result;
                });
    }

    AsynGyanis::Core::Task<std::vector<PostRecord>> OrgRepository::postOptions(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await m_database->withConnection<std::vector<PostRecord>>(
                loop,
                [tenant = m_tenant](DatabaseConnection &connection) -> std::vector<PostRecord>
                {
                    std::vector<DatabaseValue> parameters;
                    const std::string          clause = tenantClause(tenant, parameters, "p.tenant_id");
                    const auto                 rows =
                            Sql::executeOrThrow(connection, std::string{kSelectPost} + "WHERE p.deleted = 0 AND p.status = 1" + clause + " ORDER BY p.sort, p.id", parameters);
                    std::vector<PostRecord> result;
                    while (rows->next())
                    {
                        result.push_back(readPost(*rows));
                    }
                    return result;
                });
    }

    AsynGyanis::Core::Task<bool> OrgRepository::postCodeExists(AsynGyanis::Core::EventLoop &loop, std::string code, const std::int64_t excludeId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [code = std::move(code), excludeId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{code, excludeId};
                    const std::string          clause = tenantClause(tenant, parameters, "tenant_id");
                    const auto                 rows =
                            Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_post WHERE post_code = ? AND id <> ? AND deleted = 0" + clause, parameters);
                    return rows->next() && Sql::integer(*rows, "total") > 0;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> OrgRepository::createPost(AsynGyanis::Core::EventLoop &loop, const PostWrite &write, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [write, operatorId, tenant = m_tenant](DatabaseConnection &connection) -> std::int64_t
                {
                    const std::int64_t                  id  = Common::nextIdentifier();
                    const std::string                   now = Common::nowDateTimeUtc();
                    const std::array<DatabaseValue, 12> parameters{id,
                                                                   tenant.tenantId,
                                                                   write.code.value_or(std::string{}),
                                                                   write.name.value_or(std::string{}),
                                                                   static_cast<std::int64_t>(write.sort.value_or(0)),
                                                                   static_cast<std::int64_t>(write.status.value_or(1)),
                                                                   write.remark.value_or(std::string{}),
                                                                   now,
                                                                   now,
                                                                   operatorId,
                                                                   operatorId,
                                                                   std::int64_t{0}};
                    Sql::executeOrThrowVoid(
                            connection,
                            "INSERT INTO sys_post (id, tenant_id, post_code, post_name, sort, status, remark, created_at, updated_at, created_by, updated_by, deleted) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                            parameters);
                    return id;
                });
    }

    AsynGyanis::Core::Task<bool> OrgRepository::updatePost(AsynGyanis::Core::EventLoop &loop, const std::int64_t postId, const PostWrite &write, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [postId, write, operatorId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                                                            {
                                                                std::string                sql = "UPDATE sys_post SET updated_at = ?, updated_by = ?";
                                                                std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), operatorId};
                                                                if (write.code.has_value())
                                                                {
                                                                    sql += ", post_code = ?";
                                                                    parameters.emplace_back(*write.code);
                                                                }
                                                                if (write.name.has_value())
                                                                {
                                                                    sql += ", post_name = ?";
                                                                    parameters.emplace_back(*write.name);
                                                                }
                                                                if (write.sort.has_value())
                                                                {
                                                                    sql += ", sort = ?";
                                                                    parameters.emplace_back(static_cast<std::int64_t>(*write.sort));
                                                                }
                                                                if (write.status.has_value())
                                                                {
                                                                    sql += ", status = ?";
                                                                    parameters.emplace_back(static_cast<std::int64_t>(*write.status));
                                                                }
                                                                if (write.remark.has_value())
                                                                {
                                                                    sql += ", remark = ?";
                                                                    parameters.emplace_back(*write.remark);
                                                                }
                                                                sql += " WHERE id = ? AND deleted = 0";
                                                                parameters.emplace_back(postId);
                                                                sql += tenantClause(tenant, parameters, "tenant_id");
                                                                const auto rows = Sql::executeOrThrow(connection, sql, parameters);
                                                                return rows->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<bool> OrgRepository::removePost(AsynGyanis::Core::EventLoop &loop, const std::int64_t postId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [postId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), postId};
                    const std::string          clause = tenantClause(tenant, parameters, "tenant_id");
                    const auto rows = Sql::executeOrThrow(connection, "UPDATE sys_post SET deleted = 1, updated_at = ? WHERE id = ? AND deleted = 0" + clause, parameters);
                    return rows->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<bool> OrgRepository::postHasUsers(AsynGyanis::Core::EventLoop &loop, const std::int64_t postId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [postId](DatabaseConnection &connection) -> bool
                                                            {
                                                                const std::array<DatabaseValue, 1> parameters{postId};
                                                                const auto                         rows =
                                                                        Sql::executeOrThrow(connection,
                                                                                            "SELECT COUNT(*) AS total FROM sys_user_post up JOIN sys_user u ON u.id = up.user_id "
                                                                                            "WHERE up.post_id = ? AND u.deleted = 0",
                                                                                            parameters);
                                                                return rows->next() && Sql::integer(*rows, "total") > 0;
                                                            });
    }
} // namespace EdgePulse::System
