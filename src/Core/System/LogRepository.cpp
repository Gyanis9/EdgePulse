#include "Core/System/LogRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace EdgePulse::System
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        constexpr std::string_view kSelectOper =
                "SELECT id, title, module, action, method, url, ip, oper_name, dept_name, params, result, error_msg, cost_ms, trace_id, oper_time FROM sys_oper_log ";
        constexpr std::string_view kSelectLogin = "SELECT id, username, ip, status, message, trace_id, login_time, location, browser, os FROM sys_login_log ";

        [[nodiscard]] std::int64_t nowEpoch()
        {
            return static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
        }

        [[nodiscard]] std::tm utcTime(const std::int64_t epoch)
        {
            const std::time_t seconds = static_cast<std::time_t>(epoch);
            std::tm           value{};
#if defined(_WIN32)
            gmtime_s(&value, &seconds);
#else
            gmtime_r(&seconds, &value);
#endif
            return value;
        }

        [[nodiscard]] std::int64_t epochFromUtc(const std::tm &value)
        {
#if defined(_WIN32)
            return static_cast<std::int64_t>(_mkgmtime(const_cast<std::tm *>(&value)));
#else
            return static_cast<std::int64_t>(timegm(const_cast<std::tm *>(&value)));
#endif
        }

        [[nodiscard]] std::string formatDate(const std::int64_t epoch)
        {
            const std::tm        value = utcTime(epoch);
            std::array<char, 16> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%04d-%02d-%02d", value.tm_year + 1900, value.tm_mon + 1, value.tm_mday);
            return buffer.data();
        }

        [[nodiscard]] std::string formatHourLabel(const std::int64_t epoch)
        {
            const std::tm       value = utcTime(epoch);
            std::array<char, 8> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%02d:00", value.tm_hour);
            return buffer.data();
        }

        [[nodiscard]] std::string formatDayLabel(const std::int64_t epoch)
        {
            const std::tm       value = utcTime(epoch);
            std::array<char, 8> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%02d-%02d", value.tm_mon + 1, value.tm_mday);
            return buffer.data();
        }

        [[nodiscard]] std::int64_t parseEpoch(const std::string_view text)
        {
            int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
            if (std::sscanf(std::string{text}.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
            {
                return 0;
            }
            std::tm value{};
            value.tm_year = year - 1900;
            value.tm_mon  = month - 1;
            value.tm_mday = day;
            value.tm_hour = hour;
            value.tm_min  = minute;
            value.tm_sec  = second;
            return epochFromUtc(value);
        }

        [[nodiscard]] std::int64_t dayStart(const std::int64_t epoch)
        {
            std::tm value = utcTime(epoch);
            value.tm_hour = 0;
            value.tm_min  = 0;
            value.tm_sec  = 0;
            return epochFromUtc(value);
        }

        [[nodiscard]] OperLogRecord readOper(const DatabaseResult &result)
        {
            OperLogRecord record;
            record.id       = Sql::integer(result, "id");
            record.title    = Sql::text(result, "title");
            record.module   = Sql::text(result, "module");
            record.action   = Sql::text(result, "action");
            record.method   = Sql::text(result, "method");
            record.url      = Sql::text(result, "url");
            record.ip       = Sql::text(result, "ip");
            record.operName = Sql::text(result, "oper_name");
            record.deptName = Sql::text(result, "dept_name");
            record.params   = Sql::text(result, "params");
            record.result   = static_cast<int>(Sql::integer(result, "result"));
            record.errorMsg = Sql::text(result, "error_msg");
            record.costMs   = Sql::integer(result, "cost_ms");
            record.traceId  = Sql::text(result, "trace_id");
            record.operTime = Sql::dateTimeText(result, "oper_time");
            return record;
        }

        [[nodiscard]] LoginLogRecord readLogin(const DatabaseResult &result)
        {
            LoginLogRecord record;
            record.id        = Sql::integer(result, "id");
            record.username  = Sql::text(result, "username");
            record.ip        = Sql::text(result, "ip");
            record.status    = static_cast<int>(Sql::integer(result, "status"));
            record.message   = Sql::text(result, "message");
            record.traceId   = Sql::text(result, "trace_id");
            record.loginTime = Sql::dateTimeText(result, "login_time");
            record.location  = Sql::text(result, "location");
            record.browser   = Sql::text(result, "browser");
            record.os        = Sql::text(result, "os");
            return record;
        }

        struct RangeSpec
        {
            int          bucketCount{24};
            std::int64_t bucketSeconds{3600};
            std::int64_t startEpoch{0};
            bool         hourly{true};
        };

        [[nodiscard]] RangeSpec rangeSpecOf(const std::string &range)
        {
            RangeSpec spec;
            if (range == "7d")
            {
                spec.bucketCount   = 7;
                spec.bucketSeconds = 86400;
                spec.hourly        = false;
            } else if (range == "30d")
            {
                spec.bucketCount   = 30;
                spec.bucketSeconds = 86400;
                spec.hourly        = false;
            }
            const std::int64_t now = nowEpoch();
            spec.startEpoch        = spec.hourly ? now - (spec.bucketCount - 1) * spec.bucketSeconds : dayStart(now) - (spec.bucketCount - 1) * spec.bucketSeconds;
            return spec;
        }

        [[nodiscard]] std::vector<TimeBucket> makeBuckets(const RangeSpec &spec)
        {
            std::vector<TimeBucket> buckets(static_cast<std::size_t>(spec.bucketCount));
            for (int index = 0; index < spec.bucketCount; ++index)
            {
                const std::int64_t start                       = spec.startEpoch + index * spec.bucketSeconds;
                buckets[static_cast<std::size_t>(index)].label = spec.hourly ? formatHourLabel(start) : formatDayLabel(start);
            }
            return buckets;
        }

        [[nodiscard]] std::size_t bucketIndex(const RangeSpec &spec, const std::int64_t epoch)
        {
            if (epoch < spec.startEpoch)
            {
                return std::numeric_limits<std::size_t>::max();
            }
            const auto index = static_cast<std::size_t>((epoch - spec.startEpoch) / spec.bucketSeconds);
            return index < static_cast<std::size_t>(spec.bucketCount) ? index : std::numeric_limits<std::size_t>::max();
        }

        [[nodiscard]] std::string buildOperFilter(const std::string &keyword, const std::string &result, std::vector<DatabaseValue> &parameters)
        {
            std::string filter = "WHERE 1 = 1";
            if (!keyword.empty())
            {
                const std::string pattern = "%" + keyword + "%";
                filter += " AND (module LIKE ? OR action LIKE ? OR oper_name LIKE ? OR url LIKE ?)";
                parameters.emplace_back(pattern);
                parameters.emplace_back(pattern);
                parameters.emplace_back(pattern);
                parameters.emplace_back(pattern);
            }
            if (result == "0" || result == "1")
            {
                filter += " AND result = ?";
                parameters.emplace_back(static_cast<std::int64_t>(result == "1" ? 1 : 0));
            }
            return filter;
        }

        [[nodiscard]] std::string buildLoginFilter(const std::string &keyword, const std::string &status, std::vector<DatabaseValue> &parameters)
        {
            std::string filter = "WHERE 1 = 1";
            if (!keyword.empty())
            {
                const std::string pattern = "%" + keyword + "%";
                filter += " AND (username LIKE ? OR ip LIKE ?)";
                parameters.emplace_back(pattern);
                parameters.emplace_back(pattern);
            }
            if (status == "0" || status == "1")
            {
                filter += " AND status = ?";
                parameters.emplace_back(static_cast<std::int64_t>(status == "1" ? 1 : 0));
            }
            return filter;
        }
    } // namespace

    LogRepository::LogRepository(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<bool> LogRepository::insertOperLog(AsynGyanis::Core::EventLoop &loop, OperLogRecord record)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [record = std::move(record)](DatabaseConnection &connection) mutable -> bool
                {
                    if (record.id == 0)
                    {
                        record.id = Common::nextIdentifier();
                    }
                    if (record.operTime.empty())
                    {
                        record.operTime = Common::nowDateTimeUtc();
                    }
                    const std::array<DatabaseValue, 15> parameters{record.id,
                                                                   record.title,
                                                                   record.module,
                                                                   record.action,
                                                                   record.method,
                                                                   record.url,
                                                                   record.ip,
                                                                   record.operName,
                                                                   record.deptName,
                                                                   record.params,
                                                                   static_cast<std::int64_t>(record.result),
                                                                   record.errorMsg,
                                                                   record.costMs,
                                                                   record.traceId,
                                                                   record.operTime};
                    Sql::executeOrThrowVoid(connection,
                                            "INSERT INTO sys_oper_log (id, title, module, action, method, url, ip, oper_name, dept_name, params, result, error_msg, cost_ms, "
                                            "trace_id, oper_time) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                            parameters);
                    return true;
                });
    }

    AsynGyanis::Core::Task<Common::PageRows<OperLogRecord>> LogRepository::listOperLogs(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string result,
                                                                                        const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<OperLogRecord>>(
                loop,
                [keyword = std::move(keyword), result = std::move(result), page](DatabaseConnection &connection) -> Common::PageRows<OperLogRecord>
                {
                    std::vector<DatabaseValue>      parameters;
                    const std::string               filter = buildOperFilter(keyword, result, parameters);
                    const auto                      count  = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_oper_log " + filter, parameters);
                    Common::PageRows<OperLogRecord> pageRows;
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
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectOper} + filter + " ORDER BY oper_time DESC, id DESC LIMIT ? OFFSET ?", parameters);
                    while (rows->next())
                    {
                        pageRows.rows.push_back(readOper(*rows));
                    }
                    return pageRows;
                });
    }

    AsynGyanis::Core::Task<std::vector<OperLogRecord>> LogRepository::exportOperLogs(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string result)
    {
        co_return co_await m_database->withConnection<std::vector<OperLogRecord>>(
                loop,
                [keyword = std::move(keyword), result = std::move(result)](DatabaseConnection &connection) -> std::vector<OperLogRecord>
                {
                    std::vector<DatabaseValue> parameters;
                    const std::string          filter = buildOperFilter(keyword, result, parameters);
                    parameters.emplace_back(std::int64_t{10000});
                    const auto                 rows = Sql::executeOrThrow(connection, std::string{kSelectOper} + filter + " ORDER BY oper_time DESC, id DESC LIMIT ?", parameters);
                    std::vector<OperLogRecord> records;
                    while (rows->next())
                    {
                        records.push_back(readOper(*rows));
                    }
                    return records;
                });
    }

    AsynGyanis::Core::Task<bool> LogRepository::removeOperLog(AsynGyanis::Core::EventLoop &loop, const std::int64_t id)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [id](DatabaseConnection &connection) -> bool
                                                            {
                                                                const std::array<DatabaseValue, 1> parameters{id};
                                                                const auto rows = Sql::executeOrThrow(connection, "DELETE FROM sys_oper_log WHERE id = ?", parameters);
                                                                return rows->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<bool> LogRepository::clearOperLogs(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [](DatabaseConnection &connection) -> bool
                                                            {
                                                                Sql::executeOrThrowVoid(connection, "DELETE FROM sys_oper_log");
                                                                return true;
                                                            });
    }

    AsynGyanis::Core::Task<Common::PageRows<LoginLogRecord>> LogRepository::listLoginLogs(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status,
                                                                                          const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<LoginLogRecord>>(
                loop,
                [keyword = std::move(keyword), status = std::move(status), page](DatabaseConnection &connection) -> Common::PageRows<LoginLogRecord>
                {
                    std::vector<DatabaseValue>       parameters;
                    const std::string                filter = buildLoginFilter(keyword, status, parameters);
                    const auto                       count  = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_login_log " + filter, parameters);
                    Common::PageRows<LoginLogRecord> pageRows;
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
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectLogin} + filter + " ORDER BY login_time DESC, id DESC LIMIT ? OFFSET ?", parameters);
                    while (rows->next())
                    {
                        pageRows.rows.push_back(readLogin(*rows));
                    }
                    return pageRows;
                });
    }

    AsynGyanis::Core::Task<std::vector<LoginLogRecord>> LogRepository::exportLoginLogs(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status)
    {
        co_return co_await m_database->withConnection<std::vector<LoginLogRecord>>(
                loop,
                [keyword = std::move(keyword), status = std::move(status)](DatabaseConnection &connection) -> std::vector<LoginLogRecord>
                {
                    std::vector<DatabaseValue> parameters;
                    const std::string          filter = buildLoginFilter(keyword, status, parameters);
                    parameters.emplace_back(std::int64_t{10000});
                    const auto rows = Sql::executeOrThrow(connection, std::string{kSelectLogin} + filter + " ORDER BY login_time DESC, id DESC LIMIT ?", parameters);
                    std::vector<LoginLogRecord> records;
                    while (rows->next())
                    {
                        records.push_back(readLogin(*rows));
                    }
                    return records;
                });
    }

    AsynGyanis::Core::Task<bool> LogRepository::removeLoginLog(AsynGyanis::Core::EventLoop &loop, const std::int64_t id)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [id](DatabaseConnection &connection) -> bool
                                                            {
                                                                const std::array<DatabaseValue, 1> parameters{id};
                                                                const auto rows = Sql::executeOrThrow(connection, "DELETE FROM sys_login_log WHERE id = ?", parameters);
                                                                return rows->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<bool> LogRepository::clearLoginLogs(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [](DatabaseConnection &connection) -> bool
                                                            {
                                                                Sql::executeOrThrowVoid(connection, "DELETE FROM sys_login_log");
                                                                return true;
                                                            });
    }

    AsynGyanis::Core::Task<LoginStats> LogRepository::loginStats(AsynGyanis::Core::EventLoop &loop, std::string range)
    {
        co_return co_await m_database->withConnection<LoginStats>(
                loop,
                [range = std::move(range)](DatabaseConnection &connection) -> LoginStats
                {
                    const RangeSpec spec = rangeSpecOf(range);
                    LoginStats      stats;
                    stats.series = makeBuckets(spec);
                    const std::array<DatabaseValue, 1> parameters{Common::formatDateTimeUtc(std::chrono::system_clock::time_point{std::chrono::seconds{spec.startEpoch}})};
                    const auto                         rows =
                            Sql::executeOrThrow(connection, "SELECT username, ip, status, login_time FROM sys_login_log WHERE login_time >= ? ORDER BY login_time", parameters);
                    std::set<std::string> users;
                    std::set<std::string> ips;
                    while (rows->next())
                    {
                        ++stats.total;
                        const int status = static_cast<int>(Sql::integer(*rows, "status"));
                        if (status != 1)
                        {
                            ++stats.failed;
                        }
                        users.insert(Sql::text(*rows, "username"));
                        ips.insert(Sql::text(*rows, "ip"));
                        const std::size_t index = bucketIndex(spec, parseEpoch(Sql::dateTimeText(*rows, "login_time")));
                        if (index != std::numeric_limits<std::size_t>::max())
                        {
                            ++stats.series[index].total;
                            if (status != 1)
                            {
                                ++stats.series[index].failed;
                            }
                        }
                    }
                    stats.users = static_cast<std::int64_t>(users.size());
                    stats.ips   = static_cast<std::int64_t>(ips.size());
                    return stats;
                });
    }

    AsynGyanis::Core::Task<OperStats> LogRepository::operStats(AsynGyanis::Core::EventLoop &loop, std::string range)
    {
        co_return co_await m_database->withConnection<OperStats>(
                loop,
                [range = std::move(range)](DatabaseConnection &connection) -> OperStats
                {
                    const RangeSpec spec = rangeSpecOf(range);
                    OperStats       stats;
                    stats.series = makeBuckets(spec);
                    const std::array<DatabaseValue, 1> parameters{Common::formatDateTimeUtc(std::chrono::system_clock::time_point{std::chrono::seconds{spec.startEpoch}})};
                    const auto                         rows =
                            Sql::executeOrThrow(connection, "SELECT oper_name, result, cost_ms, oper_time FROM sys_oper_log WHERE oper_time >= ? ORDER BY oper_time", parameters);
                    std::set<std::string>     users;
                    std::vector<std::int64_t> costs;
                    while (rows->next())
                    {
                        ++stats.total;
                        const int status = static_cast<int>(Sql::integer(*rows, "result"));
                        if (status != 1)
                        {
                            ++stats.failed;
                        }
                        users.insert(Sql::text(*rows, "oper_name"));
                        costs.push_back(Sql::integer(*rows, "cost_ms"));
                        const std::size_t index = bucketIndex(spec, parseEpoch(Sql::dateTimeText(*rows, "oper_time")));
                        if (index != std::numeric_limits<std::size_t>::max())
                        {
                            ++stats.series[index].total;
                            if (status != 1)
                            {
                                ++stats.series[index].failed;
                            }
                        }
                    }
                    stats.activeUsers = static_cast<std::int64_t>(users.size());
                    if (!costs.empty())
                    {
                        std::int64_t sum = 0;
                        for (const std::int64_t cost: costs)
                        {
                            sum += cost;
                        }
                        stats.avgCost = static_cast<double>(sum) / static_cast<double>(costs.size());
                        std::sort(costs.begin(), costs.end());
                        const std::size_t index = std::min(costs.size() - 1, (costs.size() * 95) / 100);
                        stats.p95Cost           = static_cast<double>(costs[index]);
                    }
                    return stats;
                });
    }

    AsynGyanis::Core::Task<OperTimeline> LogRepository::operTimeline(AsynGyanis::Core::EventLoop &loop, std::string date)
    {
        co_return co_await m_database->withConnection<OperTimeline>(
                loop,
                [date = std::move(date)](DatabaseConnection &connection) -> OperTimeline
                {
                    const std::int64_t now = nowEpoch();
                    const std::string  day = date.empty() ? formatDate(now) : date;
                    OperTimeline       timeline;
                    timeline.date            = day;
                    const std::int64_t start = parseEpoch(day + " 00:00:00");
                    const std::int64_t end   = start + 86400;
                    if (day == formatDate(now))
                    {
                        timeline.nowSec = now - start;
                    }
                    const std::array<DatabaseValue, 2> parameters{Common::formatDateTimeUtc(std::chrono::system_clock::time_point{std::chrono::seconds{start}}),
                                                                  Common::formatDateTimeUtc(std::chrono::system_clock::time_point{std::chrono::seconds{end}})};
                    const auto                         rows = Sql::executeOrThrow(
                            connection, "SELECT id, module, result, cost_ms, oper_time FROM sys_oper_log WHERE oper_time >= ? AND oper_time < ? ORDER BY oper_time", parameters);
                    struct LaneState
                    {
                        std::int64_t                                  count{0};
                        std::vector<TimelineBlock>                    blocks;
                        std::unordered_map<std::int64_t, std::size_t> blockIndex;
                    };
                    std::map<std::string, LaneState> lanes;
                    while (rows->next())
                    {
                        const std::int64_t epoch  = parseEpoch(Sql::dateTimeText(*rows, "oper_time"));
                        const std::int64_t second = std::max<std::int64_t>(0, epoch - start);
                        const std::int64_t block  = second / 300;
                        const std::string  module = Sql::text(*rows, "module");
                        LaneState         &lane   = lanes[module];
                        ++lane.count;
                        auto [iterator, inserted] = lane.blockIndex.emplace(block, lane.blocks.size());
                        if (inserted)
                        {
                            TimelineBlock value;
                            value.startSec = block * 300;
                            value.endSec   = value.startSec + 300;
                            lane.blocks.push_back(std::move(value));
                        }
                        TimelineBlock &value = lane.blocks[iterator->second];
                        ++value.count;
                        if (Sql::integer(*rows, "result") != 1)
                        {
                            ++value.failed;
                        }
                        value.maxCost = std::max(value.maxCost, Sql::integer(*rows, "cost_ms"));
                        if (value.ids.size() < 50)
                        {
                            value.ids.push_back(Sql::integer(*rows, "id"));
                        }
                    }
                    for (auto &[module, lane]: lanes)
                    {
                        TimelineLane value;
                        value.module = module;
                        value.count  = lane.count;
                        value.blocks = std::move(lane.blocks);
                        std::sort(value.blocks.begin(), value.blocks.end(), [](const TimelineBlock &left, const TimelineBlock &right) { return left.startSec < right.startSec; });
                        timeline.lanes.push_back(std::move(value));
                    }
                    std::sort(timeline.lanes.begin(), timeline.lanes.end(), [](const TimelineLane &left, const TimelineLane &right) { return left.count > right.count; });
                    return timeline;
                });
    }

    AsynGyanis::Core::Task<LoginHeatmap> LogRepository::loginHeatmap(AsynGyanis::Core::EventLoop &loop, const int days)
    {
        co_return co_await m_database->withConnection<LoginHeatmap>(
                loop,
                [days](DatabaseConnection &connection) -> LoginHeatmap
                {
                    LoginHeatmap heatmap;
                    heatmap.days             = days;
                    heatmap.threshold        = 5;
                    const std::int64_t now   = nowEpoch();
                    const std::int64_t since = now - static_cast<std::int64_t>(days) * 86400;
                    heatmap.since            = Common::formatDateTimeUtc(std::chrono::system_clock::time_point{std::chrono::seconds{since}});
                    heatmap.cells.resize(7 * 24);
                    for (int day = 0; day < 7; ++day)
                    {
                        for (int hour = 0; hour < 24; ++hour)
                        {
                            heatmap.cells[static_cast<std::size_t>(day * 24 + hour)].dayOfWeek = day;
                            heatmap.cells[static_cast<std::size_t>(day * 24 + hour)].hour      = hour;
                        }
                    }
                    const std::array<DatabaseValue, 1> parameters{heatmap.since};
                    const auto                         rows =
                            Sql::executeOrThrow(connection, "SELECT username, ip, status, login_time FROM sys_login_log WHERE login_time >= ? ORDER BY login_time", parameters);
                    struct KeyState
                    {
                        std::int64_t          failed{0};
                        std::int64_t          total{0};
                        std::set<std::string> peers;
                        std::string           first;
                        std::string           last;
                    };
                    std::unordered_map<std::string, KeyState> byIp;
                    std::unordered_map<std::string, KeyState> byUser;
                    while (rows->next())
                    {
                        const std::string  username = Sql::text(*rows, "username");
                        const std::string  ip       = Sql::text(*rows, "ip");
                        const bool         failed   = Sql::integer(*rows, "status") != 1;
                        const std::string  time     = Sql::dateTimeText(*rows, "login_time");
                        const std::int64_t epoch    = parseEpoch(time);
                        const std::tm      value    = utcTime(epoch);
                        const int          day      = (value.tm_wday + 6) % 7;
                        HeatCell          &cell     = heatmap.cells[static_cast<std::size_t>(day * 24 + value.tm_hour)];
                        ++cell.total;
                        if (failed)
                        {
                            ++cell.failed;
                        }
                        KeyState &ipState = byIp[ip];
                        ++ipState.total;
                        ipState.failed += failed ? 1 : 0;
                        ipState.peers.insert(username);
                        if (ipState.first.empty())
                        {
                            ipState.first = time;
                        }
                        ipState.last        = time;
                        KeyState &userState = byUser[username];
                        ++userState.total;
                        userState.failed += failed ? 1 : 0;
                        userState.peers.insert(ip);
                        if (userState.first.empty())
                        {
                            userState.first = time;
                        }
                        userState.last = time;
                    }
                    for (const auto &[key, state]: byIp)
                    {
                        if (state.failed >= heatmap.threshold)
                        {
                            heatmap.anomalies.push_back(HeatAnomaly{"ip", key, state.failed, state.total, static_cast<std::int64_t>(state.peers.size()), state.first, state.last});
                        }
                    }
                    for (const auto &[key, state]: byUser)
                    {
                        if (state.failed >= heatmap.threshold)
                        {
                            heatmap.anomalies.push_back(
                                    HeatAnomaly{"user", key, state.failed, state.total, static_cast<std::int64_t>(state.peers.size()), state.first, state.last});
                        }
                    }
                    std::sort(heatmap.anomalies.begin(), heatmap.anomalies.end(), [](const HeatAnomaly &left, const HeatAnomaly &right) { return left.failed > right.failed; });
                    if (heatmap.anomalies.size() > 10)
                    {
                        heatmap.anomalies.resize(10);
                    }
                    return heatmap;
                });
    }

    AsynGyanis::Core::Task<std::vector<TimeBucket>> LogRepository::dailyOper(AsynGyanis::Core::EventLoop &loop, const int days)
    {
        co_return co_await m_database->withConnection<std::vector<TimeBucket>>(
                loop,
                [days](DatabaseConnection &connection) -> std::vector<TimeBucket>
                {
                    const std::int64_t      now   = nowEpoch();
                    const std::int64_t      start = dayStart(now) - static_cast<std::int64_t>(days - 1) * 86400;
                    std::vector<TimeBucket> buckets(static_cast<std::size_t>(days));
                    for (int index = 0; index < days; ++index)
                    {
                        buckets[static_cast<std::size_t>(index)].label = formatDate(start + static_cast<std::int64_t>(index) * 86400);
                    }
                    const std::array<DatabaseValue, 1> parameters{Common::formatDateTimeUtc(std::chrono::system_clock::time_point{std::chrono::seconds{start}})};
                    const auto                         rows = Sql::executeOrThrow(connection, "SELECT result, oper_time FROM sys_oper_log WHERE oper_time >= ?", parameters);
                    while (rows->next())
                    {
                        const std::int64_t epoch = parseEpoch(Sql::dateTimeText(*rows, "oper_time"));
                        const std::int64_t index = (epoch - start) / 86400;
                        if (index < 0 || index >= days)
                        {
                            continue;
                        }
                        TimeBucket &bucket = buckets[static_cast<std::size_t>(index)];
                        ++bucket.total;
                        if (Sql::integer(*rows, "result") != 1)
                        {
                            ++bucket.failed;
                        }
                    }
                    return buckets;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> LogRepository::loginCountBetween(AsynGyanis::Core::EventLoop &loop, std::string from, std::string to)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [from = std::move(from), to = std::move(to)](DatabaseConnection &connection) -> std::int64_t
                {
                    const std::array<DatabaseValue, 2> parameters{from, to};
                    const auto rows = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_login_log WHERE login_time >= ? AND login_time < ?", parameters);
                    return rows->next() ? Sql::integer(*rows, "total") : 0;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> LogRepository::operCountBetween(AsynGyanis::Core::EventLoop &loop, std::string from, std::string to)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [from = std::move(from), to = std::move(to)](DatabaseConnection &connection) -> std::int64_t
                {
                    const std::array<DatabaseValue, 2> parameters{from, to};
                    const auto rows = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_oper_log WHERE oper_time >= ? AND oper_time < ?", parameters);
                    return rows->next() ? Sql::integer(*rows, "total") : 0;
                });
    }

    AsynGyanis::Core::Task<std::int64_t> LogRepository::distinctLoginUsersSince(AsynGyanis::Core::EventLoop &loop, std::string since)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [since = std::move(since)](DatabaseConnection &connection) -> std::int64_t
                {
                    const std::array<DatabaseValue, 1> parameters{since};
                    const auto                         rows =
                            Sql::executeOrThrow(connection, "SELECT COUNT(DISTINCT username) AS total FROM sys_login_log WHERE login_time >= ? AND status = 1", parameters);
                    return rows->next() ? Sql::integer(*rows, "total") : 0;
                });
    }

    AsynGyanis::Core::Task<std::vector<LoginLogRecord>> LogRepository::recentLogins(AsynGyanis::Core::EventLoop &loop, const int limit)
    {
        co_return co_await m_database->withConnection<std::vector<LoginLogRecord>>(
                loop,
                [limit](DatabaseConnection &connection) -> std::vector<LoginLogRecord>
                {
                    const std::array<DatabaseValue, 1> parameters{static_cast<std::int64_t>(limit)};
                    const auto                         rows = Sql::executeOrThrow(connection, std::string{kSelectLogin} + "ORDER BY login_time DESC, id DESC LIMIT ?", parameters);
                    std::vector<LoginLogRecord>        records;
                    while (rows->next())
                    {
                        records.push_back(readLogin(*rows));
                    }
                    return records;
                });
    }
} // namespace EdgePulse::System
