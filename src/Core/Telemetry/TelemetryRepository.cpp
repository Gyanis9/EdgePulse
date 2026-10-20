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

    TelemetryRepository::TelemetryRepository(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<bool> TelemetryRepository::insertBatch(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId, std::vector<TelemetrySample> samples)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [nodeId, samples = std::move(samples)](DatabaseConnection &connection) -> bool
                {
                    if (samples.empty())
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
                            const std::array<DatabaseValue, 8> parameters{Common::nextIdentifier(), nodeId, sample.metric, sample.value, sample.unit, labels, collectedAt,
                                                                          Common::nowDateTimeUtc()};
                            Sql::executeOrThrowVoid(
                                    connection, "INSERT INTO biz_telemetry (id, node_id, metric, value, unit, labels, collected_at, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
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
                [nodeId](DatabaseConnection &connection) -> std::vector<TelemetryPoint>
                {
                    const std::array<DatabaseValue, 1> parameters{nodeId};
                    const auto                  rows = Sql::executeOrThrow(connection,
                                                                           "SELECT t.id, t.node_id, t.metric, t.value, t.unit, t.collected_at FROM biz_telemetry t "
                                                                           "JOIN (SELECT metric AS m, MAX(id) AS latest FROM biz_telemetry WHERE node_id = ? GROUP BY metric) x "
                                                                           "ON x.latest = t.id WHERE t.node_id = ? ORDER BY t.metric",
                                                                           std::array<DatabaseValue, 2>{nodeId, nodeId});
                    std::vector<TelemetryPoint> points;
                    while (rows->next())
                    {
                        points.push_back(readPoint(*rows));
                    }
                    return points;
                });
    }

    AsynGyanis::Core::Task<std::vector<TelemetryPoint>> TelemetryRepository::series(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId, std::string metric,
                                                                                    std::string from, std::string to, const int limit)
    {
        co_return co_await m_database->withConnection<std::vector<TelemetryPoint>>(
                loop,
                [nodeId, metric = std::move(metric), from = std::move(from), to = std::move(to), limit](DatabaseConnection &connection) -> std::vector<TelemetryPoint>
                {
                    const std::array<DatabaseValue, 5> parameters{nodeId, metric, from, to, static_cast<std::int64_t>(limit)};
                    const auto rows = Sql::executeOrThrow(connection,
                                                          "SELECT id, node_id, metric, value, unit, collected_at FROM biz_telemetry "
                                                          "WHERE node_id = ? AND metric = ? AND collected_at >= ? AND collected_at <= ? ORDER BY collected_at LIMIT ?",
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
                                                            [retentionDays](DatabaseConnection &connection) -> bool
                                                            {
                                                                if (retentionDays <= 0)
                                                                {
                                                                    return false;
                                                                }
                                                                const auto cutoff = std::chrono::system_clock::now() - std::chrono::hours{24 * retentionDays};
                                                                const std::array<DatabaseValue, 1> parameters{Common::formatDateTimeUtc(cutoff)};
                                                                Sql::executeOrThrowVoid(connection, "DELETE FROM biz_telemetry WHERE collected_at < ?", parameters);
                                                                return true;
                                                            });
    }
} // namespace EdgePulse::Telemetry
