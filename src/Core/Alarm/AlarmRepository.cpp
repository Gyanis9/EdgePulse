#include "Core/Alarm/AlarmRepository.h"

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

namespace EdgePulse::Alarm
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        constexpr std::string_view kSelectRule = "SELECT id, name, node_id, metric, comparison, threshold, level, enabled, cooldown_seconds, created_at FROM biz_alarm_rule ";
        constexpr std::string_view kSelectAlarm =
                "SELECT a.id, a.rule_id, a.node_id, a.level, a.metric, a.value, a.threshold, a.status, a.message, a.triggered_at, a.acknowledged_at, a.resolved_at, "
                "COALESCE(n.name, '') AS node_name FROM biz_alarm a LEFT JOIN biz_node n ON n.id = a.node_id ";

        [[nodiscard]] AlarmRuleRecord readRule(const DatabaseResult &result)
        {
            AlarmRuleRecord rule;
            rule.id                       = Sql::integer(result, "id");
            rule.name                     = Sql::text(result, "name");
            rule.nodeId                   = Sql::integer(result, "node_id");
            rule.metric                   = Sql::text(result, "metric");
            rule.comparison               = Sql::text(result, "comparison");
            const DatabaseValue threshold = result.getValue("threshold");
            if (const auto *number = std::get_if<double>(&threshold); number != nullptr)
            {
                rule.threshold = *number;
            } else if (const auto *integer = std::get_if<std::int64_t>(&threshold); integer != nullptr)
            {
                rule.threshold = static_cast<double>(*integer);
            }
            rule.level           = Sql::text(result, "level");
            rule.enabled         = Sql::boolean(result, "enabled");
            rule.cooldownSeconds = static_cast<int>(Sql::integer(result, "cooldown_seconds"));
            rule.createTime      = Sql::dateTimeText(result, "created_at");
            return rule;
        }

        [[nodiscard]] AlarmRecord readAlarm(const DatabaseResult &result)
        {
            AlarmRecord alarm;
            alarm.id                  = Sql::integer(result, "id");
            alarm.ruleId              = Sql::integer(result, "rule_id");
            alarm.nodeId              = Sql::integer(result, "node_id");
            alarm.nodeName            = Sql::text(result, "node_name");
            alarm.level               = Sql::text(result, "level");
            alarm.metric              = Sql::text(result, "metric");
            const DatabaseValue value = result.getValue("value");
            if (const auto *number = std::get_if<double>(&value); number != nullptr)
            {
                alarm.value = *number;
            }
            const DatabaseValue threshold = result.getValue("threshold");
            if (const auto *number = std::get_if<double>(&threshold); number != nullptr)
            {
                alarm.threshold = *number;
            }
            alarm.status         = Sql::text(result, "status");
            alarm.message        = Sql::text(result, "message");
            alarm.triggeredAt    = Sql::dateTimeText(result, "triggered_at");
            alarm.acknowledgedAt = Sql::dateTimeText(result, "acknowledged_at");
            alarm.resolvedAt     = Sql::dateTimeText(result, "resolved_at");
            return alarm;
        }
    } // namespace

    AlarmRepository::AlarmRepository(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<Common::PageRows<AlarmRuleRecord>> AlarmRepository::listRules(AsynGyanis::Core::EventLoop &loop, std::string keyword, const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<AlarmRuleRecord>>(
                loop,
                [keyword = std::move(keyword), page](DatabaseConnection &connection) -> Common::PageRows<AlarmRuleRecord>
                {
                    std::string                filter = "WHERE deleted = 0";
                    std::vector<DatabaseValue> parameters;
                    if (!keyword.empty())
                    {
                        const std::string pattern = "%" + keyword + "%";
                        filter += " AND (name LIKE ? OR metric LIKE ?)";
                        parameters.emplace_back(pattern);
                        parameters.emplace_back(pattern);
                    }
                    const auto                        count = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM biz_alarm_rule " + filter, parameters);
                    Common::PageRows<AlarmRuleRecord> pageRows;
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
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectRule} + filter + " ORDER BY id LIMIT ? OFFSET ?", parameters);
                    while (rows->next())
                    {
                        pageRows.rows.push_back(readRule(*rows));
                    }
                    return pageRows;
                });
    }

    AsynGyanis::Core::Task<std::vector<AlarmRuleRecord>> AlarmRepository::enabledRules(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await m_database->withConnection<std::vector<AlarmRuleRecord>>(
                loop,
                [](DatabaseConnection &connection) -> std::vector<AlarmRuleRecord>
                {
                    const auto                   rows = Sql::executeOrThrow(connection, std::string{kSelectRule} + "WHERE deleted = 0 AND enabled = 1 ORDER BY id");
                    std::vector<AlarmRuleRecord> rules;
                    while (rows->next())
                    {
                        rules.push_back(readRule(*rows));
                    }
                    return rules;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> AlarmRepository::createRule(AsynGyanis::Core::EventLoop &loop, AlarmRuleRecord rule, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [rule = std::move(rule), operatorId](DatabaseConnection &connection) -> std::int64_t
                {
                    const std::int64_t                  id  = Common::nextIdentifier();
                    const std::string                   now = Common::nowDateTimeUtc();
                    const std::array<DatabaseValue, 13> parameters{id,
                                                                   rule.name,
                                                                   rule.nodeId,
                                                                   rule.metric,
                                                                   rule.comparison,
                                                                   rule.threshold,
                                                                   rule.level,
                                                                   static_cast<std::int64_t>(rule.enabled ? 1 : 0),
                                                                   static_cast<std::int64_t>(rule.cooldownSeconds),
                                                                   now,
                                                                   now,
                                                                   operatorId,
                                                                   std::int64_t{0}};
                    Sql::executeOrThrowVoid(connection,
                                            "INSERT INTO biz_alarm_rule (id, name, node_id, metric, comparison, threshold, level, enabled, cooldown_seconds, created_at, "
                                            "updated_at, created_by, deleted) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                            parameters);
                    return id;
                });
    }

    AsynGyanis::Core::Task<bool> AlarmRepository::updateRule(AsynGyanis::Core::EventLoop &loop, const std::int64_t ruleId, AlarmRuleRecord rule, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [ruleId, rule = std::move(rule), operatorId](DatabaseConnection &connection) -> bool
                {
                    const std::array<DatabaseValue, 11> parameters{rule.name,
                                                                   rule.nodeId,
                                                                   rule.metric,
                                                                   rule.comparison,
                                                                   rule.threshold,
                                                                   rule.level,
                                                                   static_cast<std::int64_t>(rule.enabled ? 1 : 0),
                                                                   static_cast<std::int64_t>(rule.cooldownSeconds),
                                                                   Common::nowDateTimeUtc(),
                                                                   operatorId,
                                                                   ruleId};
                    const auto rows = Sql::executeOrThrow(connection,
                                                          "UPDATE biz_alarm_rule SET name = ?, node_id = ?, metric = ?, comparison = ?, threshold = ?, level = ?, enabled = ?, "
                                                          "cooldown_seconds = ?, updated_at = ?, updated_by = ? WHERE id = ? AND deleted = 0",
                                                          parameters);
                    return rows->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<bool> AlarmRepository::removeRule(AsynGyanis::Core::EventLoop &loop, const std::int64_t ruleId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [ruleId](DatabaseConnection &connection) -> bool
                {
                    const std::array<DatabaseValue, 2> parameters{Common::nowDateTimeUtc(), ruleId};
                    const auto rows = Sql::executeOrThrow(connection, "UPDATE biz_alarm_rule SET deleted = 1, updated_at = ? WHERE id = ? AND deleted = 0", parameters);
                    return rows->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<std::optional<AlarmRuleRecord>> AlarmRepository::findRule(AsynGyanis::Core::EventLoop &loop, const std::int64_t ruleId)
    {
        co_return co_await m_database->withConnection<std::optional<AlarmRuleRecord>>(
                loop,
                [ruleId](DatabaseConnection &connection) -> std::optional<AlarmRuleRecord>
                {
                    const std::array<DatabaseValue, 1> parameters{ruleId};
                    const auto                         rows = Sql::executeOrThrow(connection, std::string{kSelectRule} + "WHERE id = ? AND deleted = 0", parameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    return readRule(*rows);
                });
    }

    AsynGyanis::Core::Task<std::optional<AlarmRecord>> AlarmRepository::findOpen(AsynGyanis::Core::EventLoop &loop, const std::int64_t ruleId, const std::int64_t nodeId)
    {
        co_return co_await m_database->withConnection<std::optional<AlarmRecord>>(
                loop,
                [ruleId, nodeId](DatabaseConnection &connection) -> std::optional<AlarmRecord>
                {
                    const std::array<DatabaseValue, 2> parameters{ruleId, nodeId};
                    const auto                         rows = Sql::executeOrThrow(
                            connection, std::string{kSelectAlarm} + "WHERE a.rule_id = ? AND a.node_id = ? AND a.status <> 'resolved' ORDER BY a.id DESC LIMIT 1", parameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    return readAlarm(*rows);
                });
    }

    AsynGyanis::Core::Task<std::optional<AlarmRecord>> AlarmRepository::insertAlarm(AsynGyanis::Core::EventLoop &loop, AlarmRuleRecord rule, const std::int64_t nodeId,
                                                                                    const double value, std::string message)
    {
        co_return co_await m_database->withConnection<std::optional<AlarmRecord>>(
                loop,
                [rule = std::move(rule), nodeId, value, message = std::move(message)](DatabaseConnection &connection) -> std::optional<AlarmRecord>
                {
                    const std::int64_t                  id  = Common::nextIdentifier();
                    const std::string                   now = Common::nowDateTimeUtc();
                    const std::array<DatabaseValue, 12> parameters{
                            id, rule.id, nodeId, rule.level, rule.metric, value, rule.threshold, std::string{"open"}, message, now, std::string{"open"}, now};
                    Sql::executeOrThrowVoid(connection,
                                            "INSERT INTO biz_alarm (id, rule_id, node_id, level, metric, value, threshold, status, message, triggered_at, dedup_key, created_at) "
                                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                            parameters);
                    const std::array<DatabaseValue, 1> idParameter{id};
                    const auto                         rows = Sql::executeOrThrow(connection, std::string{kSelectAlarm} + "WHERE a.id = ?", idParameter);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    return readAlarm(*rows);
                });
    }

    AsynGyanis::Core::Task<bool> AlarmRepository::recentlyResolved(AsynGyanis::Core::EventLoop &loop, const std::int64_t ruleId, const std::int64_t nodeId,
                                                                   const int cooldownSeconds)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [ruleId, nodeId, cooldownSeconds](DatabaseConnection &connection) -> bool
                {
                    if (cooldownSeconds <= 0)
                    {
                        return false;
                    }
                    const std::string                  cutoff = Common::formatDateTimeUtc(std::chrono::system_clock::now() - std::chrono::seconds{cooldownSeconds});
                    const std::array<DatabaseValue, 3> parameters{ruleId, nodeId, cutoff};
                    const auto                         rows = Sql::executeOrThrow(
                            connection, "SELECT COUNT(*) AS total FROM biz_alarm WHERE rule_id = ? AND node_id = ? AND status = 'resolved' AND resolved_at >= ?", parameters);
                    return rows->next() && Sql::integer(*rows, "total") > 0;
                });
    }

    AsynGyanis::Core::Task<Common::PageRows<AlarmRecord>> AlarmRepository::listAlarms(AsynGyanis::Core::EventLoop &loop, std::string status, std::string level,
                                                                                      const std::int64_t nodeId, const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<AlarmRecord>>(
                loop,
                [status = std::move(status), level = std::move(level), nodeId, page](DatabaseConnection &connection) -> Common::PageRows<AlarmRecord>
                {
                    std::string                filter = "WHERE 1 = 1";
                    std::vector<DatabaseValue> parameters;
                    if (!status.empty())
                    {
                        filter += " AND a.status = ?";
                        parameters.emplace_back(status);
                    }
                    if (!level.empty())
                    {
                        filter += " AND a.level = ?";
                        parameters.emplace_back(level);
                    }
                    if (nodeId > 0)
                    {
                        filter += " AND a.node_id = ?";
                        parameters.emplace_back(nodeId);
                    }
                    const auto                    count = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM biz_alarm a " + filter, parameters);
                    Common::PageRows<AlarmRecord> pageRows;
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
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectAlarm} + filter + " ORDER BY a.triggered_at DESC, a.id DESC LIMIT ? OFFSET ?", parameters);
                    while (rows->next())
                    {
                        pageRows.rows.push_back(readAlarm(*rows));
                    }
                    return pageRows;
                });
    }

    AsynGyanis::Core::Task<std::optional<AlarmRecord>> AlarmRepository::findAlarm(AsynGyanis::Core::EventLoop &loop, const std::int64_t alarmId)
    {
        co_return co_await m_database->withConnection<std::optional<AlarmRecord>>(loop,
                                                                                  [alarmId](DatabaseConnection &connection) -> std::optional<AlarmRecord>
                                                                                  {
                                                                                      const std::array<DatabaseValue, 1> parameters{alarmId};
                                                                                      const auto                         rows = Sql::executeOrThrow(
                                                                                              connection, std::string{kSelectAlarm} + "WHERE a.id = ?", parameters);
                                                                                      if (!rows->next())
                                                                                      {
                                                                                          return std::nullopt;
                                                                                      }
                                                                                      return readAlarm(*rows);
                                                                                  });
    }

    AsynGyanis::Core::Task<std::optional<AlarmRecord>> AlarmRepository::acknowledge(AsynGyanis::Core::EventLoop &loop, const std::int64_t alarmId, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<std::optional<AlarmRecord>>(
                loop,
                [alarmId, operatorId](DatabaseConnection &connection) -> std::optional<AlarmRecord>
                {
                    const std::array<DatabaseValue, 3> parameters{Common::nowDateTimeUtc(), operatorId, alarmId};
                    const auto                         update = Sql::executeOrThrow(connection,
                                                                                    "UPDATE biz_alarm SET status = 'acknowledged', acknowledged_at = ?, acknowledged_by = ? "
                                                                                    "WHERE id = ? AND status = 'open'",
                                                                                    parameters);
                    if (update->affectedRowCount() == 0)
                    {
                        return std::nullopt;
                    }
                    const std::array<DatabaseValue, 1> idParameter{alarmId};
                    const auto                         rows = Sql::executeOrThrow(connection, std::string{kSelectAlarm} + "WHERE a.id = ?", idParameter);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    return readAlarm(*rows);
                });
    }

    AsynGyanis::Core::Task<std::optional<AlarmRecord>> AlarmRepository::resolve(AsynGyanis::Core::EventLoop &loop, const std::int64_t alarmId, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<std::optional<AlarmRecord>>(
                loop,
                [alarmId, operatorId](DatabaseConnection &connection) -> std::optional<AlarmRecord>
                {
                    const std::array<DatabaseValue, 3> parameters{Common::nowDateTimeUtc(), operatorId, alarmId};
                    const auto                         update = Sql::executeOrThrow(connection,
                                                                                    "UPDATE biz_alarm SET status = 'resolved', resolved_at = ?, resolved_by = ? "
                                                                                    "WHERE id = ? AND status <> 'resolved'",
                                                                                    parameters);
                    if (update->affectedRowCount() == 0)
                    {
                        return std::nullopt;
                    }
                    const std::array<DatabaseValue, 1> idParameter{alarmId};
                    const auto                         rows = Sql::executeOrThrow(connection, std::string{kSelectAlarm} + "WHERE a.id = ?", idParameter);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    return readAlarm(*rows);
                });
    }

    AsynGyanis::Core::Task<std::int64_t> AlarmRepository::openCount(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await m_database->withConnection<std::int64_t>(loop,
                                                                    [](DatabaseConnection &connection) -> std::int64_t
                                                                    {
                                                                        const auto rows = Sql::executeOrThrow(connection,
                                                                                                              "SELECT COUNT(*) AS total FROM biz_alarm WHERE status <> 'resolved'");
                                                                        return rows->next() ? Sql::integer(*rows, "total") : 0;
                                                                    });
    }
} // namespace EdgePulse::Alarm
