#include "Core/Ops/OpsRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/TimeFormat.h"

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace EdgePulse::Ops
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseValue;
    } // namespace

    OpsRepository::OpsRepository(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<OpsSummary> OpsRepository::summary(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await m_database->withConnection<OpsSummary>(
                loop,
                [](DatabaseConnection &connection) -> OpsSummary
                {
                    OpsSummary summary;
                    const auto nodes = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total, COALESCE(SUM(CASE WHEN status = 'online' THEN 1 ELSE 0 END), 0) AS online "
                                                                       "FROM biz_node WHERE deleted = 0");
                    if (nodes->next())
                    {
                        summary.nodeTotal  = Sql::integer(*nodes, "total");
                        summary.nodeOnline = Sql::integer(*nodes, "online");
                    }
                    const auto alarms = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM biz_alarm WHERE status <> 'resolved'");
                    if (alarms->next())
                    {
                        summary.alarmOpen = Sql::integer(*alarms, "total");
                    }
                    const std::array<DatabaseValue, 1> since{Common::formatDateTimeUtc(std::chrono::system_clock::now() - std::chrono::hours{24})};
                    const auto                         telemetry = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM biz_telemetry WHERE collected_at >= ?", since);
                    if (telemetry->next())
                    {
                        summary.telemetry24h = Sql::integer(*telemetry, "total");
                    }
                    const auto commands =
                            Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total, COALESCE(SUM(CASE WHEN status IN ('pending', 'running') THEN 1 ELSE 0 END), 0) AS pending "
                                                            "FROM biz_command");
                    if (commands->next())
                    {
                        summary.commandTotal   = Sql::integer(*commands, "total");
                        summary.commandPending = Sql::integer(*commands, "pending");
                    }
                    return summary;
                });
    }
} // namespace EdgePulse::Ops
