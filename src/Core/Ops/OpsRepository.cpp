#include "Core/Ops/OpsRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/TimeFormat.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Ops
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseValue;
    } // namespace

    OpsRepository::OpsRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant) : m_database(std::move(database)), m_tenant(tenant)
    {
    }

    std::shared_ptr<OpsRepository> OpsRepository::scoped(const System::TenantScope tenant) const
    {
        return std::make_shared<OpsRepository>(m_database, tenant);
    }

    AsynGyanis::Core::Task<OpsSummary> OpsRepository::summary(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await m_database->withConnection<OpsSummary>(
                loop,
                [tenant = m_tenant](DatabaseConnection &connection) -> OpsSummary
                {
                    const std::string          tenantClause = tenant.allTenants ? std::string{} : " AND tenant_id = ?";
                    std::vector<DatabaseValue> nodeParameters;
                    if (!tenant.allTenants)
                    {
                        nodeParameters.emplace_back(tenant.tenantId);
                    }
                    OpsSummary summary;
                    const auto nodes = Sql::executeOrThrow(connection,
                                                           "SELECT COUNT(*) AS total, COALESCE(SUM(CASE WHEN status = 'online' THEN 1 ELSE 0 END), 0) AS online "
                                                           "FROM biz_node WHERE deleted = 0" +
                                                                   tenantClause,
                                                           nodeParameters);
                    if (nodes->next())
                    {
                        summary.nodeTotal  = Sql::integer(*nodes, "total");
                        summary.nodeOnline = Sql::integer(*nodes, "online");
                    }
                    const auto alarms = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM biz_alarm WHERE status <> 'resolved'" + tenantClause, nodeParameters);
                    if (alarms->next())
                    {
                        summary.alarmOpen = Sql::integer(*alarms, "total");
                    }
                    std::vector<DatabaseValue> sinceParameters{Common::formatDateTimeUtc(std::chrono::system_clock::now() - std::chrono::hours{24})};
                    if (!tenant.allTenants)
                    {
                        sinceParameters.emplace_back(tenant.tenantId);
                    }
                    const auto telemetry = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM biz_telemetry WHERE collected_at >= ?" + tenantClause, sinceParameters);
                    if (telemetry->next())
                    {
                        summary.telemetry24h = Sql::integer(*telemetry, "total");
                    }
                    const auto commands = Sql::executeOrThrow(connection,
                                                              "SELECT COUNT(*) AS total, COALESCE(SUM(CASE WHEN status IN ('pending', 'running') THEN 1 ELSE 0 END), 0) AS pending "
                                                              "FROM biz_command WHERE 1 = 1" +
                                                                      tenantClause,
                                                              nodeParameters);
                    if (commands->next())
                    {
                        summary.commandTotal   = Sql::integer(*commands, "total");
                        summary.commandPending = Sql::integer(*commands, "pending");
                    }
                    return summary;
                });
    }
} // namespace EdgePulse::Ops
