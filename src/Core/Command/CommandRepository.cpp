#include "Core/Command/CommandRepository.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace EdgePulse::Command
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        constexpr std::string_view kSelectCommand = "SELECT c.id, c.tenant_id, c.node_id, c.type, c.payload, c.status, c.result, c.message, c.created_by, c.created_at, "
                                                    "c.dispatched_at, c.finished_at, c.timeout_seconds, c.scheduled_at, c.batch_id, "
                                                    "COALESCE(n.name, '') AS node_name FROM biz_command c LEFT JOIN biz_node n ON n.id = c.node_id ";

        [[nodiscard]] CommandRecord readCommand(const DatabaseResult &result)
        {
            CommandRecord command;
            command.id             = Sql::integer(result, "id");
            command.tenantId       = Sql::integer(result, "tenant_id");
            command.nodeId         = Sql::integer(result, "node_id");
            command.nodeName       = Sql::text(result, "node_name");
            command.type           = Sql::text(result, "type");
            command.payload        = Sql::text(result, "payload");
            command.status         = Sql::text(result, "status");
            command.result         = Sql::text(result, "result");
            command.message        = Sql::text(result, "message");
            command.createdBy      = Sql::integer(result, "created_by");
            command.createdAt      = Sql::dateTimeText(result, "created_at");
            command.dispatchedAt   = Sql::dateTimeText(result, "dispatched_at");
            command.finishedAt     = Sql::dateTimeText(result, "finished_at");
            command.timeoutSeconds = static_cast<int>(Sql::integer(result, "timeout_seconds"));
            command.scheduledAt    = Sql::dateTimeText(result, "scheduled_at");
            command.batchId        = Sql::integer(result, "batch_id");
            return command;
        }

        [[nodiscard]] std::optional<CommandRecord> findOnConnection(DatabaseConnection &connection, const std::int64_t commandId, const System::TenantScope tenant)
        {
            std::vector<DatabaseValue> parameters{commandId};
            std::string                clause;
            if (!tenant.allTenants)
            {
                clause = " AND c.tenant_id = ?";
                parameters.emplace_back(tenant.tenantId);
            }
            const auto rows = Sql::executeOrThrow(connection, std::string{kSelectCommand} + "WHERE c.id = ?" + clause, parameters);
            if (!rows->next())
            {
                return std::nullopt;
            }
            return readCommand(*rows);
        }

        /**
         * @brief 把 SQL DATETIME 文本（YYYY-MM-DD HH:MM:SS）解析成 UTC 时刻
         * @param text 数据库读出的时间文本
         * @return std::optional<std::chrono::system_clock::time_point> 合法时返回时刻；空串或格式错误时为空
         */
        [[nodiscard]] std::optional<std::chrono::system_clock::time_point> parseSqlDateTime(const std::string_view text)
        {
            if (text.size() < 19 || text[4] != '-' || text[7] != '-' || text[10] != ' ' || text[13] != ':' || text[16] != ':')
            {
                return std::nullopt;
            }
            const auto readPart = [](const std::string_view source, const std::size_t offset, const std::size_t length) -> std::optional<int>
            {
                int value = 0;
                const auto [end, error] = std::from_chars(source.data() + offset, source.data() + offset + length, value);
                if (error != std::errc{} || end != source.data() + offset + length)
                {
                    return std::nullopt;
                }
                return value;
            };
            const std::optional<int> year   = readPart(text, 0, 4);
            const std::optional<int> month  = readPart(text, 5, 2);
            const std::optional<int> day    = readPart(text, 8, 2);
            const std::optional<int> hour   = readPart(text, 11, 2);
            const std::optional<int> minute = readPart(text, 14, 2);
            const std::optional<int> second = readPart(text, 17, 2);
            if (!year.has_value() || !month.has_value() || !day.has_value() || !hour.has_value() || !minute.has_value() || !second.has_value())
            {
                return std::nullopt;
            }
            const std::chrono::year_month_day calendar{std::chrono::year{*year}, std::chrono::month{static_cast<unsigned>(*month)},
                                                       std::chrono::day{static_cast<unsigned>(*day)}};
            if (!calendar.ok() || *hour < 0 || *hour > 23 || *minute < 0 || *minute > 59 || *second < 0 || *second > 59)
            {
                return std::nullopt;
            }
            return std::chrono::sys_days{calendar} + std::chrono::hours{*hour} + std::chrono::minutes{*minute} + std::chrono::seconds{*second};
        }

        /**
         * @brief 从命令载荷里取固件标识，供旧版未绑定任务按固件与节点兜底匹配
         */
        [[nodiscard]] std::int64_t payloadFirmwareId(const std::string &payload)
        {
            const std::optional<AsynGyanis::Base::ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(payload);
            if (!parsed.has_value() || !parsed->is_object() || !parsed->contains("firmwareId"))
            {
                return 0;
            }
            return AsynGyanis::Base::configValueAs<std::int64_t>(parsed->at("firmwareId")).value_or(0);
        }

        constexpr std::string_view kCommandTimeoutMessage = "执行超时：超过 timeoutSeconds 与宽限期仍未收到结果";
    } // namespace

    CommandRepository::CommandRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant) : m_database(std::move(database)), m_tenant(tenant)
    {
    }

    std::shared_ptr<CommandRepository> CommandRepository::scoped(const System::TenantScope tenant) const
    {
        return std::make_shared<CommandRepository>(m_database, tenant);
    }

    AsynGyanis::Core::Task<std::optional<CommandRecord>> CommandRepository::create(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId, std::string type,
                                                                                   std::string payload, const std::int64_t createdBy, const int timeoutSeconds,
                                                                                   std::string scheduledAt, const std::int64_t batchId)
    {
        co_return co_await m_database->withConnection<std::optional<CommandRecord>>(
                loop,
                [nodeId, type = std::move(type), payload = std::move(payload), createdBy, timeoutSeconds, scheduledAt = std::move(scheduledAt), batchId,
                 tenant = m_tenant](DatabaseConnection &connection) -> std::optional<CommandRecord>
                {
                    std::string                nodeSql = "SELECT tenant_id FROM biz_node WHERE id = ? AND deleted = 0";
                    std::vector<DatabaseValue> nodeParameters{nodeId};
                    if (!tenant.allTenants)
                    {
                        nodeSql += " AND tenant_id = ?";
                        nodeParameters.emplace_back(tenant.tenantId);
                    }
                    const auto node = Sql::executeOrThrow(connection, nodeSql, nodeParameters);
                    if (!node->next())
                    {
                        return std::nullopt;
                    }
                    // 命令归属目标节点所属租户：平台管理员跨租户下发时也落在节点自己的租户下
                    const std::int64_t                  ownerTenantId  = Sql::integer(*node, "tenant_id");
                    const std::int64_t                  id             = Common::nextIdentifier();
                    const std::string                   now            = Common::nowDateTimeUtc();
                    const DatabaseValue                 scheduledValue = scheduledAt.empty() ? DatabaseValue{std::monostate{}} : DatabaseValue{scheduledAt};
                    const std::array<DatabaseValue, 12> parameters{id,
                                                                   ownerTenantId,
                                                                   nodeId,
                                                                   type,
                                                                   payload,
                                                                   std::string{"pending"},
                                                                   createdBy,
                                                                   now,
                                                                   static_cast<std::int64_t>(timeoutSeconds),
                                                                   std::string{"pending"},
                                                                   scheduledValue,
                                                                   batchId};
                    Sql::executeOrThrowVoid(connection,
                                            "INSERT INTO biz_command (id, tenant_id, node_id, type, payload, status, created_by, created_at, timeout_seconds, message, "
                                            "scheduled_at, batch_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                            parameters);
                    return findOnConnection(connection, id, tenant);
                });
    }

    AsynGyanis::Core::Task<Common::PageRows<CommandRecord>> CommandRepository::list(AsynGyanis::Core::EventLoop &loop, std::string status, const std::int64_t nodeId,
                                                                                    const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<CommandRecord>>(
                loop,
                [status = std::move(status), nodeId, page, tenant = m_tenant](DatabaseConnection &connection) -> Common::PageRows<CommandRecord>
                {
                    std::string                filter = "WHERE 1 = 1";
                    std::vector<DatabaseValue> parameters;
                    if (!tenant.allTenants)
                    {
                        filter += " AND c.tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    if (!status.empty())
                    {
                        filter += " AND c.status = ?";
                        parameters.emplace_back(status);
                    }
                    if (nodeId > 0)
                    {
                        filter += " AND c.node_id = ?";
                        parameters.emplace_back(nodeId);
                    }
                    const auto                      count = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM biz_command c " + filter, parameters);
                    Common::PageRows<CommandRecord> pageRows;
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
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectCommand} + filter + " ORDER BY c.created_at DESC, c.id DESC LIMIT ? OFFSET ?", parameters);
                    while (rows->next())
                    {
                        pageRows.rows.push_back(readCommand(*rows));
                    }
                    return pageRows;
                });
    }

    AsynGyanis::Core::Task<std::optional<CommandRecord>> CommandRepository::find(AsynGyanis::Core::EventLoop &loop, const std::int64_t commandId)
    {
        co_return co_await m_database->withConnection<std::optional<CommandRecord>>(
                loop, [commandId, tenant = m_tenant](DatabaseConnection &connection) -> std::optional<CommandRecord> { return findOnConnection(connection, commandId, tenant); });
    }

    AsynGyanis::Core::Task<std::optional<CommandRecord>> CommandRepository::takeNext(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        co_return co_await m_database->withConnection<std::optional<CommandRecord>>(
                loop,
                [nodeId, tenant = m_tenant](DatabaseConnection &connection) -> std::optional<CommandRecord>
                {
                    Sql::executeOrThrowVoid(connection, "BEGIN");
                    try
                    {
                        std::string                select = "SELECT id FROM biz_command WHERE node_id = ? AND status = 'pending'";
                        std::string                clause;
                        std::vector<DatabaseValue> nodeParameter{nodeId};
                        std::vector<DatabaseValue> updateParameter;
                        if (!tenant.allTenants)
                        {
                            select += " AND tenant_id = ?";
                            clause = " AND tenant_id = ?";
                            nodeParameter.emplace_back(tenant.tenantId);
                        }
                        select += " AND (scheduled_at IS NULL OR scheduled_at <= ?)";
                        nodeParameter.emplace_back(Common::nowDateTimeUtc());
                        const auto rows = Sql::executeOrThrow(connection, select + " ORDER BY created_at, id LIMIT 1", nodeParameter);
                        if (!rows->next())
                        {
                            Sql::executeOrThrowVoid(connection, "COMMIT");
                            return std::nullopt;
                        }
                        const std::int64_t id = Sql::integer(*rows, "id");
                        updateParameter.emplace_back(Common::nowDateTimeUtc());
                        updateParameter.emplace_back(id);
                        if (!tenant.allTenants)
                        {
                            updateParameter.emplace_back(tenant.tenantId);
                        }
                        const auto update = Sql::executeOrThrow(
                                connection, "UPDATE biz_command SET status = 'running', dispatched_at = ? WHERE id = ? AND status = 'pending'" + clause, updateParameter);
                        if (update->affectedRowCount() == 0)
                        {
                            Sql::executeOrThrowVoid(connection, "ROLLBACK");
                            return std::nullopt;
                        }
                        Sql::executeOrThrowVoid(connection, "COMMIT");
                        return findOnConnection(connection, id, tenant);
                    } catch (...)
                    {
                        try
                        {
                            Sql::executeOrThrowVoid(connection, "ROLLBACK");
                        } catch (...)
                        {
                        }
                        throw;
                    }
                });
    }

    AsynGyanis::Core::Task<std::vector<CommandRecord>> CommandRepository::listDue(AsynGyanis::Core::EventLoop &loop, const std::size_t limit)
    {
        co_return co_await m_database->withConnection<std::vector<CommandRecord>>(
                loop,
                [limit, tenant = m_tenant](DatabaseConnection &connection) -> std::vector<CommandRecord>
                {
                    std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc()};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND c.tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    parameters.emplace_back(static_cast<std::int64_t>(limit));
                    const auto rows = Sql::executeOrThrow(connection,
                                                          std::string{kSelectCommand} + "WHERE c.status = 'pending' AND c.scheduled_at IS NOT NULL AND c.scheduled_at <= ?" +
                                                                  clause + " ORDER BY c.scheduled_at, c.id LIMIT ?",
                                                          parameters);
                    std::vector<CommandRecord> commands;
                    while (rows->next())
                    {
                        commands.push_back(readCommand(*rows));
                    }
                    return commands;
                });
    }

    AsynGyanis::Core::Task<bool> CommandRepository::markDispatched(AsynGyanis::Core::EventLoop &loop, const std::int64_t commandId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [commandId, tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), commandId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(connection, "UPDATE biz_command SET status = 'running', dispatched_at = ? WHERE id = ? AND status = 'pending'" + clause,
                                                          parameters);
                    return rows->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<std::optional<CommandRecord>> CommandRepository::finish(AsynGyanis::Core::EventLoop &loop, const std::int64_t commandId, std::string status,
                                                                                   std::string result, std::string message)
    {
        co_return co_await m_database->withConnection<std::optional<CommandRecord>>(
                loop,
                [commandId, status = std::move(status), result = std::move(result), message = std::move(message),
                 tenant = m_tenant](DatabaseConnection &connection) -> std::optional<CommandRecord>
                {
                    if (status != "succeeded" && status != "failed" && status != "timeout")
                    {
                        return std::nullopt;
                    }
                    std::vector<DatabaseValue> parameters{status, result, message, Common::nowDateTimeUtc(), commandId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(connection,
                                                          "UPDATE biz_command SET status = ?, result = ?, message = ?, finished_at = ? "
                                                          "WHERE id = ? AND status IN ('pending', 'running')" +
                                                                  clause,
                                                          parameters);
                    if (rows->affectedRowCount() == 0)
                    {
                        return std::nullopt;
                    }
                    return findOnConnection(connection, commandId, tenant);
                });
    }

    AsynGyanis::Core::Task<std::vector<CommandRecord>> CommandRepository::cancelBatch(AsynGyanis::Core::EventLoop &loop, const std::int64_t batchId)
    {
        co_return co_await m_database->withConnection<std::vector<CommandRecord>>(
                loop,
                [batchId, tenant = m_tenant](DatabaseConnection &connection) -> std::vector<CommandRecord>
                {
                    std::vector<DatabaseValue> parameters{batchId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND c.tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(
                            connection, "SELECT c.id FROM biz_command c WHERE c.batch_id = ? AND c.status IN ('pending', 'running')" + clause + " ORDER BY c.id", parameters);
                    std::vector<std::int64_t> ids;
                    while (rows->next())
                    {
                        ids.push_back(Sql::integer(*rows, "id"));
                    }
                    if (ids.empty())
                    {
                        return {};
                    }
                    std::vector<DatabaseValue> updateParameters{Common::nowDateTimeUtc(), batchId};
                    std::string                updateClause;
                    if (!tenant.allTenants)
                    {
                        updateClause = " AND tenant_id = ?";
                        updateParameters.emplace_back(tenant.tenantId);
                    }
                    Sql::executeOrThrowVoid(
                            connection,
                            "UPDATE biz_command SET status = 'canceled', message = '已批量取消', finished_at = ? WHERE batch_id = ? AND status IN ('pending', 'running')" +
                                    updateClause,
                            updateParameters);
                    std::vector<CommandRecord> commands;
                    for (const std::int64_t commandId: ids)
                    {
                        if (auto command = findOnConnection(connection, commandId, tenant); command.has_value())
                        {
                            commands.push_back(std::move(*command));
                        }
                    }
                    return commands;
                });
    }

    AsynGyanis::Core::Task<std::optional<CommandRecord>> CommandRepository::cancel(AsynGyanis::Core::EventLoop &loop, const std::int64_t commandId)
    {
        co_return co_await m_database->withConnection<std::optional<CommandRecord>>(
                loop,
                [commandId, tenant = m_tenant](DatabaseConnection &connection) -> std::optional<CommandRecord>
                {
                    std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), commandId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(
                            connection,
                            "UPDATE biz_command SET status = 'canceled', message = '已取消', finished_at = ? WHERE id = ? AND status IN ('pending', 'running')" + clause,
                            parameters);
                    if (rows->affectedRowCount() == 0)
                    {
                        return std::nullopt;
                    }
                    return findOnConnection(connection, commandId, tenant);
                });
    }

    AsynGyanis::Core::Task<CommandTimeoutSweepResult> CommandRepository::expireTimedOut(AsynGyanis::Core::EventLoop &loop, const std::chrono::system_clock::time_point now,
                                                                                      const int graceSeconds, const std::size_t limit)
    {
        co_return co_await m_database->withConnection<CommandTimeoutSweepResult>(
                loop,
                [now, graceSeconds, limit, tenant = m_tenant](DatabaseConnection &connection) -> CommandTimeoutSweepResult
                {
                    struct Candidate
                    {
                        std::int64_t id{0};
                        std::int64_t tenantId{1};
                        std::int64_t nodeId{0};
                        std::string  payload;
                    };

                    Sql::executeOrThrowVoid(connection, "BEGIN");
                    try
                    {
                        std::vector<DatabaseValue> parameters;
                        std::string                clause;
                        if (!tenant.allTenants)
                        {
                            clause = " AND c.tenant_id = ?";
                            parameters.emplace_back(tenant.tenantId);
                        }
                        parameters.emplace_back(static_cast<std::int64_t>(limit));
                        // 先按真正开始执行的时刻排序：running 看 dispatched_at，pending 看 scheduled_at；
                        // 两者都为空时才回退 created_at。窗口必须有界，单次最多处理 limit 条
                        const auto rows = Sql::executeOrThrow(connection,
                                                              "SELECT c.id, c.tenant_id, c.node_id, c.payload, c.status, c.created_at, c.dispatched_at, c.scheduled_at, "
                                                              "c.timeout_seconds FROM biz_command c WHERE c.status IN ('pending', 'running') AND c.timeout_seconds > 0" +
                                                                      clause + " ORDER BY COALESCE(c.dispatched_at, c.scheduled_at, c.created_at), c.id LIMIT ?",
                                                              parameters);

                        const auto nowSeconds = std::chrono::floor<std::chrono::seconds>(now);
                        const auto grace      = std::chrono::seconds{std::max(0, graceSeconds)};
                        std::vector<Candidate> candidates;
                        while (rows->next())
                        {
                            const int         timeoutSeconds = static_cast<int>(Sql::integer(*rows, "timeout_seconds"));
                            const std::string status         = Sql::text(*rows, "status");
                            const std::string dispatchedAt   = Sql::dateTimeText(*rows, "dispatched_at");
                            const std::string scheduledAt    = Sql::dateTimeText(*rows, "scheduled_at");
                            const std::string createdAt      = Sql::dateTimeText(*rows, "created_at");
                            std::string       baseText       = createdAt;
                            if (status == "running" && !dispatchedAt.empty())
                            {
                                baseText = dispatchedAt;
                            } else if (status != "running" && !scheduledAt.empty())
                            {
                                baseText = scheduledAt;
                            }
                            const std::optional<std::chrono::system_clock::time_point> base = parseSqlDateTime(baseText);
                            if (timeoutSeconds <= 0 || !base.has_value())
                            {
                                continue;
                            }
                            if (*base + std::chrono::seconds{timeoutSeconds} + grace > nowSeconds)
                            {
                                continue;
                            }
                            candidates.push_back(Candidate{Sql::integer(*rows, "id"), Sql::integer(*rows, "tenant_id"), Sql::integer(*rows, "node_id"),
                                                           Sql::text(*rows, "payload")});
                        }

                        CommandTimeoutSweepResult result;
                        const std::string         finishedAt = Common::formatDateTimeUtc(now);
                        for (const Candidate &candidate: candidates)
                        {
                            const auto updated = Sql::executeOrThrow(
                                    connection,
                                    "UPDATE biz_command SET status = 'timeout', result = ?, message = ?, finished_at = ? WHERE id = ? AND status IN ('pending', 'running')",
                                    std::array<DatabaseValue, 4>{std::string{"{}"}, std::string{kCommandTimeoutMessage}, finishedAt, candidate.id});
                            if (updated->affectedRowCount() == 0)
                            {
                                continue;
                            }

                            // 绑定任务按 command_id 精确命中；旧版任务用负值占位，按载荷固件与节点兜底
                            std::vector<DatabaseValue> taskParameters{std::string{kCommandTimeoutMessage}, finishedAt, candidate.id, candidate.tenantId};
                            const auto                 boundTasks = Sql::executeOrThrow(
                                    connection,
                                    "UPDATE biz_firmware_task SET status = 'failed', progress = 0, message = ?, updated_at = ? WHERE command_id = ? AND tenant_id = ? "
                                    "AND status NOT IN ('installed', 'failed')",
                                    taskParameters);
                            std::size_t failedTasks = boundTasks->affectedRowCount();
                            if (failedTasks == 0)
                            {
                                if (const std::int64_t firmwareId = payloadFirmwareId(candidate.payload); firmwareId > 0)
                                {
                                    const std::array<DatabaseValue, 5> legacyParameters{std::string{kCommandTimeoutMessage}, finishedAt, firmwareId, candidate.nodeId,
                                                                                        candidate.tenantId};
                                    const auto                         legacyTasks = Sql::executeOrThrow(
                                            connection,
                                            "UPDATE biz_firmware_task SET status = 'failed', progress = 0, message = ?, updated_at = ? WHERE firmware_id = ? AND node_id = ? "
                                            "AND tenant_id = ? AND command_id < 0 AND status NOT IN ('installed', 'failed')",
                                            legacyParameters);
                                    failedTasks = legacyTasks->affectedRowCount();
                                }
                            }
                            result.failedFirmwareTasks += failedTasks;
                            if (auto command = findOnConnection(connection, candidate.id, tenant); command.has_value())
                            {
                                result.commands.push_back(std::move(*command));
                            }
                        }
                        Sql::executeOrThrowVoid(connection, "COMMIT");
                        return result;
                    } catch (...)
                    {
                        try
                        {
                            Sql::executeOrThrowVoid(connection, "ROLLBACK");
                        } catch (...)
                        {
                        }
                        throw;
                    }
                });
    }

    AsynGyanis::Core::Task<std::int64_t> CommandRepository::pendingCount(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [tenant = m_tenant](DatabaseConnection &connection) -> std::int64_t
                {
                    std::vector<DatabaseValue> parameters;
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    // 执行中的命令同样没有结果，积压读数把两种未结束状态一起算进去
                    const auto rows = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM biz_command WHERE status IN ('pending', 'running')" + clause, parameters);
                    return rows->next() ? Sql::integer(*rows, "total") : 0;
                });
    }
} // namespace EdgePulse::Command
