#include "System/UserRepository.h"

#include "Common/TimeFormat.h"

#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::System
{
    namespace
    {
        /**
         * @brief 读一列文本；NULL 或非文本时交回空串
         */
        [[nodiscard]] std::string readText(const AsynGyanis::Database::DatabaseValue &value)
        {
            if (const auto *text = std::get_if<std::string>(&value); text != nullptr)
            {
                return *text;
            }
            return {};
        }

        /**
         * @brief 读一列整数；NULL 或非整数时交回 0
         */
        [[nodiscard]] std::int64_t readInteger(const AsynGyanis::Database::DatabaseValue &value)
        {
            if (const auto *number = std::get_if<std::int64_t>(&value); number != nullptr)
            {
                return *number;
            }
            return 0;
        }

        /**
         * @brief 把当前行映射成 UserRecord
         */
        [[nodiscard]] UserRecord readUser(const AsynGyanis::Database::DatabaseResult &result)
        {
            UserRecord user;
            user.id            = readInteger(result.getValue("id"));
            user.username      = readText(result.getValue("username"));
            user.passwordHash  = readText(result.getValue("password"));
            user.nickname      = readText(result.getValue("nickname"));
            user.phone         = readText(result.getValue("phone"));
            user.email         = readText(result.getValue("email"));
            user.departmentId  = readInteger(result.getValue("dept_id"));
            user.status        = static_cast<int>(readInteger(result.getValue("status")));
            user.lastLoginTime = readText(result.getValue("last_login_time"));
            return user;
        }
    } // namespace

    UserRepository::UserRepository(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<std::optional<UserRecord>> UserRepository::findByUsername(AsynGyanis::Core::EventLoop &loop, std::string username)
    {
        co_return co_await m_database->withConnection<std::optional<UserRecord>>(
                loop,
                [username = std::move(username)](AsynGyanis::Database::DatabaseConnection &connection) -> std::optional<UserRecord>
                {
                    const std::array<AsynGyanis::Database::DatabaseValue, 1>    parameters{username};
                    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(
                            "SELECT id, username, password, nickname, phone, email, dept_id, status, last_login_time FROM sys_user WHERE username = ? AND deleted = 0", parameters);
                    // 查询失败与“用户不存在”必须分开：前者是 500，后者是登录失败
                    if (!result)
                    {
                        throw Common::DatabaseException("按登录名查询用户失败：" + connection.lastError());
                    }
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
                [userId](AsynGyanis::Database::DatabaseConnection &connection) -> std::optional<UserRecord>
                {
                    const std::array<AsynGyanis::Database::DatabaseValue, 1>    parameters{userId};
                    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(
                            "SELECT id, username, password, nickname, phone, email, dept_id, status, last_login_time FROM sys_user WHERE id = ? AND deleted = 0", parameters);
                    if (!result)
                    {
                        throw Common::DatabaseException("按标识查询用户失败：" + connection.lastError());
                    }
                    if (!result->next())
                    {
                        return std::nullopt;
                    }
                    return readUser(*result);
                });
    }

    AsynGyanis::Core::Task<std::vector<std::string>> UserRepository::permissionsFor(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        co_return co_await m_database->withConnection<std::vector<std::string>>(loop,
                                                                                [userId](AsynGyanis::Database::DatabaseConnection &connection) -> std::vector<std::string>
                                                                                {
                                                                                    const std::array<AsynGyanis::Database::DatabaseValue, 1>    parameters{userId};
                                                                                    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result =
                                                                                            connection.execute("SELECT DISTINCT m.permission FROM sys_menu m "
                                                                                                               "JOIN sys_role_menu rm ON rm.menu_id = m.id "
                                                                                                               "JOIN sys_user_role ur ON ur.role_id = rm.role_id "
                                                                                                               "WHERE ur.user_id = ? AND m.permission <> '' AND m.status = 1 "
                                                                                                               "ORDER BY m.permission",
                                                                                                               parameters);
                                                                                    if (!result)
                                                                                    {
                                                                                        throw Common::DatabaseException("查询权限码失败：" + connection.lastError());
                                                                                    }
                                                                                    std::vector<std::string> permissions;
                                                                                    while (result->next())
                                                                                    {
                                                                                        if (std::string permission = readText(result->getValue("permission")); !permission.empty())
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
                                                                                [userId](AsynGyanis::Database::DatabaseConnection &connection) -> std::vector<std::string>
                                                                                {
                                                                                    const std::array<AsynGyanis::Database::DatabaseValue, 1>    parameters{userId};
                                                                                    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result =
                                                                                            connection.execute("SELECT r.role_code FROM sys_role r "
                                                                                                               "JOIN sys_user_role ur ON ur.role_id = r.id "
                                                                                                               "WHERE ur.user_id = ? AND r.status = 1 AND r.deleted = 0 "
                                                                                                               "ORDER BY r.role_code",
                                                                                                               parameters);
                                                                                    if (!result)
                                                                                    {
                                                                                        throw Common::DatabaseException("查询角色码失败：" + connection.lastError());
                                                                                    }
                                                                                    std::vector<std::string> roles;
                                                                                    while (result->next())
                                                                                    {
                                                                                        if (std::string role = readText(result->getValue("role_code")); !role.empty())
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
                                                            [userId, passwordHash = std::move(passwordHash)](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                                                            {
                                                                const std::array<AsynGyanis::Database::DatabaseValue, 3> parameters{passwordHash, Common::nowIso8601Utc(), userId};
                                                                const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(
                                                                        "UPDATE sys_user SET password = ?, updated_at = ? WHERE id = ? AND deleted = 0", parameters);
                                                                if (!result)
                                                                {
                                                                    throw Common::DatabaseException("更新密码失败：" + connection.lastError());
                                                                }
                                                                return result->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<bool> UserRepository::touchLastLogin(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId, std::string loginTime)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [userId, loginTime = std::move(loginTime)](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                                                            {
                                                                const std::array<AsynGyanis::Database::DatabaseValue, 3>    parameters{loginTime, Common::nowIso8601Utc(), userId};
                                                                const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(
                                                                        "UPDATE sys_user SET last_login_time = ?, updated_at = ? WHERE id = ? AND deleted = 0", parameters);
                                                                if (!result)
                                                                {
                                                                    throw Common::DatabaseException("更新登录时间失败：" + connection.lastError());
                                                                }
                                                                return result->affectedRowCount() > 0;
                                                            });
    }
} // namespace EdgePulse::System
