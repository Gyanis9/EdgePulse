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
                "SELECT id, tenant_id, name, version, file_name, content_type, size_bytes, sha256, signature, signing_key_id, signature_algorithm, storage_path, created_by, "
                "created_at FROM biz_firmware ";
        constexpr std::string_view kSelectTask =
                "SELECT t.id, t.firmware_id, t.node_id, t.status, t.progress, t.message, t.created_at, t.updated_at, t.previous_firmware_id, t.rollout_id, t.wave_id, "
                "t.command_id, COALESCE(n.name, '') AS node_name "
                "FROM biz_firmware_task t LEFT JOIN biz_node n ON n.id = t.node_id AND n.tenant_id = t.tenant_id ";

        [[nodiscard]] FirmwareRecord readFirmware(const DatabaseResult &result)
        {
            FirmwareRecord record;
            record.id                 = Sql::integer(result, "id");
            record.tenantId           = Sql::integer(result, "tenant_id");
            record.name               = Sql::text(result, "name");
            record.version            = Sql::text(result, "version");
            record.fileName           = Sql::text(result, "file_name");
            record.contentType        = Sql::text(result, "content_type");
            record.sizeBytes          = Sql::integer(result, "size_bytes");
            record.sha256             = Sql::text(result, "sha256");
            record.signature          = Sql::text(result, "signature");
            record.signingKeyId       = Sql::text(result, "signing_key_id");
            record.signatureAlgorithm = Sql::text(result, "signature_algorithm");
            record.storagePath        = Sql::text(result, "storage_path");
            record.createdBy          = Sql::integer(result, "created_by");
            record.createdAt          = Sql::dateTimeText(result, "created_at");
            return record;
        }

        [[nodiscard]] FirmwareTaskRecord readTask(const DatabaseResult &result)
        {
            FirmwareTaskRecord record;
            record.id                 = Sql::integer(result, "id");
            record.firmwareId         = Sql::integer(result, "firmware_id");
            record.nodeId             = Sql::integer(result, "node_id");
            record.nodeName           = Sql::text(result, "node_name");
            record.status             = Sql::text(result, "status");
            record.progress           = static_cast<int>(Sql::integer(result, "progress"));
            record.message            = Sql::text(result, "message");
            record.createdAt          = Sql::dateTimeText(result, "created_at");
            record.updatedAt          = Sql::dateTimeText(result, "updated_at");
            record.previousFirmwareId = Sql::integer(result, "previous_firmware_id");
            record.rolloutId          = Sql::integer(result, "rollout_id");
            record.waveId             = Sql::integer(result, "wave_id");
            record.commandId          = Sql::integer(result, "command_id");
            return record;
        }
    } // namespace

    FirmwareRepository::FirmwareRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant) : m_database(std::move(database)), m_tenant(tenant)
    {
    }

    std::shared_ptr<FirmwareRepository> FirmwareRepository::scoped(const System::TenantScope tenant) const
    {
        return std::make_shared<FirmwareRepository>(m_database, tenant);
    }

    AsynGyanis::Core::Task<std::int64_t> FirmwareRepository::create(AsynGyanis::Core::EventLoop &loop, FirmwareRecord record)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [record = std::move(record), tenant = m_tenant](DatabaseConnection &connection) mutable -> std::int64_t
                {
                    if (record.id == 0)
                    {
                        record.id = Common::nextIdentifier();
                    }
                    if (record.createdAt.empty())
                    {
                        record.createdAt = Common::nowDateTimeUtc();
                    }
                    const std::array<DatabaseValue, 15> parameters{
                            record.id,        tenant.tenantId,  record.name,      record.version,      record.fileName,           record.contentType,
                            record.sizeBytes, record.sha256,    record.signature, record.signingKeyId, record.signatureAlgorithm, record.storagePath,
                            record.createdBy, record.createdAt, std::int64_t{0}};
                    Sql::executeOrThrowVoid(
                            connection,
                            "INSERT INTO biz_firmware (id, tenant_id, name, version, file_name, content_type, size_bytes, sha256, signature, signing_key_id, signature_algorithm, "
                            "storage_path, created_by, created_at, deleted) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                            parameters);
                    return record.id;
                });
    }

    AsynGyanis::Core::Task<Common::PageRows<FirmwareRecord>> FirmwareRepository::list(AsynGyanis::Core::EventLoop &loop, std::string keyword, const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<FirmwareRecord>>(
                loop,
                [keyword = std::move(keyword), page, tenant = m_tenant](DatabaseConnection &connection) -> Common::PageRows<FirmwareRecord>
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
                [firmwareId, tenant = m_tenant](DatabaseConnection &connection) -> std::optional<FirmwareRecord>
                {
                    std::vector<DatabaseValue> parameters{firmwareId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectFirmware} + "WHERE id = ? AND deleted = 0" + clause, parameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    return readFirmware(*rows);
                });
    }

    AsynGyanis::Core::Task<std::optional<FirmwareRecord>> FirmwareRepository::findByVersion(AsynGyanis::Core::EventLoop &loop, std::string version)
    {
        co_return co_await m_database->withConnection<std::optional<FirmwareRecord>>(
                loop,
                [version = std::move(version), tenant = m_tenant](DatabaseConnection &connection) -> std::optional<FirmwareRecord>
                {
                    std::vector<DatabaseValue> parameters{version};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows =
                            Sql::executeOrThrow(connection, std::string{kSelectFirmware} + "WHERE version = ? AND deleted = 0" + clause + " ORDER BY id DESC LIMIT 1", parameters);
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
                [firmwareId, tenant = m_tenant](DatabaseConnection &connection) -> std::optional<FirmwareRecord>
                {
                    std::vector<DatabaseValue> parameters{firmwareId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectFirmware} + "WHERE id = ? AND deleted = 0" + clause, parameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    FirmwareRecord             record = readFirmware(*rows);
                    std::vector<DatabaseValue> deleteParameters{firmwareId};
                    std::string                deleteClause;
                    if (!tenant.allTenants)
                    {
                        deleteClause = " AND tenant_id = ?";
                        deleteParameters.emplace_back(tenant.tenantId);
                    }
                    Sql::executeOrThrowVoid(connection, "UPDATE biz_firmware SET deleted = 1 WHERE id = ?" + deleteClause, deleteParameters);
                    return record;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> FirmwareRepository::unsignedCount(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await m_database->withConnection<std::int64_t>(loop,
                                                                    [tenant = m_tenant](DatabaseConnection &connection) -> std::int64_t
                                                                    {
                                                                        std::string                filter = "WHERE deleted = 0 AND signature = ''";
                                                                        std::vector<DatabaseValue> parameters;
                                                                        if (!tenant.allTenants)
                                                                        {
                                                                            filter += " AND tenant_id = ?";
                                                                            parameters.emplace_back(tenant.tenantId);
                                                                        }
                                                                        const auto rows =
                                                                                Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM biz_firmware " + filter, parameters);
                                                                        return rows->next() ? Sql::integer(*rows, "total") : 0;
                                                                    });
    }

    AsynGyanis::Core::Task<std::int64_t> FirmwareRepository::createTask(AsynGyanis::Core::EventLoop &loop, const std::int64_t firmwareId, const std::int64_t nodeId,
                                                                        const std::int64_t previousFirmwareId, const std::int64_t rolloutId, const std::int64_t waveId,
                                                                        const std::int64_t commandId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [firmwareId, nodeId, previousFirmwareId, rolloutId, waveId, commandId, tenant = m_tenant](DatabaseConnection &connection) -> std::int64_t
                {
                    // 任务与固件同租户：平台管理员跨租户下发时不能落到自身默认租户
                    std::vector<DatabaseValue> firmwareParameters{firmwareId};
                    std::string                firmwareClause;
                    if (!tenant.allTenants)
                    {
                        firmwareClause = " AND tenant_id = ?";
                        firmwareParameters.emplace_back(tenant.tenantId);
                    }
                    const auto firmware = Sql::executeOrThrow(connection, "SELECT tenant_id FROM biz_firmware WHERE id = ? AND deleted = 0" + firmwareClause, firmwareParameters);
                    if (!firmware->next())
                    {
                        return 0;
                    }
                    const std::int64_t         ownerTenantId = Sql::integer(*firmware, "tenant_id");
                    std::vector<DatabaseValue> parameters{firmwareId, nodeId, ownerTenantId};
                    const auto                 existing = Sql::executeOrThrow(
                            connection,
                            "SELECT COUNT(*) AS total FROM biz_firmware_task WHERE firmware_id = ? AND node_id = ? AND tenant_id = ? AND status IN ('pending', 'downloading')",
                            parameters);
                    if (existing->next() && Sql::integer(*existing, "total") > 0)
                    {
                        return 0;
                    }
                    const std::int64_t                  id  = Common::nextIdentifier();
                    const std::string                   now = Common::nowDateTimeUtc();
                    const std::array<DatabaseValue, 13> insertParameters{
                            id,        ownerTenantId, firmwareId, nodeId, std::string{"pending"}, std::int64_t{0}, std::string{}, now, now, previousFirmwareId,
                            rolloutId, waveId,        commandId};
                    Sql::executeOrThrowVoid(connection,
                                            "INSERT INTO biz_firmware_task (id, tenant_id, firmware_id, node_id, status, progress, message, created_at, updated_at, "
                                            "previous_firmware_id, rollout_id, wave_id, command_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                            insertParameters);
                    return id;
                });
    }

    AsynGyanis::Core::Task<bool> FirmwareRepository::bindTaskCommand(AsynGyanis::Core::EventLoop &loop, const std::int64_t taskId, const std::int64_t commandId)
    {
        if (commandId <= 0)
        {
            co_return false;
        }
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [taskId, commandId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                                                            {
                                                                std::vector<DatabaseValue> parameters{commandId, taskId};
                                                                std::string                clause;
                                                                if (!tenant.allTenants)
                                                                {
                                                                    clause = " AND tenant_id = ?";
                                                                    parameters.emplace_back(tenant.tenantId);
                                                                }
                                                                const auto update = Sql::executeOrThrow(
                                                                        connection, "UPDATE biz_firmware_task SET command_id = ? WHERE id = ?" + clause, parameters);
                                                                return update->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<std::int64_t> FirmwareRepository::latestOtherFirmware(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId, const std::int64_t excludeFirmwareId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [nodeId, excludeFirmwareId, tenant = m_tenant](DatabaseConnection &connection) -> std::int64_t
                {
                    std::vector<DatabaseValue> parameters{nodeId, excludeFirmwareId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(connection,
                                                          "SELECT firmware_id FROM biz_firmware_task WHERE node_id = ? AND firmware_id <> ?" + clause +
                                                                  " ORDER BY CASE WHEN status = 'installed' THEN 0 ELSE 1 END, updated_at DESC, id DESC LIMIT 1",
                                                          parameters);
                    return rows->next() ? Sql::integer(*rows, "firmware_id") : 0;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> FirmwareRepository::latestInstalledFirmware(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId,
                                                                                     const std::int64_t excludeFirmwareId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [nodeId, excludeFirmwareId, tenant = m_tenant](DatabaseConnection &connection) -> std::int64_t
                {
                    std::vector<DatabaseValue> parameters{nodeId, excludeFirmwareId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(connection,
                                                          "SELECT firmware_id FROM biz_firmware_task WHERE node_id = ? AND firmware_id <> ? AND status = 'installed'" + clause +
                                                                  " ORDER BY updated_at DESC, id DESC LIMIT 1",
                                                          parameters);
                    return rows->next() ? Sql::integer(*rows, "firmware_id") : 0;
                });
    }

    AsynGyanis::Core::Task<std::optional<FirmwareTaskRecord>> FirmwareRepository::findTask(AsynGyanis::Core::EventLoop &loop, const std::int64_t taskId)
    {
        co_return co_await m_database->withConnection<std::optional<FirmwareTaskRecord>>(
                loop,
                [taskId, tenant = m_tenant](DatabaseConnection &connection) -> std::optional<FirmwareTaskRecord>
                {
                    std::vector<DatabaseValue> parameters{taskId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND t.tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectTask} + "WHERE t.id = ?" + clause, parameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    return readTask(*rows);
                });
    }

    AsynGyanis::Core::Task<bool> FirmwareRepository::updateTaskById(AsynGyanis::Core::EventLoop &loop, const std::int64_t taskId, std::string status, const int progress,
                                                                    std::string message)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [taskId, status = std::move(status), progress, message = std::move(message), tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{status, static_cast<std::int64_t>(progress), message, Common::nowDateTimeUtc(), taskId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto update =
                            Sql::executeOrThrow(connection, "UPDATE biz_firmware_task SET status = ?, progress = ?, message = ?, updated_at = ? WHERE id = ?" + clause, parameters);
                    return update->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> FirmwareRepository::rollbackSource(AsynGyanis::Core::EventLoop &loop, const std::int64_t firmwareId, const std::int64_t nodeId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [firmwareId, nodeId, tenant = m_tenant](DatabaseConnection &connection) -> std::int64_t
                {
                    std::vector<DatabaseValue> parameters{firmwareId, nodeId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(connection,
                                                          "SELECT previous_firmware_id FROM biz_firmware_task WHERE firmware_id = ? AND node_id = ? AND previous_firmware_id > 0" +
                                                                  clause + " ORDER BY id DESC LIMIT 1",
                                                          parameters);
                    return rows->next() ? Sql::integer(*rows, "previous_firmware_id") : 0;
                });
    }

    AsynGyanis::Core::Task<std::vector<FirmwareTaskRecord>> FirmwareRepository::tasks(AsynGyanis::Core::EventLoop &loop, const std::int64_t firmwareId)
    {
        co_return co_await m_database->withConnection<std::vector<FirmwareTaskRecord>>(
                loop,
                [firmwareId, tenant = m_tenant](DatabaseConnection &connection) -> std::vector<FirmwareTaskRecord>
                {
                    std::vector<DatabaseValue> parameters{firmwareId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND t.tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectTask} + "WHERE t.firmware_id = ?" + clause + " ORDER BY t.id", parameters);
                    std::vector<FirmwareTaskRecord> tasks;
                    while (rows->next())
                    {
                        tasks.push_back(readTask(*rows));
                    }
                    return tasks;
                });
    }

    AsynGyanis::Core::Task<std::optional<FirmwareTaskRecord>> FirmwareRepository::updateTask(AsynGyanis::Core::EventLoop &loop, const std::int64_t firmwareId,
                                                                                             const std::int64_t nodeId, std::string status, const int progress, std::string message,
                                                                                             const std::int64_t commandId)
    {
        co_return co_await m_database->withConnection<std::optional<FirmwareTaskRecord>>(
                loop,
                [firmwareId, nodeId, status = std::move(status), progress, message = std::move(message), commandId,
                 tenant = m_tenant](DatabaseConnection &connection) -> std::optional<FirmwareTaskRecord>
                {
                    const auto applyUpdate = [&](const bool legacyOnly)
                    {
                        std::vector<DatabaseValue> parameters{status, static_cast<std::int64_t>(progress), message, Common::nowDateTimeUtc(), firmwareId, nodeId};
                        std::string                clause;
                        if (commandId != 0)
                        {
                            // 升级前建立的任务没有命令号；只有在按命令更新落空时才回退到这些行
                            clause = legacyOnly ? " AND command_id = 0" : " AND command_id = ?";
                            if (!legacyOnly)
                            {
                                parameters.emplace_back(commandId);
                            }
                        }
                        if (!tenant.allTenants)
                        {
                            clause += " AND tenant_id = ?";
                            parameters.emplace_back(tenant.tenantId);
                        }
                        const auto update = Sql::executeOrThrow(
                                connection, "UPDATE biz_firmware_task SET status = ?, progress = ?, message = ?, updated_at = ? WHERE firmware_id = ? AND node_id = ?" + clause,
                                parameters);
                        return update->affectedRowCount();
                    };

                    auto matched        = applyUpdate(false);
                    bool legacyFallback = false;
                    if (matched == 0 && commandId != 0)
                    {
                        matched        = applyUpdate(true);
                        legacyFallback = matched > 0;
                    }
                    if (matched == 0)
                    {
                        return std::nullopt;
                    }
                    std::vector<DatabaseValue> queryParameters{firmwareId, nodeId};
                    std::string                queryClause;
                    if (commandId != 0)
                    {
                        queryClause = legacyFallback ? " AND t.command_id = 0" : " AND t.command_id = ?";
                        if (!legacyFallback)
                        {
                            queryParameters.emplace_back(commandId);
                        }
                    }
                    if (!tenant.allTenants)
                    {
                        queryClause += " AND t.tenant_id = ?";
                        queryParameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(
                            connection, std::string{kSelectTask} + "WHERE t.firmware_id = ? AND t.node_id = ?" + queryClause + " ORDER BY t.id DESC LIMIT 1", queryParameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    return readTask(*rows);
                });
    }
} // namespace EdgePulse::Firmware
