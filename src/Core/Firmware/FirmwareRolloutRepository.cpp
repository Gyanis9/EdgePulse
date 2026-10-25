#include "Core/Firmware/FirmwareRolloutRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Database/SqlRetry.h"
#include "Base/Exception/EdgePulseException.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"
#include "Database/Common/DatabaseValue.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace EdgePulse::Firmware
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        constexpr std::string_view kSelectRollout =
                "SELECT id, tenant_id, firmware_id, name, policy_json, status, current_wave, total_waves, created_by, created_at, started_at, finished_at, paused_at, "
                "pause_reason, last_error, version FROM biz_firmware_rollout ";
        constexpr std::string_view kSelectWave =
                "SELECT id, rollout_id, wave_index, status, target_count, dispatched_at, deadline_at, settled_at, gate_json, created_at, updated_at "
                "FROM biz_firmware_rollout_wave ";
        constexpr std::string_view kSelectTarget = "SELECT id, rollout_id, wave_id, tenant_id, node_id, previous_firmware_id, task_id, command_id, status, health_json, updated_at "
                                                   "FROM biz_firmware_rollout_target ";

        /**
         * @brief 空文本按 SQL NULL 落库，读取时才分得清“没有时间”与“时间是空串”
         */
        [[nodiscard]] DatabaseValue nullableText(const std::string &text)
        {
            return text.empty() ? DatabaseValue{std::monostate{}} : DatabaseValue{text};
        }

        /**
         * @brief 回滚当前事务；回滚本身失败时不盖过真正的失败原因
         */
        void rollbackQuietly(DatabaseConnection &connection)
        {
            try
            {
                Sql::executeOrThrowVoid(connection, "ROLLBACK");
            } catch (...)
            {
            }
        }

        [[nodiscard]] FirmwareRolloutRecord readRollout(const DatabaseResult &result)
        {
            FirmwareRolloutRecord record;
            record.id          = Sql::integer(result, "id");
            record.tenantId    = Sql::integer(result, "tenant_id");
            record.firmwareId  = Sql::integer(result, "firmware_id");
            record.name        = Sql::text(result, "name");
            record.policyJson  = Sql::text(result, "policy_json");
            record.status      = Sql::text(result, "status");
            record.currentWave = static_cast<int>(Sql::integer(result, "current_wave"));
            record.totalWaves  = static_cast<int>(Sql::integer(result, "total_waves"));
            record.createdBy   = Sql::integer(result, "created_by");
            record.createdAt   = Sql::dateTimeText(result, "created_at");
            record.startedAt   = Sql::dateTimeText(result, "started_at");
            record.finishedAt  = Sql::dateTimeText(result, "finished_at");
            record.pausedAt    = Sql::dateTimeText(result, "paused_at");
            record.pauseReason = Sql::text(result, "pause_reason");
            record.lastError   = Sql::text(result, "last_error");
            record.version     = Sql::integer(result, "version");
            return record;
        }

        [[nodiscard]] FirmwareRolloutWaveRecord readWave(const DatabaseResult &result)
        {
            FirmwareRolloutWaveRecord record;
            record.id           = Sql::integer(result, "id");
            record.rolloutId    = Sql::integer(result, "rollout_id");
            record.waveIndex    = static_cast<int>(Sql::integer(result, "wave_index"));
            record.status       = Sql::text(result, "status");
            record.targetCount  = static_cast<int>(Sql::integer(result, "target_count"));
            record.dispatchedAt = Sql::dateTimeText(result, "dispatched_at");
            record.deadlineAt   = Sql::dateTimeText(result, "deadline_at");
            record.settledAt    = Sql::dateTimeText(result, "settled_at");
            record.gateJson     = Sql::text(result, "gate_json");
            record.createdAt    = Sql::dateTimeText(result, "created_at");
            record.updatedAt    = Sql::dateTimeText(result, "updated_at");
            return record;
        }

        [[nodiscard]] FirmwareRolloutTargetRecord readTarget(const DatabaseResult &result)
        {
            FirmwareRolloutTargetRecord record;
            record.id                 = Sql::integer(result, "id");
            record.rolloutId          = Sql::integer(result, "rollout_id");
            record.waveId             = Sql::integer(result, "wave_id");
            record.tenantId           = Sql::integer(result, "tenant_id");
            record.nodeId             = Sql::integer(result, "node_id");
            record.previousFirmwareId = Sql::integer(result, "previous_firmware_id");
            record.taskId             = Sql::integer(result, "task_id");
            record.commandId          = Sql::integer(result, "command_id");
            record.status             = Sql::text(result, "status");
            record.healthJson         = Sql::text(result, "health_json");
            record.updatedAt          = Sql::dateTimeText(result, "updated_at");
            return record;
        }

        [[nodiscard]] std::optional<FirmwareRolloutRecord> findRolloutOnConnection(DatabaseConnection &connection, const std::int64_t rolloutId, const System::TenantScope &tenant)
        {
            std::vector<DatabaseValue> parameters{rolloutId};
            std::string                clause;
            if (!tenant.allTenants)
            {
                clause = " AND tenant_id = ?";
                parameters.emplace_back(tenant.tenantId);
            }
            const auto rows = Sql::executeOrThrow(connection, std::string{kSelectRollout} + "WHERE id = ?" + clause, parameters);
            if (!rows->next())
            {
                return std::nullopt;
            }
            return readRollout(*rows);
        }

        /**
         * @brief 生成一条写入波次的语句参数；时间戳为空时落 NULL
         */
        void insertWave(DatabaseConnection &connection, const FirmwareRolloutWaveRecord &wave)
        {
            const std::vector<DatabaseValue> parameters{wave.id,
                                                        wave.rolloutId,
                                                        static_cast<std::int64_t>(wave.waveIndex),
                                                        wave.status,
                                                        static_cast<std::int64_t>(wave.targetCount),
                                                        nullableText(wave.dispatchedAt),
                                                        nullableText(wave.deadlineAt),
                                                        nullableText(wave.settledAt),
                                                        wave.gateJson,
                                                        wave.createdAt,
                                                        wave.updatedAt};
            Sql::executeOrThrowVoid(connection,
                                    "INSERT INTO biz_firmware_rollout_wave (id, rollout_id, wave_index, status, target_count, dispatched_at, deadline_at, settled_at, "
                                    "gate_json, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                    parameters);
        }

        /**
         * @brief 生成一条写入目标的语句参数
         */
        void insertTarget(DatabaseConnection &connection, const FirmwareRolloutTargetRecord &target)
        {
            const std::vector<DatabaseValue> parameters{target.id,     target.rolloutId, target.waveId, target.tenantId,   target.nodeId,   target.previousFirmwareId,
                                                        target.taskId, target.commandId, target.status, target.healthJson, target.updatedAt};
            Sql::executeOrThrowVoid(connection,
                                    "INSERT INTO biz_firmware_rollout_target (id, rollout_id, wave_id, tenant_id, node_id, previous_firmware_id, task_id, command_id, "
                                    "status, health_json, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                    parameters);
        }
    } // namespace

    FirmwareRolloutRepository::FirmwareRolloutRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant) : m_database(std::move(database)), m_tenant(tenant)
    {
    }

    std::shared_ptr<FirmwareRolloutRepository> FirmwareRolloutRepository::scoped(const System::TenantScope tenant) const
    {
        return std::make_shared<FirmwareRolloutRepository>(m_database, tenant);
    }

    AsynGyanis::Core::Task<std::int64_t> FirmwareRolloutRepository::create(AsynGyanis::Core::EventLoop &loop, FirmwareRolloutDraft draft)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [draft = std::move(draft), tenant = m_tenant](DatabaseConnection &connection) mutable -> std::int64_t
                {
                    // 固件必须先存在并与发布同租户：跨租户引用会让目标节点与固件分属两个租户
                    std::vector<DatabaseValue> firmwareParameters{draft.rollout.firmwareId};
                    std::string                firmwareClause;
                    if (!tenant.allTenants)
                    {
                        firmwareClause = " AND tenant_id = ?";
                        firmwareParameters.emplace_back(tenant.tenantId);
                    }
                    const auto firmware = Sql::executeOrThrow(connection, "SELECT tenant_id FROM biz_firmware WHERE id = ? AND deleted = 0" + firmwareClause, firmwareParameters);
                    if (!firmware->next())
                    {
                        throw Common::DatabaseException("创建灰度发布失败：固件不存在或不属于当前租户");
                    }
                    const std::int64_t ownerTenantId = Sql::integer(*firmware, "tenant_id");

                    const std::string      now     = Common::nowDateTimeUtc();
                    FirmwareRolloutRecord &rollout = draft.rollout;
                    if (rollout.id == 0)
                    {
                        rollout.id = Common::nextIdentifier();
                    }
                    rollout.tenantId   = ownerTenantId;
                    rollout.totalWaves = static_cast<int>(draft.waves.size());
                    if (rollout.status.empty())
                    {
                        rollout.status = "draft";
                    }
                    if (rollout.createdAt.empty())
                    {
                        rollout.createdAt = now;
                    }
                    if (rollout.version <= 0)
                    {
                        rollout.version = 1;
                    }

                    try
                    {
                        Sql::executeOrThrowVoid(connection, "BEGIN");
                        const std::vector<DatabaseValue> rolloutParameters{rollout.id,
                                                                           ownerTenantId,
                                                                           rollout.firmwareId,
                                                                           rollout.name,
                                                                           rollout.policyJson,
                                                                           rollout.status,
                                                                           static_cast<std::int64_t>(rollout.currentWave),
                                                                           static_cast<std::int64_t>(rollout.totalWaves),
                                                                           rollout.createdBy,
                                                                           rollout.createdAt,
                                                                           nullableText(rollout.startedAt),
                                                                           nullableText(rollout.finishedAt),
                                                                           nullableText(rollout.pausedAt),
                                                                           rollout.pauseReason,
                                                                           rollout.lastError,
                                                                           rollout.version};
                        Sql::executeOrThrowVoid(connection,
                                                "INSERT INTO biz_firmware_rollout (id, tenant_id, firmware_id, name, policy_json, status, current_wave, total_waves, created_by, "
                                                "created_at, started_at, finished_at, paused_at, pause_reason, last_error, version) "
                                                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                                rolloutParameters);

                        for (FirmwareRolloutWaveDraft &wave: draft.waves)
                        {
                            if (wave.wave.id == 0)
                            {
                                wave.wave.id = Common::nextIdentifier();
                            }
                            wave.wave.rolloutId   = rollout.id;
                            wave.wave.targetCount = static_cast<int>(wave.targets.size());
                            if (wave.wave.status.empty())
                            {
                                wave.wave.status = "pending";
                            }
                            if (wave.wave.createdAt.empty())
                            {
                                wave.wave.createdAt = now;
                            }
                            if (wave.wave.updatedAt.empty())
                            {
                                wave.wave.updatedAt = now;
                            }
                            insertWave(connection, wave.wave);

                            for (FirmwareRolloutTargetRecord &target: wave.targets)
                            {
                                if (target.id == 0)
                                {
                                    target.id = Common::nextIdentifier();
                                }
                                target.rolloutId = rollout.id;
                                target.waveId    = wave.wave.id;
                                target.tenantId  = ownerTenantId;
                                if (target.status.empty())
                                {
                                    target.status = "pending";
                                }
                                if (target.updatedAt.empty())
                                {
                                    target.updatedAt = now;
                                }
                                insertTarget(connection, target);
                            }
                        }
                        Sql::executeOrThrowVoid(connection, "COMMIT");
                        return rollout.id;
                    } catch (const Common::DatabaseException &failure)
                    {
                        // 头、波次或任一目标失败都要整笔回滚，不能留下半份聚合
                        rollbackQuietly(connection);
                        // 目标表只有 (rollout_id, node_id) 这一条唯一约束，撞上它说明同一节点在发布里出现了两次
                        if (Sql::isUniqueViolation(failure.what()))
                        {
                            throw Common::DatabaseException("创建灰度发布失败：同一节点在同一发布中重复出现，node_id 必须唯一");
                        }
                        throw;
                    } catch (...)
                    {
                        rollbackQuietly(connection);
                        throw;
                    }
                });
    }

    AsynGyanis::Core::Task<std::optional<FirmwareRolloutAggregate>> FirmwareRolloutRepository::find(AsynGyanis::Core::EventLoop &loop, const std::int64_t rolloutId)
    {
        co_return co_await m_database->withConnection<std::optional<FirmwareRolloutAggregate>>(
                loop,
                [rolloutId, tenant = m_tenant](DatabaseConnection &connection) -> std::optional<FirmwareRolloutAggregate>
                {
                    const std::optional<FirmwareRolloutRecord> rollout = findRolloutOnConnection(connection, rolloutId, tenant);
                    if (!rollout.has_value())
                    {
                        return std::nullopt;
                    }
                    FirmwareRolloutAggregate aggregate;
                    aggregate.rollout = *rollout;

                    const auto waves =
                            Sql::executeOrThrow(connection, std::string{kSelectWave} + "WHERE rollout_id = ? ORDER BY wave_index, id", std::vector<DatabaseValue>{rolloutId});
                    while (waves->next())
                    {
                        aggregate.waves.push_back(readWave(*waves));
                    }

                    std::vector<DatabaseValue> targetParameters{rolloutId};
                    std::string                targetClause;
                    if (!tenant.allTenants)
                    {
                        targetClause = " AND tenant_id = ?";
                        targetParameters.emplace_back(tenant.tenantId);
                    }
                    const auto targets =
                            Sql::executeOrThrow(connection, std::string{kSelectTarget} + "WHERE rollout_id = ?" + targetClause + " ORDER BY wave_id, id", targetParameters);
                    while (targets->next())
                    {
                        aggregate.targets.push_back(readTarget(*targets));
                    }
                    return aggregate;
                });
    }

    AsynGyanis::Core::Task<Common::PageRows<FirmwareRolloutRecord>> FirmwareRolloutRepository::list(AsynGyanis::Core::EventLoop &loop, const std::int64_t firmwareId,
                                                                                                    const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<FirmwareRolloutRecord>>(
                loop,
                [firmwareId, page, tenant = m_tenant](DatabaseConnection &connection) -> Common::PageRows<FirmwareRolloutRecord>
                {
                    std::string                filter = "WHERE 1 = 1";
                    std::vector<DatabaseValue> parameters;
                    if (!tenant.allTenants)
                    {
                        filter += " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    if (firmwareId > 0)
                    {
                        filter += " AND firmware_id = ?";
                        parameters.emplace_back(firmwareId);
                    }
                    const auto                              count = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM biz_firmware_rollout " + filter, parameters);
                    Common::PageRows<FirmwareRolloutRecord> pageRows;
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
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectRollout} + filter + " ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?", parameters);
                    while (rows->next())
                    {
                        pageRows.rows.push_back(readRollout(*rows));
                    }
                    return pageRows;
                });
    }

    AsynGyanis::Core::Task<bool> FirmwareRolloutRepository::updateStatus(AsynGyanis::Core::EventLoop &loop, const std::int64_t rolloutId, const std::int64_t expectedVersion,
                                                                         std::string status, const int currentWave, std::string pauseReason, std::string lastError)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [rolloutId, expectedVersion, status = std::move(status), currentWave, pauseReason = std::move(pauseReason), lastError = std::move(lastError),
                 tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::optional<FirmwareRolloutRecord> current = findRolloutOnConnection(connection, rolloutId, tenant);
                    if (!current.has_value() || current->version != expectedVersion)
                    {
                        return false;
                    }
                    const std::string now = Common::nowDateTimeUtc();
                    current->status       = status;
                    if (currentWave >= 0)
                    {
                        current->currentWave = currentWave;
                    }
                    if (status == "running")
                    {
                        // 恢复运行清掉暂停现场；开始时间只在首次进入运行时落一次
                        if (current->startedAt.empty())
                        {
                            current->startedAt = now;
                        }
                        current->pausedAt.clear();
                        current->pauseReason.clear();
                    } else if (status == "paused")
                    {
                        current->pausedAt    = now;
                        current->pauseReason = pauseReason;
                    }
                    if (status == "completed" || status == "rolled_back" || status == "aborted" || status == "failed")
                    {
                        current->finishedAt = now;
                    }
                    if (!lastError.empty())
                    {
                        current->lastError = lastError;
                    }

                    std::vector<DatabaseValue> parameters{current->status,
                                                          static_cast<std::int64_t>(current->currentWave),
                                                          current->pauseReason,
                                                          current->lastError,
                                                          nullableText(current->startedAt),
                                                          nullableText(current->pausedAt),
                                                          nullableText(current->finishedAt),
                                                          rolloutId,
                                                          expectedVersion};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    // 版本条件把读取与写入绑成一次比较交换：期间提交的状态推进会让这次更新落空
                    const auto update = Sql::executeOrThrow(connection,
                                                            "UPDATE biz_firmware_rollout SET status = ?, current_wave = ?, pause_reason = ?, last_error = ?, started_at = ?, "
                                                            "paused_at = ?, finished_at = ?, version = version + 1 WHERE id = ? AND version = ?" +
                                                                    clause,
                                                            parameters);
                    return update->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<bool> FirmwareRolloutRepository::setWave(AsynGyanis::Core::EventLoop &loop, const std::int64_t waveId, std::string status, std::string dispatchedAt,
                                                                    std::string deadlineAt, std::string settledAt)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [waveId, status = std::move(status), dispatchedAt = std::move(dispatchedAt), deadlineAt = std::move(deadlineAt), settledAt = std::move(settledAt),
                 tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{status, dispatchedAt, dispatchedAt, deadlineAt, deadlineAt, settledAt, settledAt, Common::nowDateTimeUtc(), waveId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        // 波次表没有租户列，用发布头做存在性校验
                        clause = " AND EXISTS (SELECT 1 FROM biz_firmware_rollout r WHERE r.id = biz_firmware_rollout_wave.rollout_id AND r.tenant_id = ?)";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto update = Sql::executeOrThrow(connection,
                                                            "UPDATE biz_firmware_rollout_wave SET status = ?, "
                                                            "dispatched_at = CASE WHEN ? = '' THEN dispatched_at ELSE ? END, "
                                                            "deadline_at = CASE WHEN ? = '' THEN deadline_at ELSE ? END, "
                                                            "settled_at = CASE WHEN ? = '' THEN settled_at ELSE ? END, updated_at = ? WHERE id = ?" +
                                                                    clause,
                                                            parameters);
                    return update->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<bool> FirmwareRolloutRepository::appendGate(AsynGyanis::Core::EventLoop &loop, const std::int64_t waveId, std::string gateJson)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [waveId, gateJson = std::move(gateJson), tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    try
                    {
                        Sql::executeOrThrowVoid(connection, "BEGIN");
                        std::vector<DatabaseValue> parameters{waveId};
                        std::string                clause;
                        if (!tenant.allTenants)
                        {
                            clause = " AND r.tenant_id = ?";
                            parameters.emplace_back(tenant.tenantId);
                        }
                        const auto rows = Sql::executeOrThrow(connection,
                                                              "SELECT w.gate_json FROM biz_firmware_rollout_wave w JOIN biz_firmware_rollout r ON r.id = w.rollout_id "
                                                              "WHERE w.id = ?" +
                                                                      clause,
                                                              parameters);
                        if (!rows->next())
                        {
                            rollbackQuietly(connection);
                            return false;
                        }
                        std::string existing = Sql::text(*rows, "gate_json");
                        if (!existing.empty())
                        {
                            // 门控快照按行追加，保留历次判定的顺序
                            existing.push_back('\n');
                        }
                        existing += gateJson;
                        Sql::executeOrThrowVoid(connection, "UPDATE biz_firmware_rollout_wave SET gate_json = ?, updated_at = ? WHERE id = ?",
                                                std::vector<DatabaseValue>{existing, Common::nowDateTimeUtc(), waveId});
                        Sql::executeOrThrowVoid(connection, "COMMIT");
                        return true;
                    } catch (...)
                    {
                        rollbackQuietly(connection);
                        throw;
                    }
                });
    }

    AsynGyanis::Core::Task<bool> FirmwareRolloutRepository::updateTarget(AsynGyanis::Core::EventLoop &loop, const std::int64_t targetId, FirmwareRolloutTargetUpdate update)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [targetId, update = std::move(update), tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{
                            update.status,    update.healthJson,         update.healthJson,         update.taskId, update.taskId, update.commandId,
                            update.commandId, update.previousFirmwareId, update.previousFirmwareId, update.waveId, update.waveId, Common::nowDateTimeUtc(),
                            targetId};
                    std::string clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto result = Sql::executeOrThrow(connection,
                                                            "UPDATE biz_firmware_rollout_target SET status = ?, "
                                                            "health_json = CASE WHEN ? = '' THEN health_json ELSE ? END, "
                                                            "task_id = CASE WHEN ? >= 0 THEN ? ELSE task_id END, "
                                                            "command_id = CASE WHEN ? >= 0 THEN ? ELSE command_id END, "
                                                            "previous_firmware_id = CASE WHEN ? >= 0 THEN ? ELSE previous_firmware_id END, "
                                                            "wave_id = CASE WHEN ? >= 0 THEN ? ELSE wave_id END, updated_at = ? WHERE id = ?" +
                                                                    clause,
                                                            parameters);
                    return result->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<std::vector<FirmwareRolloutRecord>> FirmwareRolloutRepository::listDueRollouts(AsynGyanis::Core::EventLoop &loop, const std::size_t limit)
    {
        co_return co_await m_database->withConnection<std::vector<FirmwareRolloutRecord>>(
                loop,
                [limit, tenant = m_tenant](DatabaseConnection &connection) -> std::vector<FirmwareRolloutRecord>
                {
                    // 暂停与草稿要等人工动作，只有这些状态需要协调器推进
                    std::string                filter = "WHERE status IN ('scheduled', 'preparing', 'running', 'rolling_back')";
                    std::vector<DatabaseValue> parameters;
                    if (!tenant.allTenants)
                    {
                        filter += " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    parameters.emplace_back(static_cast<std::int64_t>(limit));
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectRollout} + filter + " ORDER BY created_at, id LIMIT ?", parameters);
                    std::vector<FirmwareRolloutRecord> rollouts;
                    while (rows->next())
                    {
                        rollouts.push_back(readRollout(*rows));
                    }
                    return rollouts;
                });
    }
} // namespace EdgePulse::Firmware
