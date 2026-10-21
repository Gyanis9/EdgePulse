#include "Core/Notification/NotificationRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Notification
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        constexpr std::string_view kSelectChannel = "SELECT id, tenant_id, name, type, endpoint, recipients, username, secret, sender, ca_file, min_level, enabled, created_by, "
                                                    "created_at FROM biz_notification_channel ";

        [[nodiscard]] ChannelRecord readChannel(const DatabaseResult &result)
        {
            ChannelRecord record;
            record.id         = Sql::integer(result, "id");
            record.tenantId   = Sql::integer(result, "tenant_id");
            record.name       = Sql::text(result, "name");
            record.type       = Sql::text(result, "type");
            record.endpoint   = Sql::text(result, "endpoint");
            record.recipients = Sql::text(result, "recipients");
            record.username   = Sql::text(result, "username");
            record.secret     = Sql::text(result, "secret");
            record.sender     = Sql::text(result, "sender");
            record.caFile     = Sql::text(result, "ca_file");
            record.minLevel   = Sql::text(result, "min_level");
            record.enabled    = Sql::boolean(result, "enabled");
            record.createdBy  = Sql::integer(result, "created_by");
            record.createdAt  = Sql::dateTimeText(result, "created_at");
            return record;
        }
    } // namespace

    NotificationRepository::NotificationRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant) : m_database(std::move(database)), m_tenant(tenant)
    {
    }

    std::shared_ptr<NotificationRepository> NotificationRepository::scoped(const System::TenantScope tenant) const
    {
        return std::make_shared<NotificationRepository>(m_database, tenant);
    }

    AsynGyanis::Core::Task<Common::PageRows<ChannelRecord>> NotificationRepository::list(AsynGyanis::Core::EventLoop &loop, std::string keyword, const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<ChannelRecord>>(
                loop,
                [keyword = std::move(keyword), page, tenant = m_tenant](DatabaseConnection &connection) -> Common::PageRows<ChannelRecord>
                {
                    std::string                filter = "WHERE deleted = 0";
                    std::vector<DatabaseValue> parameters;
                    if (!tenant.allTenants)
                    {
                        filter += " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    if (!keyword.empty())
                    {
                        const std::string pattern = "%" + keyword + "%";
                        filter += " AND (name LIKE ? OR endpoint LIKE ? OR type LIKE ?)";
                        parameters.emplace_back(pattern);
                        parameters.emplace_back(pattern);
                        parameters.emplace_back(pattern);
                    }
                    const auto                      count = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM biz_notification_channel " + filter, parameters);
                    Common::PageRows<ChannelRecord> pageRows;
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
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectChannel} + filter + " ORDER BY id DESC LIMIT ? OFFSET ?", parameters);
                    while (rows->next())
                    {
                        pageRows.rows.push_back(readChannel(*rows));
                    }
                    return pageRows;
                });
    }

    AsynGyanis::Core::Task<std::optional<ChannelRecord>> NotificationRepository::find(AsynGyanis::Core::EventLoop &loop, const std::int64_t channelId)
    {
        co_return co_await m_database->withConnection<std::optional<ChannelRecord>>(
                loop,
                [channelId, tenant = m_tenant](DatabaseConnection &connection) -> std::optional<ChannelRecord>
                {
                    std::vector<DatabaseValue> parameters{channelId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectChannel} + "WHERE id = ? AND deleted = 0" + clause, parameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    return readChannel(*rows);
                });
    }

    AsynGyanis::Core::Task<std::vector<ChannelRecord>> NotificationRepository::enabledChannels(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await m_database->withConnection<std::vector<ChannelRecord>>(
                loop,
                [tenant = m_tenant](DatabaseConnection &connection) -> std::vector<ChannelRecord>
                {
                    std::vector<DatabaseValue> parameters;
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectChannel} + "WHERE deleted = 0 AND enabled = 1" + clause + " ORDER BY id", parameters);
                    std::vector<ChannelRecord> channels;
                    while (rows->next())
                    {
                        channels.push_back(readChannel(*rows));
                    }
                    return channels;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> NotificationRepository::create(AsynGyanis::Core::EventLoop &loop, ChannelRecord record)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [record = std::move(record), tenant = m_tenant](DatabaseConnection &connection) mutable -> std::int64_t
                {
                    if (record.id == 0)
                    {
                        record.id = Common::nextIdentifier();
                    }
                    const std::string                   now = Common::nowDateTimeUtc();
                    const std::array<DatabaseValue, 14> parameters{record.id,
                                                                   tenant.tenantId,
                                                                   record.name,
                                                                   record.type,
                                                                   record.endpoint,
                                                                   record.recipients,
                                                                   record.username,
                                                                   record.secret,
                                                                   record.sender,
                                                                   record.minLevel,
                                                                   record.enabled ? std::int64_t{1} : std::int64_t{0},
                                                                   record.createdBy,
                                                                   now,
                                                                   now};
                    Sql::executeOrThrowVoid(connection,
                                            "INSERT INTO biz_notification_channel (id, tenant_id, name, type, endpoint, recipients, username, secret, sender, min_level, enabled, "
                                            "created_by, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                            parameters);
                    return record.id;
                });
    }

    AsynGyanis::Core::Task<bool> NotificationRepository::update(AsynGyanis::Core::EventLoop &loop, const std::int64_t channelId, ChannelRecord record)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [channelId, record = std::move(record), tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::string                sql = "UPDATE biz_notification_channel SET name = ?, type = ?, endpoint = ?, recipients = ?, username = ?, sender = ?, "
                                                     "min_level = ?, enabled = ?, updated_at = ?";
                    std::vector<DatabaseValue> parameters{record.name,
                                                          record.type,
                                                          record.endpoint,
                                                          record.recipients,
                                                          record.username,
                                                          record.sender,
                                                          record.minLevel,
                                                          static_cast<std::int64_t>(record.enabled ? 1 : 0),
                                                          Common::nowDateTimeUtc()};
                    if (!record.secret.empty())
                    {
                        sql += ", secret = ?";
                        parameters.emplace_back(record.secret);
                    }
                    sql += " WHERE id = ? AND deleted = 0";
                    parameters.emplace_back(channelId);
                    if (!tenant.allTenants)
                    {
                        sql += " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    return Sql::executeOrThrow(connection, sql, parameters)->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<bool> NotificationRepository::remove(AsynGyanis::Core::EventLoop &loop, const std::int64_t channelId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [channelId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), channelId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows =
                            Sql::executeOrThrow(connection, "UPDATE biz_notification_channel SET deleted = 1, updated_at = ? WHERE id = ? AND deleted = 0" + clause, parameters);
                    return rows->affectedRowCount() > 0;
                });
    }
} // namespace EdgePulse::Notification
