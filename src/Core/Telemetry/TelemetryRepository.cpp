#include "Core/Telemetry/TelemetryRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Telemetry
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        [[nodiscard]] TelemetryPoint readPoint(const DatabaseResult &result)
        {
            TelemetryPoint point;
            point.id                  = Sql::integer(result, "id");
            point.nodeId              = Sql::integer(result, "node_id");
            point.metric              = Sql::text(result, "metric");
            const DatabaseValue value = result.getValue("value");
            if (const auto *number = std::get_if<double>(&value); number != nullptr)
            {
                point.value = *number;
            } else if (const auto *integer = std::get_if<std::int64_t>(&value); integer != nullptr)
            {
                point.value = static_cast<double>(*integer);
            }
            point.unit        = Sql::text(result, "unit");
            point.collectedAt = Sql::dateTimeText(result, "collected_at");
            return point;
        }
    } // namespace

    TelemetryRepository::TelemetryRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant) : m_database(std::move(database)), m_tenant(tenant)
    {
    }

    std::shared_ptr<TelemetryRepository> TelemetryRepository::scoped(const System::TenantScope tenant) const
    {
        return std::make_shared<TelemetryRepository>(m_database, tenant);
    }

    AsynGyanis::Core::Task<bool> TelemetryRepository::insertBatch(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId, std::vector<TelemetrySample> samples)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [nodeId, samples = std::move(samples), tenant = m_tenant](DatabaseConnection &connection) -> bool
                {
                    if (samples.empty())
                    {
                        return false;
                    }
                    // 遥测归属必须与目标节点一致，避免跨租户把数据写进别人的节点
                    std::vector<DatabaseValue> nodeParameters{nodeId};
                    std::string                nodeClause;
                    if (!tenant.allTenants)
                    {
                        nodeClause = " AND tenant_id = ?";
                        nodeParameters.emplace_back(tenant.tenantId);
                    }
                    const auto node = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM biz_node WHERE id = ? AND deleted = 0" + nodeClause, nodeParameters);
                    if (!node->next() || Sql::integer(*node, "total") == 0)
                    {
                        return false;
                    }
                    Sql::executeOrThrowVoid(connection, "BEGIN");
                    try
                    {
                        for (const TelemetrySample &sample: samples)
                        {
                            const std::string                  collectedAt = sample.collectedAt.empty() ? Common::nowDateTimeUtc() : sample.collectedAt;
                            const DatabaseValue                labels      = sample.labelsJson.empty() ? DatabaseValue{std::monostate{}} : DatabaseValue{sample.labelsJson};
                            const std::array<DatabaseValue, 9> parameters{
                                    Common::nextIdentifier(), tenant.tenantId, nodeId, sample.metric, sample.value, sample.unit, labels, collectedAt, Common::nowDateTimeUtc()};
                            Sql::executeOrThrowVoid(
                                    connection,
                                    "INSERT INTO biz_telemetry (id, tenant_id, node_id, metric, value, unit, labels, collected_at, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                    parameters);
                        }
                        Sql::executeOrThrowVoid(connection, "COMMIT");
                        return true;
                    } catch (...)
                    {
                        // 一批遥测要么全落库、要么全不落，避免告警看到半批数据
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

    AsynGyanis::Core::Task<std::vector<TelemetryPoint>> TelemetryRepository::latest(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        co_return co_await m_database->withConnection<std::vector<TelemetryPoint>>(
                loop,
                [nodeId, tenant = m_tenant](DatabaseConnection &connection) -> std::vector<TelemetryPoint>
                {
                    std::vector<DatabaseValue> parameters{nodeId};
                    std::string                tenantInner;
                    std::string                tenantOuter;
                    if (!tenant.allTenants)
                    {
                        tenantInner = " AND tenant_id = ?";
                        tenantOuter = " AND t.tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    parameters.emplace_back(nodeId);
                    if (!tenant.allTenants)
                    {
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto                  rows = Sql::executeOrThrow(connection,
                                                                           "SELECT t.id, t.node_id, t.metric, t.value, t.unit, t.collected_at FROM biz_telemetry t "
                                                                           "JOIN (SELECT metric AS m, MAX(id) AS latest FROM biz_telemetry WHERE node_id = ?" +
                                                                                   tenantInner +
                                                                                   " GROUP BY metric) x "
                                                                                   "ON x.latest = t.id WHERE t.node_id = ?" +
                                                                                   tenantOuter + " ORDER BY t.metric",
                                                                           parameters);
                    std::vector<TelemetryPoint> points;
                    while (rows->next())
                    {
                        points.push_back(readPoint(*rows));
                    }
                    return points;
                });
    }

    AsynGyanis::Core::Task<std::optional<TelemetryFreshness>> TelemetryRepository::newestTimestamps(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        co_return co_await m_database->withConnection<std::optional<TelemetryFreshness>>(
                loop,
                [nodeId, tenant = m_tenant](DatabaseConnection &connection) -> std::optional<TelemetryFreshness>
                {
                    std::vector<DatabaseValue> parameters{nodeId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(
                            connection, "SELECT MAX(created_at) AS created_at, MAX(collected_at) AS collected_at FROM biz_telemetry WHERE node_id = ?" + clause, parameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    TelemetryFreshness freshness;
                    freshness.createdAt   = Sql::dateTimeText(*rows, "created_at");
                    freshness.collectedAt = Sql::dateTimeText(*rows, "collected_at");
                    if (freshness.createdAt.empty() && freshness.collectedAt.empty())
                    {
                        return std::nullopt;
                    }
                    return freshness;
                });
    }

    AsynGyanis::Core::Task<std::optional<TelemetryFreshness>> TelemetryRepository::newestSample(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        co_return co_await m_database->withConnection<std::optional<TelemetryFreshness>>(
                loop,
                [nodeId, tenant = m_tenant](DatabaseConnection &connection) -> std::optional<TelemetryFreshness>
                {
                    std::vector<DatabaseValue> parameters{nodeId};
                    std::string                clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    // 只取一行：接收时间与采集时间必须来自同一条样本，不能用两行各自的最大值拼出新鲜度
                    const auto rows = Sql::executeOrThrow(
                            connection, "SELECT created_at, collected_at FROM biz_telemetry WHERE node_id = ?" + clause + " ORDER BY created_at DESC, id DESC LIMIT 1",
                            parameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    TelemetryFreshness freshness;
                    freshness.createdAt   = Sql::dateTimeText(*rows, "created_at");
                    freshness.collectedAt = Sql::dateTimeText(*rows, "collected_at");
                    return freshness;
                });
    }

    AsynGyanis::Core::Task<std::vector<TelemetryPoint>> TelemetryRepository::series(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId, std::string metric,
                                                                                    std::string from, std::string to, const int limit)
    {
        co_return co_await m_database->withConnection<std::vector<TelemetryPoint>>(loop,
                                                                                   [nodeId, metric = std::move(metric), from = std::move(from), to = std::move(to), limit,
                                                                                    tenant = m_tenant](DatabaseConnection &connection) -> std::vector<TelemetryPoint>
                                                                                   {
                                                                                       std::vector<DatabaseValue> parameters{nodeId, metric, from, to};
                                                                                       std::string                clause;
                                                                                       if (!tenant.allTenants)
                                                                                       {
                                                                                           clause = " AND tenant_id = ?";
                                                                                           parameters.emplace_back(tenant.tenantId);
                                                                                       }
                                                                                       parameters.emplace_back(static_cast<std::int64_t>(limit));
                                                                                       const auto rows = Sql::executeOrThrow(
                                                                                               connection,
                                                                                               "SELECT id, node_id, metric, value, unit, collected_at FROM biz_telemetry "
                                                                                               "WHERE node_id = ? AND metric = ? AND collected_at >= ? AND collected_at <= ?" +
                                                                                                       clause + " ORDER BY collected_at LIMIT ?",
                                                                                               parameters);
                                                                                       std::vector<TelemetryPoint> points;
                                                                                       while (rows->next())
                                                                                       {
                                                                                           points.push_back(readPoint(*rows));
                                                                                       }
                                                                                       return points;
                                                                                   });
    }

    AsynGyanis::Core::Task<bool> TelemetryRepository::prune(AsynGyanis::Core::EventLoop &loop, const int retentionDays)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [retentionDays, tenant = m_tenant](DatabaseConnection &connection) -> bool
                                                            {
                                                                if (retentionDays <= 0)
                                                                {
                                                                    return false;
                                                                }
                                                                const auto                 cutoff = std::chrono::system_clock::now() - std::chrono::hours{24 * retentionDays};
                                                                std::vector<DatabaseValue> parameters{Common::formatDateTimeUtc(cutoff)};
                                                                std::string                clause;
                                                                if (!tenant.allTenants)
                                                                {
                                                                    clause = " AND tenant_id = ?";
                                                                    parameters.emplace_back(tenant.tenantId);
                                                                }
                                                                Sql::executeOrThrowVoid(connection, "DELETE FROM biz_telemetry WHERE collected_at < ?" + clause, parameters);
                                                                return true;
                                                            });
    }
} // namespace EdgePulse::Telemetry
