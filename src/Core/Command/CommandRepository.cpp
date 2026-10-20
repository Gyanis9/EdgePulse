#include "Core/Command/CommandRepository.h"

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

namespace EdgePulse::Command
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        constexpr std::string_view kSelectCommand =
                "SELECT c.id, c.node_id, c.type, c.payload, c.status, c.result, c.message, c.created_by, c.created_at, c.dispatched_at, c.finished_at, c.timeout_seconds, "
                "COALESCE(n.name, '') AS node_name FROM biz_command c LEFT JOIN biz_node n ON n.id = c.node_id ";

        [[nodiscard]] CommandRecord readCommand(const DatabaseResult &result)
        {
            CommandRecord command;
            command.id             = Sql::integer(result, "id");
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
            return command;
        }

        [[nodiscard]] std::optional<CommandRecord> findOnConnection(DatabaseConnection &connection, const std::int64_t commandId)
        {
            const std::array<DatabaseValue, 1> parameters{commandId};
            const auto                         rows = Sql::executeOrThrow(connection, std::string{kSelectCommand} + "WHERE c.id = ?", parameters);
            if (!rows->next())
            {
                return std::nullopt;
            }
            return readCommand(*rows);
        }
    } // namespace

    CommandRepository::CommandRepository(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<std::optional<CommandRecord>> CommandRepository::create(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId, std::string type,
                                                                                   std::string payload, const std::int64_t createdBy, const int timeoutSeconds)
    {
        co_return co_await m_database->withConnection<std::optional<CommandRecord>>(
                loop,
                [nodeId, type = std::move(type), payload = std::move(payload), createdBy, timeoutSeconds](DatabaseConnection &connection) -> std::optional<CommandRecord>
                {
                    const std::array<DatabaseValue, 1> nodeParameter{nodeId};
                    const auto                         node = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM biz_node WHERE id = ? AND deleted = 0", nodeParameter);
                    if (!node->next() || Sql::integer(*node, "total") == 0)
                    {
                        return std::nullopt;
                    }
                    const std::int64_t                 id  = Common::nextIdentifier();
                    const std::string                  now = Common::nowDateTimeUtc();
                    const std::array<DatabaseValue, 9> parameters{
                            id, nodeId, type, payload, std::string{"pending"}, createdBy, now, static_cast<std::int64_t>(timeoutSeconds), std::string{"pending"}};
                    Sql::executeOrThrowVoid(connection,
                                            "INSERT INTO biz_command (id, node_id, type, payload, status, created_by, created_at, timeout_seconds, message) "
                                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                            parameters);
                    return findOnConnection(connection, id);
                });
    }

    AsynGyanis::Core::Task<Common::PageRows<CommandRecord>> CommandRepository::list(AsynGyanis::Core::EventLoop &loop, std::string status, const std::int64_t nodeId,
                                                                                    const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<CommandRecord>>(
                loop,
                [status = std::move(status), nodeId, page](DatabaseConnection &connection) -> Common::PageRows<CommandRecord>
                {
                    std::string                filter = "WHERE 1 = 1";
                    std::vector<DatabaseValue> parameters;
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
        co_return co_await m_database->withConnection<std::optional<CommandRecord>>(loop, [commandId](DatabaseConnection &connection) -> std::optional<CommandRecord>
                                                                                    { return findOnConnection(connection, commandId); });
    }

    AsynGyanis::Core::Task<std::optional<CommandRecord>> CommandRepository::takeNext(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        co_return co_await m_database->withConnection<std::optional<CommandRecord>>(
                loop,
                [nodeId](DatabaseConnection &connection) -> std::optional<CommandRecord>
                {
                    Sql::executeOrThrowVoid(connection, "BEGIN");
                    try
                    {
                        const std::array<DatabaseValue, 1> nodeParameter{nodeId};
                        const auto rows = Sql::executeOrThrow(connection, "SELECT id FROM biz_command WHERE node_id = ? AND status = 'pending' ORDER BY created_at, id LIMIT 1",
                                                              nodeParameter);
                        if (!rows->next())
                        {
                            Sql::executeOrThrowVoid(connection, "COMMIT");
                            return std::nullopt;
                        }
                        const std::int64_t id = Sql::integer(*rows, "id");
                        const auto update     = Sql::executeOrThrow(connection, "UPDATE biz_command SET status = 'running', dispatched_at = ? WHERE id = ? AND status = 'pending'",
                                                                    std::array<DatabaseValue, 2>{Common::nowDateTimeUtc(), id});
                        if (update->affectedRowCount() == 0)
                        {
                            Sql::executeOrThrowVoid(connection, "ROLLBACK");
                            return std::nullopt;
                        }
                        Sql::executeOrThrowVoid(connection, "COMMIT");
                        return findOnConnection(connection, id);
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

    AsynGyanis::Core::Task<bool> CommandRepository::markDispatched(AsynGyanis::Core::EventLoop &loop, const std::int64_t commandId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [commandId](DatabaseConnection &connection) -> bool
                {
                    const auto rows = Sql::executeOrThrow(connection, "UPDATE biz_command SET status = 'running', dispatched_at = ? WHERE id = ? AND status = 'pending'",
                                                          std::array<DatabaseValue, 2>{Common::nowDateTimeUtc(), commandId});
                    return rows->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<std::optional<CommandRecord>> CommandRepository::finish(AsynGyanis::Core::EventLoop &loop, const std::int64_t commandId, std::string status,
                                                                                   std::string result, std::string message)
    {
        co_return co_await m_database->withConnection<std::optional<CommandRecord>>(
                loop,
                [commandId, status = std::move(status), result = std::move(result), message = std::move(message)](DatabaseConnection &connection) -> std::optional<CommandRecord>
                {
                    if (status != "succeeded" && status != "failed" && status != "timeout")
                    {
                        return std::nullopt;
                    }
                    const auto rows = Sql::executeOrThrow(connection,
                                                          "UPDATE biz_command SET status = ?, result = ?, message = ?, finished_at = ? "
                                                          "WHERE id = ? AND status IN ('pending', 'running')",
                                                          std::array<DatabaseValue, 5>{status, result, message, Common::nowDateTimeUtc(), commandId});
                    if (rows->affectedRowCount() == 0)
                    {
                        return std::nullopt;
                    }
                    return findOnConnection(connection, commandId);
                });
    }

    AsynGyanis::Core::Task<std::optional<CommandRecord>> CommandRepository::cancel(AsynGyanis::Core::EventLoop &loop, const std::int64_t commandId)
    {
        co_return co_await m_database->withConnection<std::optional<CommandRecord>>(
                loop,
                [commandId](DatabaseConnection &connection) -> std::optional<CommandRecord>
                {
                    const auto rows = Sql::executeOrThrow(
                            connection, "UPDATE biz_command SET status = 'canceled', message = '已取消', finished_at = ? WHERE id = ? AND status IN ('pending', 'running')",
                            std::array<DatabaseValue, 2>{Common::nowDateTimeUtc(), commandId});
                    if (rows->affectedRowCount() == 0)
                    {
                        return std::nullopt;
                    }
                    return findOnConnection(connection, commandId);
                });
    }
} // namespace EdgePulse::Command
