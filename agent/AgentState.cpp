#include "AgentState.h"

#include "Base/Database/Database.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Database/SqlRetry.h"
#include "Base/Exception/EdgePulseException.h"
#include "Base/Log/LogMacros.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Platform/FileSystem/FileSystem.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Agent
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        constexpr std::int64_t kTelemetryQueueLimit    = 10000; ///< 遥测队列上限
        constexpr std::int64_t kResultQueueLimit       = 1000;  ///< 命令结果队列上限
        constexpr int          kWriteAttemptCount      = 3;     ///< 锁竞争时的写事务尝试次数
        constexpr int          kTelemetryRetentionDays = 7;     ///< 遥测离线保留天数

        /**
         * @brief 执行一条语句，失败时把驱动原文收进返回值
         * @param connection 状态库连接
         * @param sql SQL 文本
         * @param parameters 绑定参数
         * @return 成功为空；失败返回中文原因
         */
        [[nodiscard]] std::expected<void, std::string> executeStatement(DatabaseConnection &connection, const std::string_view sql,
                                                                        const std::span<const DatabaseValue> parameters = {})
        {
            std::unique_ptr<DatabaseResult> result = parameters.empty() ? connection.execute(sql) : connection.execute(sql, parameters);
            if (result == nullptr)
            {
                std::string reason = connection.lastError();
                if (reason.empty())
                {
                    reason = "数据库未给出失败原因";
                }
                return std::unexpected(std::move(reason));
            }
            return {};
        }

        /**
         * @brief 执行一条查询并把结果集交给调用方
         * @param connection 状态库连接
         * @param sql SQL 文本
         * @param parameters 绑定参数
         * @return 成功返回结果集；失败返回中文原因
         */
        [[nodiscard]] std::expected<std::unique_ptr<DatabaseResult>, std::string> queryStatement(DatabaseConnection &connection, const std::string_view sql,
                                                                                                 const std::span<const DatabaseValue> parameters = {})
        {
            std::unique_ptr<DatabaseResult> result = parameters.empty() ? connection.execute(sql) : connection.execute(sql, parameters);
            if (result == nullptr)
            {
                std::string reason = connection.lastError();
                if (reason.empty())
                {
                    reason = "数据库未给出失败原因";
                }
                return std::unexpected(std::move(reason));
            }
            return result;
        }

        /**
         * @brief 判断失败是否值得重试写事务
         * @param message 驱动错误文本
         * @return true 表示锁竞争或死锁；唯一键冲突不算
         */
        [[nodiscard]] bool isRetryableWriteFailure(const std::string_view message)
        {
            return Sql::isRetryableSqlFailure(message) && !Sql::isUniqueViolation(message);
        }

        /**
         * @brief 在立即取写锁的事务里执行写动作，锁竞争时重试
         * @param connection 状态库连接
         * @param work 事务内的写动作
         * @return 成功为空；失败返回最后一次的中文原因
         */
        [[nodiscard]] std::expected<void, std::string> runWriteTransaction(DatabaseConnection                                                          &connection,
                                                                           const std::function<std::expected<void, std::string>(DatabaseConnection &)> &work)
        {
            for (int attempt = 0; attempt < kWriteAttemptCount; ++attempt)
            {
                const auto begun = executeStatement(connection, "BEGIN IMMEDIATE;");
                if (!begun)
                {
                    if (isRetryableWriteFailure(begun.error()) && attempt + 1 < kWriteAttemptCount)
                    {
                        continue;
                    }
                    return begun;
                }

                const auto written = work(connection);
                if (!written)
                {
                    static_cast<void>(executeStatement(connection, "ROLLBACK;"));
                    if (isRetryableWriteFailure(written.error()) && attempt + 1 < kWriteAttemptCount)
                    {
                        continue;
                    }
                    return written;
                }

                const auto committed = executeStatement(connection, "COMMIT;");
                if (committed)
                {
                    return {};
                }
                static_cast<void>(executeStatement(connection, "ROLLBACK;"));
                if (isRetryableWriteFailure(committed.error()) && attempt + 1 < kWriteAttemptCount)
                {
                    continue;
                }
                return committed;
            }
            return std::unexpected("写事务在锁竞争重试后仍未提交");
        }

        /**
         * @brief 生成 UTC 文本
         * @param moment 秒级时刻
         * @return std::string 形如 2026-10-24T12:00:00Z
         */
        [[nodiscard]] std::string utcText(const std::chrono::sys_seconds moment)
        {
            return std::format("{:%Y-%m-%dT%H:%M:%S}Z", moment);
        }

        /**
         * @brief 取当前 UTC 时刻文本
         * @return std::string 形如 2026-10-24T12:00:00Z
         */
        [[nodiscard]] std::string currentUtcText()
        {
            return utcText(std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        }

        /**
         * @brief 把队列裁到上限，超出部分按入队顺序删除最旧的记录
         * @param connection 状态库连接
         * @param table 队列表名，只允许调用方传入固定常量
         * @param limit 队列上限
         * @param label 写进日志的队列名
         * @return 成功为空；失败返回中文原因
         */
        [[nodiscard]] std::expected<void, std::string> pruneQueue(DatabaseConnection &connection, const std::string_view table, const std::int64_t limit,
                                                                  const std::string_view label)
        {
            const std::string countSql = "SELECT COUNT(*) AS total FROM " + std::string{table};
            const auto        counted  = queryStatement(connection, countSql);
            if (!counted)
            {
                return std::unexpected(counted.error());
            }
            std::int64_t total = 0;
            if ((*counted)->next())
            {
                total = Sql::integer(**counted, "total");
            }
            if (total <= limit)
            {
                return {};
            }

            const std::int64_t                 excess    = total - limit;
            const std::string                  deleteSql = "DELETE FROM " + std::string{table} + " WHERE id IN (SELECT id FROM " + std::string{table} + " ORDER BY id ASC LIMIT ?)";
            const std::array<DatabaseValue, 1> parameters{excess};
            const auto                         deleted = executeStatement(connection, deleteSql, parameters);
            if (!deleted)
            {
                return std::unexpected(deleted.error());
            }
            LOG_WARN_FMT("{}队列超过上限 {} 条，已丢弃最旧的 {} 条", label, limit, excess);
            return {};
        }
    } // namespace

    AgentState::AgentState(std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection) : m_connection(std::move(connection)), m_mutex(std::make_unique<std::mutex>())
    {
    }

    AgentState::AgentState(AgentState &&other) noexcept = default;

    AgentState &AgentState::operator=(AgentState &&other) noexcept = default;

    AgentState::~AgentState() = default;

    std::expected<AgentState, std::string> AgentState::open(const std::filesystem::path &databasePath)
    {
        try
        {
            const std::string                   databaseFilePath = AsynGyanis::Platform::FileSystem::utf8FromPath(databasePath);
            std::unique_ptr<DatabaseConnection> connection       = Common::Database::openDirectSqlite(databaseFilePath);
            AgentState                          state(std::move(connection));
            const auto                          initialized = state.initialize();
            if (!initialized)
            {
                return std::unexpected(initialized.error());
            }
            return state;
        } catch (const std::exception &failure)
        {
            return std::unexpected("打开 Agent 状态库失败：" + AsynGyanis::Platform::FileSystem::utf8FromPath(databasePath) + "：" + failure.what());
        }
    }

    std::expected<void, std::string> AgentState::initialize()
    {
        {
            std::lock_guard lock(*m_mutex);

            // 先设 WAL 与等待窗口：后面的建表与队列写入都按这两项生效
            const auto wal = executeStatement(*m_connection, "PRAGMA journal_mode=WAL;");
            if (!wal)
            {
                return std::unexpected("无法启用状态库 WAL 日志模式：" + wal.error());
            }
            const auto busyTimeout = executeStatement(*m_connection, "PRAGMA busy_timeout=5000;");
            if (!busyTimeout)
            {
                return std::unexpected("无法设置状态库锁等待时间：" + busyTimeout.error());
            }

            const std::array<std::string_view, 6> schema{
                    "CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, value TEXT NOT NULL);",
                    "CREATE TABLE IF NOT EXISTS telemetry_queue (id INTEGER PRIMARY KEY, payload TEXT NOT NULL, created_at TEXT NOT NULL);",
                    "CREATE TABLE IF NOT EXISTS result_queue (id INTEGER PRIMARY KEY, command_id INTEGER NOT NULL, payload TEXT NOT NULL, created_at TEXT NOT NULL);",
                    "CREATE UNIQUE INDEX IF NOT EXISTS idx_result_queue_command_id ON result_queue(command_id);",
                    "CREATE TABLE IF NOT EXISTS update_state (id INTEGER PRIMARY KEY CHECK (id = 1), version TEXT NOT NULL DEFAULT '', previous_binary TEXT NOT NULL DEFAULT '', "
                    "healthy_at TEXT NOT NULL DEFAULT '', pending_result TEXT NOT NULL DEFAULT '');",
                    "INSERT OR IGNORE INTO update_state (id) VALUES (1);",
            };
            const auto created = runWriteTransaction(*m_connection,
                                                     [&schema](DatabaseConnection &connection) -> std::expected<void, std::string>
                                                     {
                                                         for (const std::string_view statement: schema)
                                                         {
                                                             const auto executed = executeStatement(connection, statement);
                                                             if (!executed)
                                                             {
                                                                 return std::unexpected("创建状态库结构失败：" + executed.error());
                                                             }
                                                         }
                                                         return {};
                                                     });
            if (!created)
            {
                return std::unexpected(created.error());
            }
        }

        return cleanupTelemetry();
    }

    std::expected<void, std::string> AgentState::cleanupTelemetry()
    {
        std::lock_guard                    lock(*m_mutex);
        const auto                         cutoff = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) - std::chrono::days{kTelemetryRetentionDays};
        const std::array<DatabaseValue, 1> parameters{utcText(cutoff)};
        const auto                         cleaned = runWriteTransaction(*m_connection, [&parameters](DatabaseConnection &connection) -> std::expected<void, std::string>
                                                                         { return executeStatement(connection, "DELETE FROM telemetry_queue WHERE created_at < ?", parameters); });
        if (!cleaned)
        {
            return std::unexpected("清理过期遥测失败：" + cleaned.error());
        }
        return {};
    }

    std::optional<std::string> AgentState::get(const std::string_view key)
    {
        std::lock_guard                    lock(*m_mutex);
        const std::array<DatabaseValue, 1> parameters{std::string{key}};
        const auto                         rows = queryStatement(*m_connection, "SELECT value FROM state WHERE key = ?", parameters);
        if (!rows)
        {
            LOG_ERROR_FMT("读取状态键 {} 失败：{}", key, rows.error());
            return std::nullopt;
        }
        if (!(*rows)->next())
        {
            return std::nullopt;
        }
        return Sql::text(**rows, "value");
    }

    void AgentState::set(const std::string_view key, const std::string_view value)
    {
        std::lock_guard                    lock(*m_mutex);
        const std::array<DatabaseValue, 2> parameters{std::string{key}, std::string{value}};
        const auto                         written = runWriteTransaction(
                *m_connection, [&parameters](DatabaseConnection &connection) -> std::expected<void, std::string>
                { return executeStatement(connection, "INSERT INTO state (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value", parameters); });
        if (!written)
        {
            LOG_ERROR_FMT("写入状态键 {} 失败：{}", key, written.error());
        }
    }

    void AgentState::enqueueTelemetry(std::string payload)
    {
        std::lock_guard                    lock(*m_mutex);
        const std::array<DatabaseValue, 2> parameters{std::move(payload), currentUtcText()};
        const auto                         appended = runWriteTransaction(*m_connection,
                                                                          [&parameters](DatabaseConnection &connection) -> std::expected<void, std::string>
                                                                          {
                                                      const auto inserted =
                                                              executeStatement(connection, "INSERT INTO telemetry_queue (payload, created_at) VALUES (?, ?)", parameters);
                                                      if (!inserted)
                                                      {
                                                          return inserted;
                                                      }
                                                      return pruneQueue(connection, "telemetry_queue", kTelemetryQueueLimit, "遥测");
                                                                          });
        if (!appended)
        {
            LOG_ERROR_FMT("遥测入队失败：{}", appended.error());
        }
    }

    std::vector<QueuedItem> AgentState::takeTelemetry(const std::size_t limit)
    {
        const std::size_t boundedLimit = std::min<std::size_t>(limit, static_cast<std::size_t>(kTelemetryQueueLimit));
        if (boundedLimit == 0)
        {
            return {};
        }

        std::lock_guard                    lock(*m_mutex);
        const std::array<DatabaseValue, 1> parameters{static_cast<std::int64_t>(boundedLimit)};
        const auto                         rows = queryStatement(*m_connection, "SELECT id, payload, created_at FROM telemetry_queue ORDER BY id ASC LIMIT ?", parameters);
        if (!rows)
        {
            LOG_ERROR_FMT("读取遥测队列失败：{}", rows.error());
            return {};
        }

        std::vector<QueuedItem> items;
        while ((*rows)->next())
        {
            items.push_back(QueuedItem{Sql::integer(**rows, "id"), Sql::text(**rows, "payload"), Sql::text(**rows, "created_at")});
        }
        return items;
    }

    void AgentState::ackTelemetry(const std::int64_t id)
    {
        std::lock_guard                    lock(*m_mutex);
        const std::array<DatabaseValue, 1> parameters{id};
        const auto                         deleted = runWriteTransaction(*m_connection, [&parameters](DatabaseConnection &connection) -> std::expected<void, std::string>
                                                                         { return executeStatement(connection, "DELETE FROM telemetry_queue WHERE id = ?", parameters); });
        if (!deleted)
        {
            LOG_ERROR_FMT("确认遥测 {} 失败：{}", id, deleted.error());
        }
    }

    void AgentState::enqueueResult(const std::int64_t commandId, std::string payload)
    {
        std::lock_guard                    lock(*m_mutex);
        const std::array<DatabaseValue, 3> parameters{commandId, std::move(payload), currentUtcText()};
        const auto                         appended = runWriteTransaction(*m_connection,
                                                                          [&parameters, commandId](DatabaseConnection &connection) -> std::expected<void, std::string>
                                                                          {
                                                      const auto inserted = executeStatement(
                                                              connection, "INSERT INTO result_queue (command_id, payload, created_at) VALUES (?, ?, ?)", parameters);
                                                      if (!inserted)
                                                      {
                                                          if (Sql::isUniqueViolation(inserted.error()))
                                                          {
                                                              LOG_WARN_FMT("命令 {} 的结果已在队列中，忽略重复入队", commandId);
                                                              return {};
                                                          }
                                                          return inserted;
                                                      }
                                                      return pruneQueue(connection, "result_queue", kResultQueueLimit, "命令结果");
                                                                          });
        if (!appended)
        {
            LOG_ERROR_FMT("命令 {} 结果入队失败：{}", commandId, appended.error());
        }
    }

    bool AgentState::hasResult(const std::int64_t commandId)
    {
        std::lock_guard                    lock(*m_mutex);
        const std::array<DatabaseValue, 1> parameters{commandId};
        const auto                         rows = queryStatement(*m_connection, "SELECT 1 FROM result_queue WHERE command_id = ? LIMIT 1", parameters);
        if (!rows)
        {
            LOG_ERROR_FMT("查询命令 {} 是否有结果失败：{}；按已有结果处理", commandId, rows.error());
            return true;
        }
        return (*rows)->next();
    }

    std::vector<QueuedResult> AgentState::takeResults(const std::size_t limit)
    {
        const std::size_t boundedLimit = std::min<std::size_t>(limit, static_cast<std::size_t>(kResultQueueLimit));
        if (boundedLimit == 0)
        {
            return {};
        }

        std::lock_guard                    lock(*m_mutex);
        const std::array<DatabaseValue, 1> parameters{static_cast<std::int64_t>(boundedLimit)};
        const auto                         rows = queryStatement(*m_connection, "SELECT id, command_id, payload, created_at FROM result_queue ORDER BY id ASC LIMIT ?", parameters);
        if (!rows)
        {
            LOG_ERROR_FMT("读取命令结果队列失败：{}", rows.error());
            return {};
        }

        std::vector<QueuedResult> results;
        while ((*rows)->next())
        {
            results.push_back(QueuedResult{Sql::integer(**rows, "id"), Sql::integer(**rows, "command_id"), Sql::text(**rows, "payload"), Sql::text(**rows, "created_at")});
        }
        return results;
    }

    void AgentState::ackResult(const std::int64_t id)
    {
        std::lock_guard                    lock(*m_mutex);
        const std::array<DatabaseValue, 1> parameters{id};
        const auto                         deleted = runWriteTransaction(*m_connection, [&parameters](DatabaseConnection &connection) -> std::expected<void, std::string>
                                                                         { return executeStatement(connection, "DELETE FROM result_queue WHERE id = ?", parameters); });
        if (!deleted)
        {
            LOG_ERROR_FMT("确认命令结果 {} 失败：{}", id, deleted.error());
        }
    }

    std::optional<std::string> AgentState::pendingUpdateResult()
    {
        std::lock_guard lock(*m_mutex);
        const auto      rows = queryStatement(*m_connection, "SELECT pending_result FROM update_state WHERE id = 1");
        if (!rows)
        {
            LOG_ERROR_FMT("读取升级待上报结果失败：{}", rows.error());
            return std::nullopt;
        }
        if (!(*rows)->next())
        {
            return std::nullopt;
        }
        std::string text = Sql::text(**rows, "pending_result");
        if (text.empty())
        {
            return std::nullopt;
        }
        return text;
    }

    void AgentState::setPendingUpdateResult(std::string text)
    {
        std::lock_guard                    lock(*m_mutex);
        const std::array<DatabaseValue, 1> parameters{std::move(text)};
        const auto                         written = runWriteTransaction(
                *m_connection,
                [&parameters](DatabaseConnection &connection) -> std::expected<void, std::string>
                {
                    return executeStatement(connection,
                                            "INSERT INTO update_state (id, pending_result) VALUES (1, ?) ON CONFLICT(id) DO UPDATE SET pending_result = excluded.pending_result",
                                            parameters);
                });
        if (!written)
        {
            LOG_ERROR_FMT("写入升级待上报结果失败：{}", written.error());
        }
    }

    std::size_t AgentState::telemetryQueueDepth()
    {
        std::lock_guard lock(*m_mutex);
        const auto      rows = queryStatement(*m_connection, "SELECT COUNT(*) AS total FROM telemetry_queue");
        if (!rows)
        {
            LOG_ERROR_FMT("读取遥测队列深度失败：{}", rows.error());
            return 0;
        }
        return (*rows)->next() ? static_cast<std::size_t>(Sql::integer(**rows, "total")) : 0;
    }

    std::size_t AgentState::resultQueueDepth()
    {
        std::lock_guard lock(*m_mutex);
        const auto      rows = queryStatement(*m_connection, "SELECT COUNT(*) AS total FROM result_queue");
        if (!rows)
        {
            LOG_ERROR_FMT("读取命令结果队列深度失败：{}", rows.error());
            return 0;
        }
        return (*rows)->next() ? static_cast<std::size_t>(Sql::integer(**rows, "total")) : 0;
    }
} // namespace EdgePulse::Agent
