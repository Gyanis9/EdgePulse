#include "Core/Firmware/FirmwareRepository.h"

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

namespace EdgePulse::Firmware
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        constexpr std::string_view kSelectFirmware =
                "SELECT id, name, version, file_name, content_type, size_bytes, sha256, storage_path, created_by, created_at FROM biz_firmware ";
        constexpr std::string_view kSelectTask =
                "SELECT t.id, t.firmware_id, t.node_id, t.status, t.progress, t.message, t.created_at, t.updated_at, COALESCE(n.name, '') AS node_name "
                "FROM biz_firmware_task t LEFT JOIN biz_node n ON n.id = t.node_id ";

        [[nodiscard]] FirmwareRecord readFirmware(const DatabaseResult &result)
        {
            FirmwareRecord record;
            record.id          = Sql::integer(result, "id");
            record.name        = Sql::text(result, "name");
            record.version     = Sql::text(result, "version");
            record.fileName    = Sql::text(result, "file_name");
            record.contentType = Sql::text(result, "content_type");
            record.sizeBytes   = Sql::integer(result, "size_bytes");
            record.sha256      = Sql::text(result, "sha256");
            record.storagePath = Sql::text(result, "storage_path");
            record.createdBy   = Sql::integer(result, "created_by");
            record.createdAt   = Sql::dateTimeText(result, "created_at");
            return record;
        }

        [[nodiscard]] FirmwareTaskRecord readTask(const DatabaseResult &result)
        {
            FirmwareTaskRecord record;
            record.id         = Sql::integer(result, "id");
            record.firmwareId = Sql::integer(result, "firmware_id");
            record.nodeId     = Sql::integer(result, "node_id");
            record.nodeName   = Sql::text(result, "node_name");
            record.status     = Sql::text(result, "status");
            record.progress   = static_cast<int>(Sql::integer(result, "progress"));
            record.message    = Sql::text(result, "message");
            record.createdAt  = Sql::dateTimeText(result, "created_at");
            record.updatedAt  = Sql::dateTimeText(result, "updated_at");
            return record;
        }
    } // namespace

    FirmwareRepository::FirmwareRepository(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<std::int64_t> FirmwareRepository::create(AsynGyanis::Core::EventLoop &loop, FirmwareRecord record)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [record = std::move(record)](DatabaseConnection &connection) mutable -> std::int64_t
                {
                    if (record.id == 0)
                    {
                        record.id = Common::nextIdentifier();
                    }
                    if (record.createdAt.empty())
                    {
                        record.createdAt = Common::nowDateTimeUtc();
                    }
                    const std::array<DatabaseValue, 11> parameters{record.id,     record.name,        record.version,   record.fileName,  record.contentType, record.sizeBytes,
                                                                   record.sha256, record.storagePath, record.createdBy, record.createdAt, std::int64_t{0}};
                    Sql::executeOrThrowVoid(connection,
                                            "INSERT INTO biz_firmware (id, name, version, file_name, content_type, size_bytes, sha256, storage_path, created_by, created_at, "
                                            "deleted) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                            parameters);
                    return record.id;
                });
    }

    AsynGyanis::Core::Task<Common::PageRows<FirmwareRecord>> FirmwareRepository::list(AsynGyanis::Core::EventLoop &loop, std::string keyword, const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<FirmwareRecord>>(
                loop,
                [keyword = std::move(keyword), page](DatabaseConnection &connection) -> Common::PageRows<FirmwareRecord>
                {
                    std::string                filter = "WHERE deleted = 0";
                    std::vector<DatabaseValue> parameters;
                    if (!keyword.empty())
                    {
                        const std::string pattern = "%" + keyword + "%";
                        filter += " AND (name LIKE ? OR version LIKE ?)";
                        parameters.emplace_back(pattern);
                        parameters.emplace_back(pattern);
                    }
                    const auto                       count = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM biz_firmware " + filter, parameters);
                    Common::PageRows<FirmwareRecord> pageRows;
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
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectFirmware} + filter + " ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?", parameters);
                    while (rows->next())
                    {
                        pageRows.rows.push_back(readFirmware(*rows));
                    }
                    return pageRows;
                });
    }

    AsynGyanis::Core::Task<std::optional<FirmwareRecord>> FirmwareRepository::find(AsynGyanis::Core::EventLoop &loop, const std::int64_t firmwareId)
    {
        co_return co_await m_database->withConnection<std::optional<FirmwareRecord>>(
                loop,
                [firmwareId](DatabaseConnection &connection) -> std::optional<FirmwareRecord>
                {
                    const std::array<DatabaseValue, 1> parameters{firmwareId};
                    const auto                         rows = Sql::executeOrThrow(connection, std::string{kSelectFirmware} + "WHERE id = ? AND deleted = 0", parameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    return readFirmware(*rows);
                });
    }

    AsynGyanis::Core::Task<std::optional<FirmwareRecord>> FirmwareRepository::remove(AsynGyanis::Core::EventLoop &loop, const std::int64_t firmwareId)
    {
        co_return co_await m_database->withConnection<std::optional<FirmwareRecord>>(
                loop,
                [firmwareId](DatabaseConnection &connection) -> std::optional<FirmwareRecord>
                {
                    const std::array<DatabaseValue, 1> parameters{firmwareId};
                    const auto                         rows = Sql::executeOrThrow(connection, std::string{kSelectFirmware} + "WHERE id = ? AND deleted = 0", parameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    FirmwareRecord record = readFirmware(*rows);
                    Sql::executeOrThrowVoid(connection, "UPDATE biz_firmware SET deleted = 1 WHERE id = ?", parameters);
                    return record;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> FirmwareRepository::createTask(AsynGyanis::Core::EventLoop &loop, const std::int64_t firmwareId, const std::int64_t nodeId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [firmwareId, nodeId](DatabaseConnection &connection) -> std::int64_t
                {
                    const std::array<DatabaseValue, 2> parameters{firmwareId, nodeId};
                    const auto                         existing = Sql::executeOrThrow(
                            connection, "SELECT COUNT(*) AS total FROM biz_firmware_task WHERE firmware_id = ? AND node_id = ? AND status IN ('pending', 'downloading')",
                            parameters);
                    if (existing->next() && Sql::integer(*existing, "total") > 0)
                    {
                        return 0;
                    }
                    const std::int64_t                 id  = Common::nextIdentifier();
                    const std::string                  now = Common::nowDateTimeUtc();
                    const std::array<DatabaseValue, 8> insertParameters{id, firmwareId, nodeId, std::string{"pending"}, std::int64_t{0}, std::string{}, now, now};
                    Sql::executeOrThrowVoid(connection,
                                            "INSERT INTO biz_firmware_task (id, firmware_id, node_id, status, progress, message, created_at, updated_at) "
                                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                                            insertParameters);
                    return id;
                });
    }

    AsynGyanis::Core::Task<std::vector<FirmwareTaskRecord>> FirmwareRepository::tasks(AsynGyanis::Core::EventLoop &loop, const std::int64_t firmwareId)
    {
        co_return co_await m_database->withConnection<std::vector<FirmwareTaskRecord>>(
                loop,
                [firmwareId](DatabaseConnection &connection) -> std::vector<FirmwareTaskRecord>
                {
                    const std::array<DatabaseValue, 1> parameters{firmwareId};
                    const auto                         rows = Sql::executeOrThrow(connection, std::string{kSelectTask} + "WHERE t.firmware_id = ? ORDER BY t.id", parameters);
                    std::vector<FirmwareTaskRecord>    tasks;
                    while (rows->next())
                    {
                        tasks.push_back(readTask(*rows));
                    }
                    return tasks;
                });
    }

    AsynGyanis::Core::Task<std::optional<FirmwareTaskRecord>> FirmwareRepository::updateTask(AsynGyanis::Core::EventLoop &loop, const std::int64_t firmwareId,
                                                                                             const std::int64_t nodeId, std::string status, const int progress, std::string message)
    {
        co_return co_await m_database->withConnection<std::optional<FirmwareTaskRecord>>(
                loop,
                [firmwareId, nodeId, status = std::move(status), progress, message = std::move(message)](DatabaseConnection &connection) -> std::optional<FirmwareTaskRecord>
                {
                    const auto update = Sql::executeOrThrow(
                            connection, "UPDATE biz_firmware_task SET status = ?, progress = ?, message = ?, updated_at = ? WHERE firmware_id = ? AND node_id = ?",
                            std::array<DatabaseValue, 6>{status, static_cast<std::int64_t>(progress), message, Common::nowDateTimeUtc(), firmwareId, nodeId});
                    if (update->affectedRowCount() == 0)
                    {
                        return std::nullopt;
                    }
                    const std::array<DatabaseValue, 2> queryParameters{firmwareId, nodeId};
                    const auto                         rows =
                            Sql::executeOrThrow(connection, std::string{kSelectTask} + "WHERE t.firmware_id = ? AND t.node_id = ? ORDER BY t.id DESC LIMIT 1", queryParameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    return readTask(*rows);
                });
    }
} // namespace EdgePulse::Firmware
