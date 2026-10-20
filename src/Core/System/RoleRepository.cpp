#include "Core/System/RoleRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::System
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        constexpr std::string_view kSelectRole =
                "SELECT r.id, r.role_name, r.role_code, r.data_scope, r.sort, r.status, r.builtin, r.created_at, "
                "COALESCE((SELECT COUNT(*) FROM sys_user_role ur JOIN sys_user u ON u.id = ur.user_id AND u.deleted = 0 WHERE ur.role_id = r.id), 0) AS user_count "
                "FROM sys_role r ";

        [[nodiscard]] RoleRecord readRole(const DatabaseResult &result)
        {
            RoleRecord role;
            role.id         = Sql::integer(result, "id");
            role.name       = Sql::text(result, "role_name");
            role.code       = Sql::text(result, "role_code");
            role.dataScope  = Sql::text(result, "data_scope");
            role.sort       = static_cast<int>(Sql::integer(result, "sort"));
            role.status     = static_cast<int>(Sql::integer(result, "status"));
            role.builtin    = Sql::boolean(result, "builtin");
            role.userCount  = Sql::integer(result, "user_count");
            role.createTime = Sql::dateTimeText(result, "created_at");
            return role;
        }

        void replaceMenus(DatabaseConnection &connection, const std::int64_t roleId, const std::vector<std::int64_t> &menuIds)
        {
            const std::array<DatabaseValue, 1> roleParameter{roleId};
            Sql::executeOrThrowVoid(connection, "DELETE FROM sys_role_menu WHERE role_id = ?", roleParameter);
            for (const std::int64_t menuId: menuIds)
            {
                const std::array<DatabaseValue, 2> parameters{roleId, menuId};
                Sql::executeOrThrowVoid(connection, "INSERT INTO sys_role_menu (role_id, menu_id) VALUES (?, ?)", parameters);
            }
        }

        void replaceDepts(DatabaseConnection &connection, const std::int64_t roleId, const std::vector<std::int64_t> &deptIds)
        {
            const std::array<DatabaseValue, 1> roleParameter{roleId};
            Sql::executeOrThrowVoid(connection, "DELETE FROM sys_role_dept WHERE role_id = ?", roleParameter);
            for (const std::int64_t deptId: deptIds)
            {
                const std::array<DatabaseValue, 2> parameters{roleId, deptId};
                Sql::executeOrThrowVoid(connection, "INSERT INTO sys_role_dept (role_id, dept_id) VALUES (?, ?)", parameters);
            }
        }
    } // namespace

    RoleRepository::RoleRepository(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<Common::PageRows<RoleRecord>> RoleRepository::list(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status,
                                                                              const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<RoleRecord>>(
                loop,
                [keyword = std::move(keyword), status = std::move(status), page](DatabaseConnection &connection) -> Common::PageRows<RoleRecord>
                {
                    std::string                filter = "WHERE r.deleted = 0";
                    std::vector<DatabaseValue> parameters;
                    if (!keyword.empty())
                    {
                        const std::string pattern = "%" + keyword + "%";
                        filter += " AND (r.role_name LIKE ? OR r.role_code LIKE ?)";
                        parameters.emplace_back(pattern);
                        parameters.emplace_back(pattern);
                    }
                    if (status == "0" || status == "1")
                    {
                        filter += " AND r.status = ?";
                        parameters.emplace_back(static_cast<std::int64_t>(status == "1" ? 1 : 0));
                    }
                    const auto                   count = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_role r " + filter, parameters);
                    Common::PageRows<RoleRecord> pageRows;
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
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectRole} + filter + " ORDER BY r.sort, r.id LIMIT ? OFFSET ?", parameters);
                    while (rows->next())
                    {
                        pageRows.rows.push_back(readRole(*rows));
                    }
                    return pageRows;
                });
    }

    AsynGyanis::Core::Task<std::vector<RoleRecord>> RoleRepository::exportAll(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status)
    {
        co_return co_await m_database->withConnection<std::vector<RoleRecord>>(
                loop,
                [keyword = std::move(keyword), status = std::move(status)](DatabaseConnection &connection) -> std::vector<RoleRecord>
                {
                    std::string                filter = "WHERE r.deleted = 0";
                    std::vector<DatabaseValue> parameters;
                    if (!keyword.empty())
                    {
                        const std::string pattern = "%" + keyword + "%";
                        filter += " AND (r.role_name LIKE ? OR r.role_code LIKE ?)";
                        parameters.emplace_back(pattern);
                        parameters.emplace_back(pattern);
                    }
                    if (status == "0" || status == "1")
                    {
                        filter += " AND r.status = ?";
                        parameters.emplace_back(static_cast<std::int64_t>(status == "1" ? 1 : 0));
                    }
                    parameters.emplace_back(std::int64_t{10000});
                    const auto              rows = Sql::executeOrThrow(connection, std::string{kSelectRole} + filter + " ORDER BY r.sort, r.id LIMIT ?", parameters);
                    std::vector<RoleRecord> result;
                    while (rows->next())
                    {
                        result.push_back(readRole(*rows));
                    }
                    return result;
                });
    }

    AsynGyanis::Core::Task<std::optional<RoleRecord>> RoleRepository::findById(AsynGyanis::Core::EventLoop &loop, const std::int64_t roleId)
    {
        co_return co_await m_database->withConnection<std::optional<RoleRecord>>(loop,
                                                                                 [roleId](DatabaseConnection &connection) -> std::optional<RoleRecord>
                                                                                 {
                                                                                     const std::array<DatabaseValue, 1> parameters{roleId};
                                                                                     const auto                         rows = Sql::executeOrThrow(
                                                                                             connection, std::string{kSelectRole} + "WHERE r.id = ? AND r.deleted = 0", parameters);
                                                                                     if (!rows->next())
                                                                                     {
                                                                                         return std::nullopt;
                                                                                     }
                                                                                     return readRole(*rows);
                                                                                 });
    }

    AsynGyanis::Core::Task<bool> RoleRepository::codeExists(AsynGyanis::Core::EventLoop &loop, std::string code, const std::int64_t excludeRoleId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [code = std::move(code), excludeRoleId](DatabaseConnection &connection) -> bool
                {
                    const std::array<DatabaseValue, 2> parameters{code, excludeRoleId};
                    const auto rows = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_role WHERE role_code = ? AND id <> ? AND deleted = 0", parameters);
                    return rows->next() && Sql::integer(*rows, "total") > 0;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> RoleRepository::create(AsynGyanis::Core::EventLoop &loop, const RoleWrite &write, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [write, operatorId](DatabaseConnection &connection) -> std::int64_t
                {
                    const std::int64_t                  id  = Common::nextIdentifier();
                    const std::string                   now = Common::nowDateTimeUtc();
                    const std::array<DatabaseValue, 11> parameters{id,
                                                                   write.name.value_or(std::string{}),
                                                                   write.code.value_or(std::string{}),
                                                                   write.dataScope.value_or(std::string{"SELF"}),
                                                                   static_cast<std::int64_t>(write.sort.value_or(0)),
                                                                   static_cast<std::int64_t>(write.status.value_or(1)),
                                                                   static_cast<std::int64_t>(0),
                                                                   write.remark.value_or(std::string{}),
                                                                   now,
                                                                   now,
                                                                   operatorId};
                    Sql::executeOrThrowVoid(connection,
                                            "INSERT INTO sys_role (id, role_name, role_code, data_scope, sort, status, builtin, remark, created_at, updated_at, created_by) "
                                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                            parameters);
                    replaceMenus(connection, id, write.menuIds.value_or(std::vector<std::int64_t>{}));
                    replaceDepts(connection, id, write.deptIds.value_or(std::vector<std::int64_t>{}));
                    return id;
                });
    }

    AsynGyanis::Core::Task<bool> RoleRepository::update(AsynGyanis::Core::EventLoop &loop, const std::int64_t roleId, const RoleWrite &write, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [roleId, write, operatorId](DatabaseConnection &connection) -> bool
                                                            {
                                                                std::string                sql = "UPDATE sys_role SET updated_at = ?, updated_by = ?";
                                                                std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), operatorId};
                                                                if (write.name.has_value())
                                                                {
                                                                    sql += ", role_name = ?";
                                                                    parameters.emplace_back(*write.name);
                                                                }
                                                                if (write.code.has_value())
                                                                {
                                                                    sql += ", role_code = ?";
                                                                    parameters.emplace_back(*write.code);
                                                                }
                                                                if (write.dataScope.has_value())
                                                                {
                                                                    sql += ", data_scope = ?";
                                                                    parameters.emplace_back(*write.dataScope);
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
                                                                parameters.emplace_back(roleId);
                                                                const auto updated = Sql::executeOrThrow(connection, sql, parameters);
                                                                if (updated->affectedRowCount() == 0)
                                                                {
                                                                    return false;
                                                                }
                                                                if (write.menuIds.has_value())
                                                                {
                                                                    replaceMenus(connection, roleId, *write.menuIds);
                                                                }
                                                                if (write.deptIds.has_value())
                                                                {
                                                                    replaceDepts(connection, roleId, *write.deptIds);
                                                                }
                                                                return true;
                                                            });
    }

    AsynGyanis::Core::Task<bool> RoleRepository::remove(AsynGyanis::Core::EventLoop &loop, const std::int64_t roleId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [roleId](DatabaseConnection &connection) -> bool
                                                            {
                                                                const std::array<DatabaseValue, 2> parameters{Common::nowDateTimeUtc(), roleId};
                                                                const auto                         rows = Sql::executeOrThrow(
                                                                        connection, "UPDATE sys_role SET deleted = 1, updated_at = ? WHERE id = ? AND deleted = 0", parameters);
                                                                if (rows->affectedRowCount() == 0)
                                                                {
                                                                    return false;
                                                                }
                                                                const std::array<DatabaseValue, 1> roleParameter{roleId};
                                                                Sql::executeOrThrowVoid(connection, "DELETE FROM sys_role_menu WHERE role_id = ?", roleParameter);
                                                                Sql::executeOrThrowVoid(connection, "DELETE FROM sys_user_role WHERE role_id = ?", roleParameter);
                                                                Sql::executeOrThrowVoid(connection, "DELETE FROM sys_role_dept WHERE role_id = ?", roleParameter);
                                                                return true;
                                                            });
    }

    AsynGyanis::Core::Task<std::vector<std::int64_t>> RoleRepository::menuIdsFor(AsynGyanis::Core::EventLoop &loop, const std::int64_t roleId)
    {
        co_return co_await m_database->withConnection<std::vector<std::int64_t>>(
                loop,
                [roleId](DatabaseConnection &connection) -> std::vector<std::int64_t>
                {
                    const std::array<DatabaseValue, 1> parameters{roleId};
                    const auto                         rows = Sql::executeOrThrow(connection, "SELECT menu_id FROM sys_role_menu WHERE role_id = ? ORDER BY menu_id", parameters);
                    std::vector<std::int64_t>          ids;
                    while (rows->next())
                    {
                        ids.push_back(Sql::integer(*rows, "menu_id"));
                    }
                    return ids;
                });
    }

    AsynGyanis::Core::Task<std::vector<std::int64_t>> RoleRepository::deptIdsFor(AsynGyanis::Core::EventLoop &loop, const std::int64_t roleId)
    {
        co_return co_await m_database->withConnection<std::vector<std::int64_t>>(
                loop,
                [roleId](DatabaseConnection &connection) -> std::vector<std::int64_t>
                {
                    const std::array<DatabaseValue, 1> parameters{roleId};
                    const auto                         rows = Sql::executeOrThrow(connection, "SELECT dept_id FROM sys_role_dept WHERE role_id = ? ORDER BY dept_id", parameters);
                    std::vector<std::int64_t>          ids;
                    while (rows->next())
                    {
                        ids.push_back(Sql::integer(*rows, "dept_id"));
                    }
                    return ids;
                });
    }

    AsynGyanis::Core::Task<bool> RoleRepository::updateMenus(AsynGyanis::Core::EventLoop &loop, const std::int64_t roleId, std::vector<std::int64_t> menuIds)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [roleId, menuIds = std::move(menuIds)](DatabaseConnection &connection) -> bool
                                                            {
                                                                replaceMenus(connection, roleId, menuIds);
                                                                return true;
                                                            });
    }

    AsynGyanis::Core::Task<std::vector<RoleRecord>> RoleRepository::listOptions(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await m_database->withConnection<std::vector<RoleRecord>>(
                loop,
                [](DatabaseConnection &connection) -> std::vector<RoleRecord>
                {
                    const auto              rows = Sql::executeOrThrow(connection, std::string{kSelectRole} + "WHERE r.deleted = 0 AND r.status = 1 ORDER BY r.sort, r.id");
                    std::vector<RoleRecord> result;
                    while (rows->next())
                    {
                        result.push_back(readRole(*rows));
                    }
                    return result;
                });
    }
} // namespace EdgePulse::System
