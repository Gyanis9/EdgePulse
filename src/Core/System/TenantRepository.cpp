#include "Core/System/TenantRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"

#include <array>
#include <cstdint>
#include <memory>
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

        [[nodiscard]] TenantRecord readTenant(const DatabaseResult &result)
        {
            TenantRecord tenant;
            tenant.id         = Sql::integer(result, "id");
            tenant.code       = Sql::text(result, "tenant_code");
            tenant.name       = Sql::text(result, "tenant_name");
            tenant.status     = static_cast<int>(Sql::integer(result, "status"));
            tenant.remark     = Sql::text(result, "remark");
            tenant.createTime = Sql::dateTimeText(result, "created_at");
            return tenant;
        }

        [[nodiscard]] bool validCode(const std::string_view code)
        {
            if (code.empty() || code.size() > 32 || code.front() < 'a' || code.front() > 'z')
            {
                return false;
            }
            for (const char character: code)
            {
                if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_'))
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] std::int64_t countOf(DatabaseConnection &connection, const std::string &sql, const std::span<const DatabaseValue> parameters)
        {
            const std::unique_ptr<DatabaseResult> result = connection.execute(sql, parameters);
            return result && result->next() ? Sql::integer(*result, "total") : -1;
        }
    } // namespace

    TenantRepository::TenantRepository(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<Common::PageRows<TenantRecord>> TenantRepository::list(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status,
                                                                                  const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<TenantRecord>>(
                loop,
                [keyword = std::move(keyword), status = std::move(status), page](DatabaseConnection &connection) -> Common::PageRows<TenantRecord>
                {
                    std::vector<DatabaseValue> parameters;
                    std::string                filter = " WHERE 1 = 1";
                    if (!keyword.empty())
                    {
                        filter += " AND (tenant_code LIKE ? OR tenant_name LIKE ?)";
                        parameters.emplace_back("%" + keyword + "%");
                        parameters.emplace_back("%" + keyword + "%");
                    }
                    if (!status.empty())
                    {
                        filter += " AND status = ?";
                        parameters.emplace_back(static_cast<std::int64_t>(std::stoll(status)));
                    }
                    Common::PageRows<TenantRecord> pageRows;
                    const auto                     count = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_tenant" + filter, parameters);
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
                    const auto rows = Sql::executeOrThrow(
                            connection, "SELECT id, tenant_code, tenant_name, status, remark, created_at FROM sys_tenant" + filter + " ORDER BY id DESC LIMIT ? OFFSET ?",
                            parameters);
                    while (rows->next())
                    {
                        pageRows.rows.push_back(readTenant(*rows));
                    }
                    return pageRows;
                });
    }

    AsynGyanis::Core::Task<std::optional<TenantRecord>> TenantRepository::findByCode(AsynGyanis::Core::EventLoop &loop, std::string code)
    {
        co_return co_await m_database->withConnection<std::optional<TenantRecord>>(
                loop,
                [code = std::move(code)](DatabaseConnection &connection) -> std::optional<TenantRecord>
                {
                    const std::array<DatabaseValue, 1> parameters{code};
                    const auto                         rows =
                            Sql::executeOrThrow(connection, "SELECT id, tenant_code, tenant_name, status, remark, created_at FROM sys_tenant WHERE tenant_code = ?", parameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    return readTenant(*rows);
                });
    }

    AsynGyanis::Core::Task<std::optional<TenantRecord>> TenantRepository::findById(AsynGyanis::Core::EventLoop &loop, const std::int64_t id)
    {
        co_return co_await m_database->withConnection<std::optional<TenantRecord>>(
                loop,
                [id](DatabaseConnection &connection) -> std::optional<TenantRecord>
                {
                    const std::array<DatabaseValue, 1> parameters{id};
                    const auto rows = Sql::executeOrThrow(connection, "SELECT id, tenant_code, tenant_name, status, remark, created_at FROM sys_tenant WHERE id = ?", parameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    return readTenant(*rows);
                });
    }

    AsynGyanis::Core::Task<std::expected<TenantProvisionResult, std::string>> TenantRepository::provision(AsynGyanis::Core::EventLoop &loop, TenantProvisionRequest request,
                                                                                                          const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<std::expected<TenantProvisionResult, std::string>>(
                loop,
                [request = std::move(request), operatorId](DatabaseConnection &connection) -> std::expected<TenantProvisionResult, std::string>
                {
                    if (!validCode(request.code))
                    {
                        return std::unexpected("租户编码必须以小写字母开头，只含小写字母、数字与下划线，长度不超过 32");
                    }
                    if (request.adminUsername.empty() || request.adminPasswordHash.empty())
                    {
                        return std::unexpected("租户管理员账号与口令不能为空");
                    }
                    if (request.name.empty() || request.name.size() > 128)
                    {
                        return std::unexpected("租户名称必须是 1 到 128 个字符");
                    }
                    if (request.adminUsername.size() > 64)
                    {
                        return std::unexpected("租户管理员登录名不能超过 64 个字符");
                    }
                    if (countOf(connection, "SELECT COUNT(*) AS total FROM sys_tenant WHERE tenant_code = ?", std::array<DatabaseValue, 1>{request.code}) > 0)
                    {
                        return std::unexpected("租户编码已存在");
                    }
                    if (countOf(connection, "SELECT COUNT(*) AS total FROM sys_user WHERE username = ?", std::array<DatabaseValue, 1>{request.adminUsername}) > 0)
                    {
                        return std::unexpected("租户管理员登录名已存在");
                    }

                    const std::int64_t tenantId = Common::nextIdentifier();
                    const std::int64_t userId   = Common::nextIdentifier();
                    const std::int64_t deptId   = Common::nextIdentifier();
                    const std::int64_t roleId   = Common::nextIdentifier();
                    const std::string  now      = Common::nowDateTimeUtc();
                    if (!connection.execute("BEGIN"))
                    {
                        return std::unexpected("租户初始化失败：无法开启事务");
                    }
                    const auto rollback = [&connection](const std::string &message) -> std::expected<TenantProvisionResult, std::string>
                    {
                        static_cast<void>(connection.execute("ROLLBACK"));
                        return std::unexpected(message);
                    };
                    const std::array<DatabaseValue, 7> tenantParameters{tenantId, request.code, request.name, std::int64_t{1}, request.remark, now, now};
                    if (!connection.execute("INSERT INTO sys_tenant (id, tenant_code, tenant_name, status, remark, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                                            tenantParameters))
                    {
                        return rollback("写入租户失败：" + connection.lastError());
                    }
                    const std::array<DatabaseValue, 7> deptParameters{deptId, tenantId, std::string{"租户根部门"}, now, now, operatorId, operatorId};
                    if (!connection.execute("INSERT INTO sys_dept (id, tenant_id, parent_id, name, sort, status, deleted, created_at, updated_at, created_by, updated_by) "
                                            "VALUES (?, ?, 0, ?, 0, 1, 0, ?, ?, ?, ?)",
                                            deptParameters))
                    {
                        return rollback("写入租户根部门失败：" + connection.lastError());
                    }
                    const std::string                   roleCode = "tenant_admin_" + request.code;
                    const std::array<DatabaseValue, 10> roleParameters{roleId,     tenantId,  std::string{"租户管理员"}, roleCode, std::int64_t{0}, std::int64_t{1}, now, now,
                                                                       operatorId, operatorId};
                    if (!connection.execute(
                                "INSERT INTO sys_role (id, tenant_id, role_name, role_code, data_scope, builtin, status, deleted, created_at, updated_at, created_by, updated_by) "
                                "VALUES (?, ?, ?, ?, 'ALL', ?, ?, 0, ?, ?, ?, ?)",
                                roleParameters))
                    {
                        return rollback("写入租户管理员角色失败：" + connection.lastError());
                    }
                    if (!connection.execute("INSERT INTO sys_role_menu (role_id, menu_id) SELECT ?, menu_id FROM sys_role_menu WHERE role_id = 3",
                                            std::array<DatabaseValue, 1>{roleId}))
                    {
                        return rollback("复制租户管理员菜单失败：" + connection.lastError());
                    }
                    const std::array<DatabaseValue, 9> userParameters{userId, tenantId,  request.adminUsername, request.adminPasswordHash, request.adminNickname, deptId, now,
                                                                      now,    operatorId};
                    if (!connection.execute(
                                "INSERT INTO sys_user (id, tenant_id, username, password, nickname, dept_id, status, deleted, platform_admin, created_at, updated_at, created_by) "
                                "VALUES (?, ?, ?, ?, ?, ?, 1, 0, 0, ?, ?, ?)",
                                userParameters))
                    {
                        return rollback("写入租户管理员账号失败：" + connection.lastError());
                    }
                    const std::array<DatabaseValue, 2> userRoleParameters{userId, roleId};
                    if (!connection.execute("INSERT INTO sys_user_role (user_id, role_id) VALUES (?, ?)", userRoleParameters))
                    {
                        return rollback("绑定租户管理员角色失败：" + connection.lastError());
                    }
                    if (!connection.execute("COMMIT"))
                    {
                        return rollback("提交租户初始化失败：" + connection.lastError());
                    }
                    return TenantProvisionResult{tenantId, userId};
                });
    }

    AsynGyanis::Core::Task<bool> TenantRepository::update(AsynGyanis::Core::EventLoop &loop, const std::int64_t id, std::string name, const int status, std::string remark)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [id, name = std::move(name), status, remark = std::move(remark)](DatabaseConnection &connection) -> bool
                                                            {
                                                                std::string                sql = "UPDATE sys_tenant SET updated_at = ?";
                                                                std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc()};
                                                                if (!name.empty())
                                                                {
                                                                    sql += ", tenant_name = ?";
                                                                    parameters.emplace_back(name);
                                                                }
                                                                if (status >= 0)
                                                                {
                                                                    sql += ", status = ?";
                                                                    parameters.emplace_back(static_cast<std::int64_t>(status));
                                                                }
                                                                sql += ", remark = ? WHERE id = ?";
                                                                parameters.emplace_back(remark);
                                                                parameters.emplace_back(id);
                                                                return Sql::executeOrThrow(connection, sql, parameters)->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<bool> TenantRepository::codeExists(AsynGyanis::Core::EventLoop &loop, std::string code)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [code = std::move(code)](DatabaseConnection &connection) -> bool
                                                            {
                                                                const std::array<DatabaseValue, 1> parameters{code};
                                                                const auto                         total =
                                                                        countOf(connection, "SELECT COUNT(*) AS total FROM sys_tenant WHERE tenant_code = ?", parameters);
                                                                return total > 0;
                                                            });
    }
} // namespace EdgePulse::System
