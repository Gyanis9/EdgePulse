#include "Core/System/DictionaryRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::System
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        constexpr std::string_view kSelectType   = "SELECT id, dict_name, dict_type, status, remark, created_at FROM sys_dict_type ";
        constexpr std::string_view kSelectData   = "SELECT id, dict_type, label, value, sort, status, tone, is_default, remark FROM sys_dict_data ";
        constexpr std::string_view kSelectConfig = "SELECT id, config_name, config_key, config_value, config_type, builtin, remark, created_at FROM sys_config ";

        [[nodiscard]] DictTypeRecord readType(const DatabaseResult &result)
        {
            DictTypeRecord record;
            record.id         = Sql::integer(result, "id");
            record.name       = Sql::text(result, "dict_name");
            record.type       = Sql::text(result, "dict_type");
            record.status     = static_cast<int>(Sql::integer(result, "status"));
            record.remark     = Sql::text(result, "remark");
            record.createTime = Sql::dateTimeText(result, "created_at");
            return record;
        }

        [[nodiscard]] DictDataRecord readData(const DatabaseResult &result)
        {
            DictDataRecord record;
            record.id        = Sql::integer(result, "id");
            record.dictType  = Sql::text(result, "dict_type");
            record.label     = Sql::text(result, "label");
            record.value     = Sql::text(result, "value");
            record.sort      = static_cast<int>(Sql::integer(result, "sort"));
            record.status    = static_cast<int>(Sql::integer(result, "status"));
            record.tone      = Sql::text(result, "tone");
            record.isDefault = Sql::boolean(result, "is_default");
            record.remark    = Sql::text(result, "remark");
            return record;
        }

        [[nodiscard]] ConfigRecord readConfig(const DatabaseResult &result)
        {
            ConfigRecord record;
            record.id         = Sql::integer(result, "id");
            record.name       = Sql::text(result, "config_name");
            record.key        = Sql::text(result, "config_key");
            record.value      = Sql::text(result, "config_value");
            record.type       = Sql::text(result, "config_type");
            record.builtin    = Sql::boolean(result, "builtin");
            record.remark     = Sql::text(result, "remark");
            record.createTime = Sql::dateTimeText(result, "created_at");
            return record;
        }
    } // namespace

    DictionaryRepository::DictionaryRepository(std::shared_ptr<Common::Database> database, TenantScope tenant) : m_database(std::move(database)), m_tenant(tenant)
    {
    }

    std::shared_ptr<DictionaryRepository> DictionaryRepository::scoped(const TenantScope tenant) const
    {
        return std::make_shared<DictionaryRepository>(m_database, tenant);
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

    AsynGyanis::Core::Task<Common::PageRows<DictTypeRecord>> DictionaryRepository::listDictTypes(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status,
                                                                                                 const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<DictTypeRecord>>(
                loop,
                [keyword = std::move(keyword), status = std::move(status), page, tenant = m_tenant](DatabaseConnection &connection) -> Common::PageRows<DictTypeRecord>
                {
                    std::string                filter = "WHERE deleted = 0";
                    std::vector<DatabaseValue> parameters;
                    filter += tenantClause(tenant, parameters, "tenant_id");
                    if (!keyword.empty())
                    {
                        const std::string pattern = "%" + keyword + "%";
                        filter += " AND (dict_name LIKE ? OR dict_type LIKE ?)";
                        parameters.emplace_back(pattern);
                        parameters.emplace_back(pattern);
                    }
                    if (status == "0" || status == "1")
                    {
                        filter += " AND status = ?";
                        parameters.emplace_back(static_cast<std::int64_t>(status == "1" ? 1 : 0));
                    }
                    const auto                       count = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_dict_type " + filter, parameters);
                    Common::PageRows<DictTypeRecord> pageRows;
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
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectType} + filter + " ORDER BY id LIMIT ? OFFSET ?", parameters);
                    while (rows->next())
                    {
                        pageRows.rows.push_back(readType(*rows));
                    }
                    return pageRows;
                });
    }

    AsynGyanis::Core::Task<std::vector<DictTypeRecord>> DictionaryRepository::exportDictTypes(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status)
    {
        co_return co_await m_database->withConnection<std::vector<DictTypeRecord>>(
                loop,
                [keyword = std::move(keyword), status = std::move(status), tenant = m_tenant](DatabaseConnection &connection) -> std::vector<DictTypeRecord>
                {
                    std::string                filter = "WHERE deleted = 0";
                    std::vector<DatabaseValue> parameters;
                    filter += tenantClause(tenant, parameters, "tenant_id");
                    if (!keyword.empty())
                    {
                        const std::string pattern = "%" + keyword + "%";
                        filter += " AND (dict_name LIKE ? OR dict_type LIKE ?)";
                        parameters.emplace_back(pattern);
                        parameters.emplace_back(pattern);
                    }
                    if (status == "0" || status == "1")
                    {
                        filter += " AND status = ?";
                        parameters.emplace_back(static_cast<std::int64_t>(status == "1" ? 1 : 0));
                    }
                    const auto                  rows = Sql::executeOrThrow(connection, std::string{kSelectType} + filter + " ORDER BY id", parameters);
                    std::vector<DictTypeRecord> result;
                    while (rows->next())
                    {
                        result.push_back(readType(*rows));
                    }
                    return result;
                });
    }

    AsynGyanis::Core::Task<bool> DictionaryRepository::dictTypeExists(AsynGyanis::Core::EventLoop &loop, std::string type, const std::int64_t excludeId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [type = std::move(type), excludeId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{type, excludeId};
                    const std::string          clause = tenantClause(tenant, parameters, "tenant_id");
                    const auto                 rows =
                            Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_dict_type WHERE dict_type = ? AND id <> ? AND deleted = 0" + clause, parameters);
                    return rows->next() && Sql::integer(*rows, "total") > 0;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> DictionaryRepository::createDictType(AsynGyanis::Core::EventLoop &loop, const DictTypeWrite &write, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [write, operatorId, tenant = m_tenant](DatabaseConnection &connection) -> std::int64_t
                {
                    const std::int64_t                  id  = Common::nextIdentifier();
                    const std::string                   now = Common::nowDateTimeUtc();
                    const std::array<DatabaseValue, 11> parameters{id,
                                                                   tenant.tenantId,
                                                                   write.name.value_or(std::string{}),
                                                                   write.type.value_or(std::string{}),
                                                                   static_cast<std::int64_t>(write.status.value_or(1)),
                                                                   write.remark.value_or(std::string{}),
                                                                   now,
                                                                   now,
                                                                   operatorId,
                                                                   operatorId,
                                                                   std::int64_t{0}};
                    Sql::executeOrThrowVoid(
                            connection,
                            "INSERT INTO sys_dict_type (id, tenant_id, dict_name, dict_type, status, remark, created_at, updated_at, created_by, updated_by, deleted) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                            parameters);
                    return id;
                });
    }

    AsynGyanis::Core::Task<bool> DictionaryRepository::updateDictType(AsynGyanis::Core::EventLoop &loop, const std::int64_t typeId, const DictTypeWrite &write,
                                                                      const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [typeId, write, operatorId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                                                            {
                                                                std::string                sql = "UPDATE sys_dict_type SET updated_at = ?, updated_by = ?";
                                                                std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), operatorId};
                                                                if (write.name.has_value())
                                                                {
                                                                    sql += ", dict_name = ?";
                                                                    parameters.emplace_back(*write.name);
                                                                }
                                                                if (write.type.has_value())
                                                                {
                                                                    sql += ", dict_type = ?";
                                                                    parameters.emplace_back(*write.type);
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
                                                                parameters.emplace_back(typeId);
                                                                sql += tenantClause(tenant, parameters, "tenant_id");
                                                                const auto rows = Sql::executeOrThrow(connection, sql, parameters);
                                                                return rows->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<bool> DictionaryRepository::removeDictType(AsynGyanis::Core::EventLoop &loop, const std::int64_t typeId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [typeId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameter{typeId};
                    const std::string          typeClause = tenantClause(tenant, parameter, "tenant_id");
                    const auto typeRows = Sql::executeOrThrow(connection, "SELECT dict_type FROM sys_dict_type WHERE id = ? AND deleted = 0" + typeClause, parameter);
                    if (!typeRows->next())
                    {
                        return false;
                    }
                    const std::string          type = Sql::text(*typeRows, "dict_type");
                    std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), typeId};
                    const std::string          clause = tenantClause(tenant, parameters, "tenant_id");
                    const auto rows = Sql::executeOrThrow(connection, "UPDATE sys_dict_type SET deleted = 1, updated_at = ? WHERE id = ? AND deleted = 0" + clause, parameters);
                    std::vector<DatabaseValue> typeParameter{type};
                    const std::string          dataClause = tenantClause(tenant, typeParameter, "tenant_id");
                    Sql::executeOrThrowVoid(connection, "DELETE FROM sys_dict_data WHERE dict_type = ?" + dataClause, typeParameter);
                    return rows->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<std::vector<DictDataRecord>> DictionaryRepository::dictData(AsynGyanis::Core::EventLoop &loop, std::string type)
    {
        co_return co_await m_database->withConnection<std::vector<DictDataRecord>>(
                loop,
                [type = std::move(type), tenant = m_tenant](DatabaseConnection &connection) -> std::vector<DictDataRecord>
                {
                    std::vector<DatabaseValue> parameters{type};
                    const std::string          clause = tenantClause(tenant, parameters, "tenant_id");
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectData} + "WHERE dict_type = ? AND deleted = 0" + clause + " ORDER BY sort, id", parameters);
                    std::vector<DictDataRecord> result;
                    while (rows->next())
                    {
                        result.push_back(readData(*rows));
                    }
                    return result;
                });
    }

    AsynGyanis::Core::Task<std::vector<DictDataRecord>> DictionaryRepository::dictOptions(AsynGyanis::Core::EventLoop &loop, std::string type)
    {
        co_return co_await m_database->withConnection<std::vector<DictDataRecord>>(
                loop,
                [type = std::move(type), tenant = m_tenant](DatabaseConnection &connection) -> std::vector<DictDataRecord>
                {
                    std::vector<DatabaseValue> parameters{type};
                    const std::string          clause = tenantClause(tenant, parameters, "tenant_id");
                    auto rows = Sql::executeOrThrow(connection, std::string{kSelectData} + "WHERE dict_type = ? AND deleted = 0 AND status = 1" + clause + " ORDER BY sort, id",
                                                    parameters);
                    std::vector<DictDataRecord> result;
                    while (rows->next())
                    {
                        result.push_back(readData(*rows));
                    }
                    if (result.empty() && !tenant.allTenants && tenant.tenantId != 1)
                    {
                        // 租户没有自定义该字典时沿用默认租户的平台字典
                        const std::array<DatabaseValue, 2> fallback{type, std::int64_t{1}};
                        const auto                         defaults = Sql::executeOrThrow(
                                connection, std::string{kSelectData} + "WHERE dict_type = ? AND deleted = 0 AND status = 1 AND tenant_id = ? ORDER BY sort, id", fallback);
                        while (defaults->next())
                        {
                            result.push_back(readData(*defaults));
                        }
                    }
                    return result;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> DictionaryRepository::createDictData(AsynGyanis::Core::EventLoop &loop, const DictDataWrite &write, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [write, operatorId, tenant = m_tenant](DatabaseConnection &connection) -> std::int64_t
                {
                    const std::int64_t                  id  = Common::nextIdentifier();
                    const std::string                   now = Common::nowDateTimeUtc();
                    const std::array<DatabaseValue, 13> parameters{id,
                                                                   tenant.tenantId,
                                                                   write.type.value_or(std::string{}),
                                                                   write.label.value_or(std::string{}),
                                                                   write.value.value_or(std::string{}),
                                                                   static_cast<std::int64_t>(write.sort.value_or(0)),
                                                                   static_cast<std::int64_t>(write.status.value_or(1)),
                                                                   write.tone.value_or(std::string{"neutral"}),
                                                                   static_cast<std::int64_t>(write.isDefault.value_or(false) ? 1 : 0),
                                                                   write.remark.value_or(std::string{}),
                                                                   now,
                                                                   now,
                                                                   operatorId};
                    Sql::executeOrThrowVoid(
                            connection,
                            "INSERT INTO sys_dict_data (id, tenant_id, dict_type, label, value, sort, status, tone, is_default, remark, created_at, updated_at, created_by) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                            parameters);
                    return id;
                });
    }

    AsynGyanis::Core::Task<bool> DictionaryRepository::updateDictData(AsynGyanis::Core::EventLoop &loop, const std::int64_t dataId, const DictDataWrite &write,
                                                                      const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [dataId, write, operatorId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                                                            {
                                                                std::string                sql = "UPDATE sys_dict_data SET updated_at = ?, updated_by = ?";
                                                                std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), operatorId};
                                                                if (write.type.has_value())
                                                                {
                                                                    sql += ", dict_type = ?";
                                                                    parameters.emplace_back(*write.type);
                                                                }
                                                                if (write.label.has_value())
                                                                {
                                                                    sql += ", label = ?";
                                                                    parameters.emplace_back(*write.label);
                                                                }
                                                                if (write.value.has_value())
                                                                {
                                                                    sql += ", value = ?";
                                                                    parameters.emplace_back(*write.value);
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
                                                                if (write.tone.has_value())
                                                                {
                                                                    sql += ", tone = ?";
                                                                    parameters.emplace_back(*write.tone);
                                                                }
                                                                if (write.isDefault.has_value())
                                                                {
                                                                    sql += ", is_default = ?";
                                                                    parameters.emplace_back(static_cast<std::int64_t>(*write.isDefault ? 1 : 0));
                                                                }
                                                                if (write.remark.has_value())
                                                                {
                                                                    sql += ", remark = ?";
                                                                    parameters.emplace_back(*write.remark);
                                                                }
                                                                sql += " WHERE id = ? AND deleted = 0";
                                                                parameters.emplace_back(dataId);
                                                                sql += tenantClause(tenant, parameters, "tenant_id");
                                                                const auto rows = Sql::executeOrThrow(connection, sql, parameters);
                                                                return rows->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<bool> DictionaryRepository::removeDictData(AsynGyanis::Core::EventLoop &loop, const std::int64_t dataId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [dataId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), dataId};
                    const std::string          clause = tenantClause(tenant, parameters, "tenant_id");
                    const auto rows = Sql::executeOrThrow(connection, "UPDATE sys_dict_data SET deleted = 1, updated_at = ? WHERE id = ? AND deleted = 0" + clause, parameters);
                    return rows->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<Common::PageRows<ConfigRecord>> DictionaryRepository::listConfigs(AsynGyanis::Core::EventLoop &loop, std::string keyword,
                                                                                             const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<ConfigRecord>>(
                loop,
                [keyword = std::move(keyword), page](DatabaseConnection &connection) -> Common::PageRows<ConfigRecord>
                {
                    std::string                filter = "WHERE deleted = 0";
                    std::vector<DatabaseValue> parameters;
                    if (!keyword.empty())
                    {
                        const std::string pattern = "%" + keyword + "%";
                        filter += " AND (config_name LIKE ? OR config_key LIKE ? OR config_value LIKE ?)";
                        parameters.emplace_back(pattern);
                        parameters.emplace_back(pattern);
                        parameters.emplace_back(pattern);
                    }
                    const auto                     count = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_config " + filter, parameters);
                    Common::PageRows<ConfigRecord> pageRows;
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
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectConfig} + filter + " ORDER BY id LIMIT ? OFFSET ?", parameters);
                    while (rows->next())
                    {
                        pageRows.rows.push_back(readConfig(*rows));
                    }
                    return pageRows;
                });
    }

    AsynGyanis::Core::Task<std::vector<ConfigRecord>> DictionaryRepository::exportConfigs(AsynGyanis::Core::EventLoop &loop, std::string keyword)
    {
        co_return co_await m_database->withConnection<std::vector<ConfigRecord>>(loop,
                                                                                 [keyword = std::move(keyword)](DatabaseConnection &connection) -> std::vector<ConfigRecord>
                                                                                 {
                                                                                     std::string                filter = "WHERE deleted = 0";
                                                                                     std::vector<DatabaseValue> parameters;
                                                                                     if (!keyword.empty())
                                                                                     {
                                                                                         const std::string pattern = "%" + keyword + "%";
                                                                                         filter += " AND (config_name LIKE ? OR config_key LIKE ? OR config_value LIKE ?)";
                                                                                         parameters.emplace_back(pattern);
                                                                                         parameters.emplace_back(pattern);
                                                                                         parameters.emplace_back(pattern);
                                                                                     }
                                                                                     const auto rows = Sql::executeOrThrow(
                                                                                             connection, std::string{kSelectConfig} + filter + " ORDER BY id", parameters);
                                                                                     std::vector<ConfigRecord> result;
                                                                                     while (rows->next())
                                                                                     {
                                                                                         result.push_back(readConfig(*rows));
                                                                                     }
                                                                                     return result;
                                                                                 });
    }

    AsynGyanis::Core::Task<bool> DictionaryRepository::configKeyExists(AsynGyanis::Core::EventLoop &loop, std::string key, const std::int64_t excludeId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [key = std::move(key), excludeId](DatabaseConnection &connection) -> bool
                {
                    const std::array<DatabaseValue, 2> parameters{key, excludeId};
                    const auto rows = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_config WHERE config_key = ? AND id <> ? AND deleted = 0", parameters);
                    return rows->next() && Sql::integer(*rows, "total") > 0;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> DictionaryRepository::createConfig(AsynGyanis::Core::EventLoop &loop, const ConfigWrite &write, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [write, operatorId](DatabaseConnection &connection) -> std::int64_t
                {
                    const std::int64_t                  id  = Common::nextIdentifier();
                    const std::string                   now = Common::nowDateTimeUtc();
                    const std::array<DatabaseValue, 12> parameters{id,
                                                                   write.name.value_or(std::string{}),
                                                                   write.key.value_or(std::string{}),
                                                                   write.value.value_or(std::string{}),
                                                                   write.type.value_or(std::string{"N"}),
                                                                   static_cast<std::int64_t>(write.builtin.value_or(false) ? 1 : 0),
                                                                   write.remark.value_or(std::string{}),
                                                                   now,
                                                                   now,
                                                                   operatorId,
                                                                   operatorId,
                                                                   std::int64_t{0}};
                    Sql::executeOrThrowVoid(connection,
                                            "INSERT INTO sys_config (id, config_name, config_key, config_value, config_type, builtin, remark, created_at, updated_at, created_by, "
                                            "updated_by, deleted) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                            parameters);
                    return id;
                });
    }

    AsynGyanis::Core::Task<bool> DictionaryRepository::updateConfig(AsynGyanis::Core::EventLoop &loop, const std::int64_t configId, const ConfigWrite &write,
                                                                    const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [configId, write, operatorId](DatabaseConnection &connection) -> bool
                                                            {
                                                                std::string                sql = "UPDATE sys_config SET updated_at = ?, updated_by = ?";
                                                                std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), operatorId};
                                                                if (write.name.has_value())
                                                                {
                                                                    sql += ", config_name = ?";
                                                                    parameters.emplace_back(*write.name);
                                                                }
                                                                if (write.key.has_value())
                                                                {
                                                                    sql += ", config_key = ?";
                                                                    parameters.emplace_back(*write.key);
                                                                }
                                                                if (write.value.has_value())
                                                                {
                                                                    sql += ", config_value = ?";
                                                                    parameters.emplace_back(*write.value);
                                                                }
                                                                if (write.type.has_value())
                                                                {
                                                                    sql += ", config_type = ?";
                                                                    parameters.emplace_back(*write.type);
                                                                }
                                                                if (write.builtin.has_value())
                                                                {
                                                                    sql += ", builtin = ?";
                                                                    parameters.emplace_back(static_cast<std::int64_t>(*write.builtin ? 1 : 0));
                                                                }
                                                                if (write.remark.has_value())
                                                                {
                                                                    sql += ", remark = ?";
                                                                    parameters.emplace_back(*write.remark);
                                                                }
                                                                sql += " WHERE id = ? AND deleted = 0";
                                                                parameters.emplace_back(configId);
                                                                const auto rows = Sql::executeOrThrow(connection, sql, parameters);
                                                                return rows->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<bool> DictionaryRepository::removeConfig(AsynGyanis::Core::EventLoop &loop, const std::int64_t configId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [configId](DatabaseConnection &connection) -> bool
                                                            {
                                                                const std::array<DatabaseValue, 2> parameters{Common::nowDateTimeUtc(), configId};
                                                                const auto                         rows = Sql::executeOrThrow(
                                                                        connection, "UPDATE sys_config SET deleted = 1, updated_at = ? WHERE id = ? AND deleted = 0", parameters);
                                                                return rows->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<std::optional<ConfigRecord>> DictionaryRepository::findConfig(AsynGyanis::Core::EventLoop &loop, const std::int64_t configId)
    {
        co_return co_await m_database->withConnection<std::optional<ConfigRecord>>(loop,
                                                                                   [configId](DatabaseConnection &connection) -> std::optional<ConfigRecord>
                                                                                   {
                                                                                       const std::array<DatabaseValue, 1> parameters{configId};
                                                                                       const auto                         rows = Sql::executeOrThrow(
                                                                                               connection, std::string{kSelectConfig} + "WHERE id = ? AND deleted = 0", parameters);
                                                                                       if (!rows->next())
                                                                                       {
                                                                                           return std::nullopt;
                                                                                       }
                                                                                       return readConfig(*rows);
                                                                                   });
    }
} // namespace EdgePulse::System
