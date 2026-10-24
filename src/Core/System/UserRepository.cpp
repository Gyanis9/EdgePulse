#include "Core/System/UserRepository.h"

#include "Base/Database/SqlHelpers.h"
#include "Base/Database/SqlRetry.h"
#include "Base/Exception/EdgePulseException.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"

#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace EdgePulse::System
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        constexpr int                       kLoginFailureAttempts = 4; ///< 并发失败计数最多尝试几次
        constexpr std::chrono::milliseconds kLoginFailureBackoff{5};   ///< 两次尝试之间的退避，只在工作线程上等

        constexpr std::string_view kSelectUser = "SELECT u.id, u.tenant_id, u.platform_admin, u.username, u.password, u.nickname, u.phone, u.email, u.dept_id, u.status, "
                                                 "u.failed_login_count, u.locked_until, "
                                                 "u.last_login_time, u.created_at, COALESCE(t.tenant_code, '') AS tenant_code, COALESCE(t.tenant_name, '') AS tenant_name, "
                                                 "COALESCE(d.name, '') AS dept_name, "
                                                 "COALESCE((SELECT GROUP_CONCAT(DISTINCT r.role_name) FROM sys_user_role ur JOIN sys_role r ON r.id = ur.role_id "
                                                 "WHERE ur.user_id = u.id AND r.deleted = 0), '') AS role_names, "
                                                 "COALESCE((SELECT GROUP_CONCAT(DISTINCT p.post_name) FROM sys_user_post up JOIN sys_post p ON p.id = up.post_id "
                                                 "WHERE up.user_id = u.id AND p.deleted = 0), '') AS post_names, "
                                                 "EXISTS (SELECT 1 FROM sys_user_mfa m WHERE m.user_id = u.id AND m.enabled = 1) AS mfa_enabled "
                                                 "FROM sys_user u LEFT JOIN sys_tenant t ON t.id = u.tenant_id LEFT JOIN sys_dept d ON d.id = u.dept_id ";

        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(DatabaseConnection &connection, const std::string &sql, std::span<const DatabaseValue> parameters)
        {
            std::unique_ptr<DatabaseResult> result = parameters.empty() ? connection.execute(sql) : connection.execute(sql, parameters);
            if (!result)
            {
                throw Common::DatabaseException("执行 SQL 失败：" + connection.lastError());
            }
            return result;
        }

        void executeStatement(DatabaseConnection &connection, const std::string &sql, std::span<const DatabaseValue> parameters)
        {
            static_cast<void>(execute(connection, sql, parameters));
        }

        [[nodiscard]] std::unique_ptr<DatabaseResult> executeWithRetry(DatabaseConnection &connection, const std::string &sql, std::span<const DatabaseValue> parameters)
        {
            for (int attempt = 0; attempt < kLoginFailureAttempts; ++attempt)
            {
                try
                {
                    return execute(connection, sql, parameters);
                } catch (const Common::DatabaseException &failure)
                {
                    if (!Sql::isRetryableSqlFailure(failure.what()) || attempt + 1 >= kLoginFailureAttempts)
                    {
                        throw;
                    }
                    // 工作线程上的短暂退避，避开并发写持有的锁；事件循环线程不经过这里
                    std::this_thread::sleep_for(kLoginFailureBackoff);
                }
            }
            throw Common::DatabaseException("登录失败计数重试次数已用尽");
        }

        void executeStatementWithRetry(DatabaseConnection &connection, const std::string &sql, std::span<const DatabaseValue> parameters)
        {
            static_cast<void>(executeWithRetry(connection, sql, parameters));
        }

        [[nodiscard]] UserRecord readUser(const DatabaseResult &result)
        {
            UserRecord user;
            user.id               = Sql::integer(result, "id");
            user.tenantId         = Sql::integer(result, "tenant_id");
            user.platformAdmin    = Sql::boolean(result, "platform_admin");
            user.tenantCode       = Sql::text(result, "tenant_code");
            user.tenantName       = Sql::text(result, "tenant_name");
            user.username         = Sql::text(result, "username");
            user.passwordHash     = Sql::text(result, "password");
            user.nickname         = Sql::text(result, "nickname");
            user.phone            = Sql::text(result, "phone");
            user.email            = Sql::text(result, "email");
            user.departmentId     = Sql::integer(result, "dept_id");
            user.departmentName   = Sql::text(result, "dept_name");
            user.status           = static_cast<int>(Sql::integer(result, "status"));
            user.mfaEnabled       = Sql::boolean(result, "mfa_enabled");
            user.failedLoginCount = static_cast<int>(Sql::integer(result, "failed_login_count"));
            user.lockedUntil      = Sql::dateTimeText(result, "locked_until");
            user.lastLoginTime    = Sql::dateTimeText(result, "last_login_time");
            user.createTime       = Sql::dateTimeText(result, "created_at");
            user.roleNames        = Sql::text(result, "role_names");
            user.postNames        = Sql::text(result, "post_names");
            return user;
        }

        /**
         * @brief 拼用户过滤条件
         * @param query 过滤条件
         * @param parameters 输出参数
         * @return std::string WHERE 子句
         */
        [[nodiscard]] std::string buildFilter(const UserQuery &query, std::vector<DatabaseValue> &parameters, const TenantScope &tenant)
        {
            std::string filter = " WHERE u.deleted = 0";
            if (!tenant.allTenants)
            {
                filter += " AND u.tenant_id = ?";
                parameters.emplace_back(tenant.tenantId);
            }
            if (!query.keyword.empty())
            {
                const std::string pattern = "%" + query.keyword + "%";
                filter += " AND (u.username LIKE ? OR u.nickname LIKE ? OR u.phone LIKE ?)";
                parameters.emplace_back(pattern);
                parameters.emplace_back(pattern);
                parameters.emplace_back(pattern);
            }
            if (query.status == "0" || query.status == "1")
            {
                filter += " AND u.status = ?";
                parameters.emplace_back(static_cast<std::int64_t>(query.status == "1" ? 1 : 0));
            }
            if (query.deptId > 0)
            {
                filter += " AND u.dept_id = ?";
                parameters.emplace_back(query.deptId);
            }
            if (query.selfOnly)
            {
                filter += " AND u.id = ?";
                parameters.emplace_back(query.userId);
            } else if (!query.allowedDeptIds.empty())
            {
                filter += " AND u.dept_id IN (";
                for (std::size_t index = 0; index < query.allowedDeptIds.size(); ++index)
                {
                    filter += index == 0 ? "?" : ", ?";
                    parameters.emplace_back(query.allowedDeptIds[index]);
                }
                filter += ")";
            }
            return filter;
        }

        void replaceRoleRelations(DatabaseConnection &connection, const std::int64_t userId, const std::vector<std::int64_t> &roleIds)
        {
            const std::array<DatabaseValue, 1> userParameter{userId};
            executeStatement(connection, "DELETE FROM sys_user_role WHERE user_id = ?", userParameter);
            for (const std::int64_t roleId: roleIds)
            {
                const std::array<DatabaseValue, 2> parameters{userId, roleId};
                executeStatement(connection, "INSERT INTO sys_user_role (user_id, role_id) VALUES (?, ?)", parameters);
            }
        }

        void replacePostRelations(DatabaseConnection &connection, const std::int64_t userId, const std::vector<std::int64_t> &postIds)
        {
            const std::array<DatabaseValue, 1> userParameter{userId};
            executeStatement(connection, "DELETE FROM sys_user_post WHERE user_id = ?", userParameter);
            for (const std::int64_t postId: postIds)
            {
                const std::array<DatabaseValue, 2> parameters{userId, postId};
                executeStatement(connection, "INSERT INTO sys_user_post (user_id, post_id) VALUES (?, ?)", parameters);
            }
        }

        [[nodiscard]] bool belongsToTenant(DatabaseConnection &connection, const std::string_view table, const std::int64_t id, const TenantScope &tenant)
        {
            if (id <= 0)
            {
                return false;
            }
            std::vector<DatabaseValue> parameters{id};
            std::string                sql = "SELECT COUNT(*) AS total FROM " + std::string{table} + " WHERE id = ? AND deleted = 0";
            if (!tenant.allTenants)
            {
                sql += " AND tenant_id = ?";
                parameters.emplace_back(tenant.tenantId);
            }
            const std::unique_ptr<DatabaseResult> result = connection.execute(sql, parameters);
            return result && result->next() && Sql::integer(*result, "total") > 0;
        }
    } // namespace

    UserRepository::UserRepository(std::shared_ptr<Common::Database> database, TenantScope tenant) : m_database(std::move(database)), m_tenant(tenant)
    {
    }

    std::shared_ptr<UserRepository> UserRepository::scoped(const TenantScope tenant) const
    {
        return std::make_shared<UserRepository>(m_database, tenant);
    }

    AsynGyanis::Core::Task<std::optional<UserRecord>> UserRepository::findByUsername(AsynGyanis::Core::EventLoop &loop, std::string username)
    {
        co_return co_await m_database->withConnection<std::optional<UserRecord>>(
                loop,
                [username = std::move(username), tenant = m_tenant](DatabaseConnection &connection) -> std::optional<UserRecord>
                {
                    std::vector<DatabaseValue> parameters{username};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND u.tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto result = execute(connection, std::string{kSelectUser} + "WHERE u.username = ? AND u.deleted = 0" + clause, parameters);
                    if (!result->next())
                    {
                        return std::nullopt;
                    }
                    return readUser(*result);
                });
    }

    AsynGyanis::Core::Task<std::optional<UserRecord>> UserRepository::findByEmail(AsynGyanis::Core::EventLoop &loop, std::string email)
    {
        co_return co_await m_database->withConnection<std::optional<UserRecord>>(
                loop,
                [email = std::move(email), tenant = m_tenant](DatabaseConnection &connection) -> std::optional<UserRecord>
                {
                    std::vector<DatabaseValue> parameters{email};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND u.tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto result = execute(connection, std::string{kSelectUser} + "WHERE u.email = ? AND u.deleted = 0" + clause, parameters);
                    if (!result->next())
                    {
                        return std::nullopt;
                    }
                    return readUser(*result);
                });
    }

    AsynGyanis::Core::Task<std::optional<UserRecord>> UserRepository::findById(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        co_return co_await m_database->withConnection<std::optional<UserRecord>>(
                loop,
                [userId, tenant = m_tenant](DatabaseConnection &connection) -> std::optional<UserRecord>
                {
                    std::vector<DatabaseValue> parameters{userId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND u.tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto result = execute(connection, std::string{kSelectUser} + "WHERE u.id = ? AND u.deleted = 0" + clause, parameters);
                    if (!result->next())
                    {
                        return std::nullopt;
                    }
                    return readUser(*result);
                });
    }

    AsynGyanis::Core::Task<Common::PageRows<UserRecord>> UserRepository::list(AsynGyanis::Core::EventLoop &loop, const UserQuery &query, const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<UserRecord>>(
                loop,
                [query, page, tenant = m_tenant](DatabaseConnection &connection) -> Common::PageRows<UserRecord>
                {
                    std::vector<DatabaseValue>   filterParameters;
                    const std::string            filter      = buildFilter(query, filterParameters, tenant);
                    const auto                   countResult = execute(connection, "SELECT COUNT(*) AS total FROM sys_user u" + filter, filterParameters);
                    Common::PageRows<UserRecord> pageRows;
                    if (countResult->next())
                    {
                        pageRows.total = Sql::integer(*countResult, "total");
                    }
                    if (pageRows.total == 0)
                    {
                        return pageRows;
                    }

                    std::vector<DatabaseValue> parameters = filterParameters;
                    parameters.emplace_back(static_cast<std::int64_t>(page.pageSize));
                    parameters.emplace_back(static_cast<std::int64_t>(page.offset()));
                    const auto result = execute(connection, std::string{kSelectUser} + filter + " ORDER BY u.id DESC LIMIT ? OFFSET ?", parameters);
                    while (result->next())
                    {
                        pageRows.rows.push_back(readUser(*result));
                    }
                    return pageRows;
                });
    }

    AsynGyanis::Core::Task<std::vector<UserRecord>> UserRepository::exportAll(AsynGyanis::Core::EventLoop &loop, const UserQuery &query)
    {
        co_return co_await m_database->withConnection<std::vector<UserRecord>>(
                loop,
                [query, tenant = m_tenant](DatabaseConnection &connection) -> std::vector<UserRecord>
                {
                    std::vector<DatabaseValue> parameters;
                    const std::string          filter = buildFilter(query, parameters, tenant);
                    parameters.emplace_back(std::int64_t{10000});
                    const auto              result = execute(connection, std::string{kSelectUser} + filter + " ORDER BY u.id DESC LIMIT ?", parameters);
                    std::vector<UserRecord> rows;
                    while (result->next())
                    {
                        rows.push_back(readUser(*result));
                    }
                    return rows;
                });
    }

    AsynGyanis::Core::Task<bool> UserRepository::usernameExists(AsynGyanis::Core::EventLoop &loop, std::string username, const std::int64_t excludeUserId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [username = std::move(username), excludeUserId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{username, excludeUserId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto result = execute(connection, "SELECT COUNT(*) AS total FROM sys_user WHERE username = ? AND deleted = 0 AND id <> ?" + clause, parameters);
                    return result->next() && Sql::integer(*result, "total") > 0;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> UserRepository::create(AsynGyanis::Core::EventLoop &loop, const UserWrite &write, std::string passwordHash, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [write, passwordHash = std::move(passwordHash), operatorId, tenant = m_tenant](DatabaseConnection &connection) -> std::int64_t
                {
                    if (tenant.allTenants)
                    {
                        return 0;
                    }
                    if (write.deptId.has_value() && !belongsToTenant(connection, "sys_dept", *write.deptId, tenant))
                    {
                        return 0;
                    }
                    for (const std::int64_t roleId: write.roleIds.value_or(std::vector<std::int64_t>{}))
                    {
                        if (!belongsToTenant(connection, "sys_role", roleId, tenant))
                        {
                            return 0;
                        }
                    }
                    for (const std::int64_t postId: write.postIds.value_or(std::vector<std::int64_t>{}))
                    {
                        if (!belongsToTenant(connection, "sys_post", postId, tenant))
                        {
                            return 0;
                        }
                    }
                    const std::int64_t                  userId   = Common::nextIdentifier();
                    const std::string                   now      = Common::nowDateTimeUtc();
                    const std::string                   username = write.username.value_or(std::string{});
                    const std::string                   nickname = write.nickname.value_or(username);
                    const std::array<DatabaseValue, 14> parameters{userId,
                                                                   tenant.tenantId,
                                                                   username,
                                                                   passwordHash,
                                                                   nickname,
                                                                   write.phone.value_or(std::string{}),
                                                                   write.email.value_or(std::string{}),
                                                                   write.deptId.value_or(0),
                                                                   static_cast<std::int64_t>(write.status.value_or(1)),
                                                                   now,
                                                                   now,
                                                                   operatorId,
                                                                   operatorId,
                                                                   std::int64_t{0}};
                    executeStatement(connection,
                                     "INSERT INTO sys_user (id, tenant_id, username, password, nickname, phone, email, dept_id, status, created_at, updated_at, created_by, "
                                     "updated_by, deleted) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                     parameters);
                    replaceRoleRelations(connection, userId, write.roleIds.value_or(std::vector<std::int64_t>{}));
                    replacePostRelations(connection, userId, write.postIds.value_or(std::vector<std::int64_t>{}));
                    return userId;
                });
    }

    AsynGyanis::Core::Task<bool> UserRepository::update(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId, const UserWrite &write, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [userId, write, operatorId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                                                            {
                                                                if (write.deptId.has_value() && !belongsToTenant(connection, "sys_dept", *write.deptId, tenant))
                                                                {
                                                                    return false;
                                                                }
                                                                for (const std::int64_t roleId: write.roleIds.value_or(std::vector<std::int64_t>{}))
                                                                {
                                                                    if (!belongsToTenant(connection, "sys_role", roleId, tenant))
                                                                    {
                                                                        return false;
                                                                    }
                                                                }
                                                                for (const std::int64_t postId: write.postIds.value_or(std::vector<std::int64_t>{}))
                                                                {
                                                                    if (!belongsToTenant(connection, "sys_post", postId, tenant))
                                                                    {
                                                                        return false;
                                                                    }
                                                                }
                                                                std::string                sql = "UPDATE sys_user SET updated_at = ?, updated_by = ?";
                                                                std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), operatorId};
                                                                if (write.username.has_value())
                                                                {
                                                                    sql += ", username = ?";
                                                                    parameters.emplace_back(*write.username);
                                                                }
                                                                if (write.nickname.has_value())
                                                                {
                                                                    sql += ", nickname = ?";
                                                                    parameters.emplace_back(*write.nickname);
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
                                                                if (write.deptId.has_value())
                                                                {
                                                                    sql += ", dept_id = ?";
                                                                    parameters.emplace_back(*write.deptId);
                                                                }
                                                                if (write.status.has_value())
                                                                {
                                                                    sql += ", status = ?";
                                                                    parameters.emplace_back(static_cast<std::int64_t>(*write.status));
                                                                }
                                                                sql += " WHERE id = ? AND deleted = 0";
                                                                parameters.emplace_back(userId);
                                                                if (!tenant.allTenants)
                                                                {
                                                                    sql += " AND tenant_id = ?";
                                                                    parameters.emplace_back(tenant.tenantId);
                                                                }
                                                                const auto updateResult = execute(connection, sql, parameters);
                                                                if (updateResult->affectedRowCount() == 0)
                                                                {
                                                                    return false;
                                                                }
                                                                if (write.roleIds.has_value())
                                                                {
                                                                    replaceRoleRelations(connection, userId, *write.roleIds);
                                                                }
                                                                if (write.postIds.has_value())
                                                                {
                                                                    replacePostRelations(connection, userId, *write.postIds);
                                                                }
                                                                return true;
                                                            });
    }

    AsynGyanis::Core::Task<bool> UserRepository::remove(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [userId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                                                            {
                                                                std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), userId};
                                                                std::string                sql = "UPDATE sys_user SET deleted = 1, updated_at = ? WHERE id = ? AND deleted = 0";
                                                                if (!tenant.allTenants)
                                                                {
                                                                    sql += " AND tenant_id = ?";
                                                                    parameters.emplace_back(tenant.tenantId);
                                                                }
                                                                const auto result = execute(connection, sql, parameters);
                                                                if (result->affectedRowCount() == 0)
                                                                {
                                                                    return false;
                                                                }
                                                                const std::array<DatabaseValue, 1> userParameter{userId};
                                                                executeStatement(connection, "DELETE FROM sys_user_role WHERE user_id = ?", userParameter);
                                                                executeStatement(connection, "DELETE FROM sys_user_post WHERE user_id = ?", userParameter);
                                                                return true;
                                                            });
    }

    AsynGyanis::Core::Task<bool> UserRepository::updateStatus(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId, const int status, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [userId, status, operatorId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                                                            {
                                                                std::vector<DatabaseValue> parameters{static_cast<std::int64_t>(status), Common::nowDateTimeUtc(), operatorId,
                                                                                                      userId};
                                                                std::string sql = "UPDATE sys_user SET status = ?, updated_at = ?, updated_by = ? WHERE id = ? AND deleted = 0";
                                                                if (!tenant.allTenants)
                                                                {
                                                                    sql += " AND tenant_id = ?";
                                                                    parameters.emplace_back(tenant.tenantId);
                                                                }
                                                                const auto result = execute(connection, sql, parameters);
                                                                return result->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<bool> UserRepository::resetPassword(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId, std::string passwordHash,
                                                               const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [userId, passwordHash = std::move(passwordHash), operatorId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{passwordHash, Common::nowDateTimeUtc(), operatorId, userId};
                    // 管理员重置口令同时解除锁定：用户拿不到新口令前的失败次数不应继续生效
                    std::string sql = "UPDATE sys_user SET password = ?, failed_login_count = 0, last_failed_login_at = NULL, locked_until = NULL, "
                                      "updated_at = ?, updated_by = ? WHERE id = ? AND deleted = 0";
                    if (!tenant.allTenants)
                    {
                        sql += " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto result = execute(connection, sql, parameters);
                    return result->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<std::optional<std::string>> UserRepository::recordLoginFailure(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId, const int windowSeconds,
                                                                                          const int maximumFailures, const int lockSeconds)
    {
        co_return co_await m_database->withConnection<std::optional<std::string>>(
                loop,
                [userId, windowSeconds, maximumFailures, lockSeconds, tenant = m_tenant](DatabaseConnection &connection) -> std::optional<std::string>
                {
                    const auto        now          = std::chrono::system_clock::now();
                    const std::string nowText      = Common::formatDateTimeUtc(now);
                    const std::string windowStart  = Common::formatDateTimeUtc(now - std::chrono::seconds{windowSeconds});
                    const std::string tenantClause = tenant.allTenants ? std::string{} : std::string{" AND tenant_id = ?"};

                    std::vector<DatabaseValue> incrementParameters{windowStart, nowText, userId};
                    if (!tenant.allTenants)
                    {
                        incrementParameters.emplace_back(tenant.tenantId);
                    }
                    // 窗口判定与自增在同一条 UPDATE 里完成：并发失败不会各自读到旧计数再加一
                    executeStatementWithRetry(connection,
                                              "UPDATE sys_user SET failed_login_count = CASE WHEN last_failed_login_at IS NULL OR last_failed_login_at < ? "
                                              "THEN 1 ELSE failed_login_count + 1 END, last_failed_login_at = ? WHERE id = ? AND deleted = 0" +
                                                      tenantClause,
                                              incrementParameters);

                    std::vector<DatabaseValue> selectParameters{userId};
                    if (!tenant.allTenants)
                    {
                        selectParameters.emplace_back(tenant.tenantId);
                    }
                    const auto row =
                            executeWithRetry(connection, "SELECT failed_login_count, locked_until FROM sys_user WHERE id = ? AND deleted = 0" + tenantClause, selectParameters);
                    if (!row->next())
                    {
                        return std::nullopt;
                    }
                    const int         failureCount = static_cast<int>(Sql::integer(*row, "failed_login_count"));
                    const std::string currentLock  = Sql::dateTimeText(*row, "locked_until");

                    if (failureCount >= maximumFailures && (currentLock.empty() || currentLock <= nowText))
                    {
                        const std::string          proposedLock = Common::formatDateTimeUtc(now + std::chrono::seconds{lockSeconds});
                        std::vector<DatabaseValue> lockParameters{proposedLock, userId, nowText};
                        if (!tenant.allTenants)
                        {
                            lockParameters.emplace_back(tenant.tenantId);
                        }
                        // 只补写仍然空着或已过期的锁定，不能覆盖并发请求刚写入的新截止时刻
                        executeStatementWithRetry(connection,
                                                  "UPDATE sys_user SET locked_until = ? WHERE id = ? AND deleted = 0 AND (locked_until IS NULL OR locked_until <= ?)" +
                                                          tenantClause,
                                                  lockParameters);
                    }

                    std::vector<DatabaseValue> finalParameters{userId};
                    if (!tenant.allTenants)
                    {
                        finalParameters.emplace_back(tenant.tenantId);
                    }
                    const auto finalRow = executeWithRetry(connection, "SELECT locked_until FROM sys_user WHERE id = ? AND deleted = 0" + tenantClause, finalParameters);
                    if (!finalRow->next())
                    {
                        return std::nullopt;
                    }
                    const std::string finalLock = Sql::dateTimeText(*finalRow, "locked_until");
                    // 只有仍在将来的锁定才回报给调用方；已过期的不算本次触发
                    return !finalLock.empty() && finalLock > nowText ? std::optional<std::string>{finalLock} : std::nullopt;
                });
    }

    AsynGyanis::Core::Task<bool> UserRepository::clearLoginFailures(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [userId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{userId};
                    std::string                sql = "UPDATE sys_user SET failed_login_count = 0, last_failed_login_at = NULL, locked_until = NULL WHERE id = ? AND deleted = 0";
                    if (!tenant.allTenants)
                    {
                        sql += " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto result = execute(connection, sql, parameters);
                    return result->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<std::vector<std::int64_t>> UserRepository::roleIdsFor(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        co_return co_await m_database->withConnection<std::vector<std::int64_t>>(
                loop,
                [userId](DatabaseConnection &connection) -> std::vector<std::int64_t>
                {
                    const std::array<DatabaseValue, 1> parameters{userId};
                    const auto                         result = execute(connection, "SELECT role_id FROM sys_user_role WHERE user_id = ? ORDER BY role_id", parameters);
                    std::vector<std::int64_t>          ids;
                    while (result->next())
                    {
                        ids.push_back(Sql::integer(*result, "role_id"));
                    }
                    return ids;
                });
    }

    AsynGyanis::Core::Task<std::vector<std::int64_t>> UserRepository::postIdsFor(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        co_return co_await m_database->withConnection<std::vector<std::int64_t>>(
                loop,
                [userId](DatabaseConnection &connection) -> std::vector<std::int64_t>
                {
                    const std::array<DatabaseValue, 1> parameters{userId};
                    const auto                         result = execute(connection, "SELECT post_id FROM sys_user_post WHERE user_id = ? ORDER BY post_id", parameters);
                    std::vector<std::int64_t>          ids;
                    while (result->next())
                    {
                        ids.push_back(Sql::integer(*result, "post_id"));
                    }
                    return ids;
                });
    }

    AsynGyanis::Core::Task<std::vector<std::pair<std::int64_t, std::int64_t>>> UserRepository::userRolePairs(AsynGyanis::Core::EventLoop &loop, const int limit)
    {
        co_return co_await m_database->withConnection<std::vector<std::pair<std::int64_t, std::int64_t>>>(
                loop,
                [limit, tenant = m_tenant](DatabaseConnection &connection) -> std::vector<std::pair<std::int64_t, std::int64_t>>
                {
                    std::vector<DatabaseValue> parameters;
                    std::string                tenantClause;
                    if (!tenant.allTenants)
                    {
                        tenantClause = " AND u.tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    parameters.emplace_back(static_cast<std::int64_t>(limit));
                    const auto rows = execute(connection,
                                              "SELECT ur.user_id, ur.role_id FROM sys_user_role ur JOIN sys_user u ON u.id = ur.user_id AND u.deleted = 0" + tenantClause +
                                                      " JOIN sys_role r ON r.id = ur.role_id AND r.deleted = 0 ORDER BY ur.user_id, ur.role_id LIMIT ?",
                                              parameters);
                    std::vector<std::pair<std::int64_t, std::int64_t>> pairs;
                    while (rows->next())
                    {
                        pairs.emplace_back(Sql::integer(*rows, "user_id"), Sql::integer(*rows, "role_id"));
                    }
                    return pairs;
                });
    }

    AsynGyanis::Core::Task<std::vector<std::string>> UserRepository::permissionsFor(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        co_return co_await m_database->withConnection<std::vector<std::string>>(
                loop,
                [userId](DatabaseConnection &connection) -> std::vector<std::string>
                {
                    const std::array<DatabaseValue, 1> parameters{userId};
                    const auto                         result = execute(connection,
                                                                        "SELECT DISTINCT m.permission FROM sys_menu m "
                                                                        "JOIN sys_role_menu rm ON rm.menu_id = m.id "
                                                                        "JOIN sys_user_role ur ON ur.role_id = rm.role_id "
                                                                        "JOIN sys_role r ON r.id = ur.role_id "
                                                                        "WHERE ur.user_id = ? AND r.status = 1 AND r.deleted = 0 AND m.permission <> '' AND m.status = 1 "
                                                                        "ORDER BY m.permission",
                                                                        parameters);
                    std::vector<std::string>           permissions;
                    while (result->next())
                    {
                        if (std::string permission = Sql::text(*result, "permission"); !permission.empty())
                        {
                            permissions.push_back(std::move(permission));
                        }
                    }
                    return permissions;
                });
    }

    AsynGyanis::Core::Task<std::vector<std::string>> UserRepository::rolesFor(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        co_return co_await m_database->withConnection<std::vector<std::string>>(loop,
                                                                                [userId](DatabaseConnection &connection) -> std::vector<std::string>
                                                                                {
                                                                                    const std::array<DatabaseValue, 1> parameters{userId};
                                                                                    const auto result = execute(connection,
                                                                                                                "SELECT r.role_code FROM sys_role r "
                                                                                                                "JOIN sys_user_role ur ON ur.role_id = r.id "
                                                                                                                "WHERE ur.user_id = ? AND r.status = 1 AND r.deleted = 0 "
                                                                                                                "ORDER BY r.role_code",
                                                                                                                parameters);
                                                                                    std::vector<std::string> roles;
                                                                                    while (result->next())
                                                                                    {
                                                                                        if (std::string role = Sql::text(*result, "role_code"); !role.empty())
                                                                                        {
                                                                                            roles.push_back(std::move(role));
                                                                                        }
                                                                                    }
                                                                                    return roles;
                                                                                });
    }

    AsynGyanis::Core::Task<bool> UserRepository::updatePassword(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId, std::string passwordHash)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [userId, passwordHash = std::move(passwordHash)](DatabaseConnection &connection) -> bool
                                                            {
                                                                const std::array<DatabaseValue, 3> parameters{passwordHash, Common::nowDateTimeUtc(), userId};
                                                                const auto                         result = execute(
                                                                        connection, "UPDATE sys_user SET password = ?, updated_at = ? WHERE id = ? AND deleted = 0", parameters);
                                                                return result->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<bool> UserRepository::touchLastLogin(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId, std::string loginTime)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [userId, loginTime = std::move(loginTime)](DatabaseConnection &connection) -> bool
                {
                    const std::array<DatabaseValue, 3> parameters{loginTime, Common::nowDateTimeUtc(), userId};
                    const auto result = execute(connection, "UPDATE sys_user SET last_login_time = ?, updated_at = ? WHERE id = ? AND deleted = 0", parameters);
                    return result->affectedRowCount() > 0;
                });
    }
} // namespace EdgePulse::System
