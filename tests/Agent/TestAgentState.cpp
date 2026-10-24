#include "AgentState.h"

#include "Base/Database/Database.h"
#include "Base/Database/SqlHelpers.h"
#include "TestSupport/TemporaryDirectory.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace EdgePulse::Agent
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseValue;

        /**
         * @brief 生成与状态库一致的 UTC 文本
         * @return std::string 形如 2026-10-24T12:00:00Z
         */
        [[nodiscard]] std::string utcNow()
        {
            return std::format("{:%Y-%m-%dT%H:%M:%S}Z", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        }

        /**
         * @brief 绕过 AgentState 打开同一条状态库，用于直接检查结构与造种子数据
         * @param path 状态库路径
         * @return std::unique_ptr<DatabaseConnection> 已连接的原生连接
         */
        [[nodiscard]] std::unique_ptr<DatabaseConnection> openRaw(const std::filesystem::path &path)
        {
            return Common::Database::openDirectSqlite(path.string());
        }

        /**
         * @brief 执行一条写语句并对失败立即报错
         * @param connection 原生连接
         * @param sql SQL 文本
         * @param parameters 绑定参数
         */
        void executeOrFail(DatabaseConnection &connection, const std::string_view sql, const std::vector<DatabaseValue> &parameters = {})
        {
            ASSERT_NE(nullptr, connection.execute(sql, parameters)) << connection.lastError();
        }

        /**
         * @brief 插入一条遥测种子
         * @param connection 原生连接
         * @param payload 正文
         * @param createdAt 入队时刻
         */
        void insertTelemetrySeed(DatabaseConnection &connection, const std::string &payload, const std::string &createdAt)
        {
            executeOrFail(connection, "INSERT INTO telemetry_queue(payload, created_at) VALUES(?, ?)", {payload, createdAt});
        }
    } // namespace

    TEST(AgentState, OpenCreatesSchemaAndPersistsKeys)
    {
        TestSupport::TemporaryDirectory directory;
        const std::filesystem::path     databasePath = directory.path() / "state" / "agent.db";

        {
            auto state = AgentState::open(databasePath);
            ASSERT_TRUE(state.has_value()) << state.error();
            EXPECT_FALSE(state->get("node_id").has_value());
            state->set("node_id", "42");
            state->set("agent_version", "1.0.0");
            state->set("node_id", "43");
        }

        {
            auto state = AgentState::open(databasePath);
            ASSERT_TRUE(state.has_value()) << state.error();
            EXPECT_EQ("43", state->get("node_id").value_or(""));
            EXPECT_EQ("1.0.0", state->get("agent_version").value_or(""));
        }

        const std::unique_ptr<DatabaseConnection> connection = openRaw(databasePath);
        const auto                                tables = connection->execute("SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
        ASSERT_NE(nullptr, tables) << connection->lastError();
        std::vector<std::string> tableNames;
        while (tables->next())
        {
            tableNames.push_back(Sql::text(*tables, "name"));
        }
        EXPECT_EQ((std::vector<std::string>{"result_queue", "state", "telemetry_queue", "update_state"}), tableNames);

        const auto index = connection->execute("SELECT sql FROM sqlite_master WHERE type = 'index' AND name = 'idx_result_queue_command_id'");
        ASSERT_NE(nullptr, index) << connection->lastError();
        ASSERT_TRUE(index->next());
        EXPECT_NE(std::string::npos, Sql::text(*index, "sql").find("UNIQUE"));
    }

    TEST(AgentState, TelemetryQueueKeepsFifoOrderUntilAcknowledged)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = AgentState::open(directory.path() / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();

        state->enqueueTelemetry("first");
        state->enqueueTelemetry("second");
        state->enqueueTelemetry("third");

        const std::vector<QueuedItem> firstBatch = state->takeTelemetry(2);
        ASSERT_EQ(2U, firstBatch.size());
        EXPECT_EQ("first", firstBatch[0].payload);
        EXPECT_EQ("second", firstBatch[1].payload);
        EXPECT_LT(firstBatch[0].id, firstBatch[1].id);

        state->ackTelemetry(firstBatch[0].id);
        const std::vector<QueuedItem> secondBatch = state->takeTelemetry(10);
        ASSERT_EQ(2U, secondBatch.size());
        EXPECT_EQ("second", secondBatch[0].payload);
        EXPECT_EQ("third", secondBatch[1].payload);

        state->ackTelemetry(secondBatch[0].id);
        state->ackTelemetry(secondBatch[1].id);
        EXPECT_TRUE(state->takeTelemetry(10).empty());
    }

    TEST(AgentState, AckTelemetryBatchDeletesGivenRowsAndKeepsFifo)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = AgentState::open(directory.path() / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();

        state->enqueueTelemetry("first");
        state->enqueueTelemetry("second");
        state->enqueueTelemetry("third");
        state->enqueueTelemetry("fourth");

        const std::vector<QueuedItem> rows = state->takeTelemetry(10);
        ASSERT_EQ(4U, rows.size());

        const auto emptied = state->ackTelemetryBatch({});
        EXPECT_TRUE(emptied.has_value()) << emptied.error();
        EXPECT_EQ(4U, state->telemetryQueueDepth()) << "空列表必须直接成功且不动队列";

        const auto deleted = state->ackTelemetryBatch({rows[0].id, rows[2].id});
        ASSERT_TRUE(deleted.has_value()) << deleted.error();
        EXPECT_EQ(2U, state->telemetryQueueDepth());

        const std::vector<QueuedItem> remaining = state->takeTelemetry(10);
        ASSERT_EQ(2U, remaining.size());
        EXPECT_EQ("second", remaining[0].payload);
        EXPECT_EQ("fourth", remaining[1].payload);

        const std::vector<std::int64_t> tooMany(501, 1);
        const auto                      rejected = state->ackTelemetryBatch(tooMany);
        EXPECT_FALSE(rejected.has_value()) << "超过单批上限必须拒绝，不能拼出一条超长语句";
        EXPECT_EQ(2U, state->telemetryQueueDepth());
    }

    TEST(AgentState, TelemetryQueueDropsOldestAtLimit)
    {
        TestSupport::TemporaryDirectory directory;
        const std::filesystem::path     databasePath = directory.path() / "agent.db";
        {
            auto state = AgentState::open(databasePath);
            ASSERT_TRUE(state.has_value()) << state.error();
        }

        const std::string createdAt = utcNow();
        {
            const std::unique_ptr<DatabaseConnection> connection = openRaw(databasePath);
            executeOrFail(*connection, "BEGIN IMMEDIATE");
            for (int index = 1; index <= 10000; ++index)
            {
                insertTelemetrySeed(*connection, "seed-" + std::to_string(index), createdAt);
            }
            executeOrFail(*connection, "COMMIT");
        }

        auto state = AgentState::open(databasePath);
        ASSERT_TRUE(state.has_value()) << state.error();
        state->enqueueTelemetry("fresh");

        const std::vector<QueuedItem> items = state->takeTelemetry(20000);
        ASSERT_EQ(10000U, items.size());
        EXPECT_EQ("seed-2", items.front().payload);
        EXPECT_EQ("fresh", items.back().payload);
    }

    TEST(AgentState, ResultQueueRejectsDuplicateCommandAndDropsOldestAtLimit)
    {
        TestSupport::TemporaryDirectory directory;
        const std::filesystem::path     databasePath = directory.path() / "agent.db";
        {
            auto state = AgentState::open(databasePath);
            ASSERT_TRUE(state.has_value()) << state.error();

            state->enqueueResult(7, "first-result");
            state->enqueueResult(7, "second-result");
            const std::vector<QueuedResult> duplicates = state->takeResults(10);
            ASSERT_EQ(1U, duplicates.size());
            EXPECT_EQ(7, duplicates[0].commandId);
            EXPECT_EQ("first-result", duplicates[0].payload);
            state->ackResult(duplicates[0].id);
        }

        const std::string createdAt = utcNow();
        {
            const std::unique_ptr<DatabaseConnection> connection = openRaw(databasePath);
            executeOrFail(*connection, "BEGIN IMMEDIATE");
            for (int index = 1; index <= 1000; ++index)
            {
                executeOrFail(*connection, "INSERT INTO result_queue(command_id, payload, created_at) VALUES(?, ?, ?)",
                              {static_cast<std::int64_t>(index), "seed-" + std::to_string(index), createdAt});
            }
            executeOrFail(*connection, "COMMIT");

            const auto duplicate = connection->execute("INSERT INTO result_queue(command_id, payload, created_at) VALUES(?, ?, ?)",
                                                       std::vector<DatabaseValue>{static_cast<std::int64_t>(1), "duplicate", createdAt});
            EXPECT_EQ(nullptr, duplicate);
        }

        auto state = AgentState::open(databasePath);
        ASSERT_TRUE(state.has_value()) << state.error();
        state->enqueueResult(5000, "fresh-result");

        const std::vector<QueuedResult> results = state->takeResults(5000);
        ASSERT_EQ(1000U, results.size());
        EXPECT_EQ(2, results.front().commandId);
        EXPECT_EQ(5000, results.back().commandId);
    }

    TEST(AgentState, OpenRemovesTelemetryOlderThanRetention)
    {
        TestSupport::TemporaryDirectory directory;
        const std::filesystem::path     databasePath = directory.path() / "agent.db";
        {
            auto state = AgentState::open(databasePath);
            ASSERT_TRUE(state.has_value()) << state.error();
        }

        {
            const std::unique_ptr<DatabaseConnection> connection = openRaw(databasePath);
            insertTelemetrySeed(*connection, "stale", "2020-01-01T00:00:00Z");
            insertTelemetrySeed(*connection, "recent", utcNow());
        }

        auto state = AgentState::open(databasePath);
        ASSERT_TRUE(state.has_value()) << state.error();
        const std::vector<QueuedItem> items = state->takeTelemetry(10);
        ASSERT_EQ(1U, items.size());
        EXPECT_EQ("recent", items[0].payload);
    }

    TEST(AgentState, PendingUpdateResultRoundTrips)
    {
        TestSupport::TemporaryDirectory directory;
        const std::filesystem::path     databasePath = directory.path() / "agent.db";
        {
            auto state = AgentState::open(databasePath);
            ASSERT_TRUE(state.has_value()) << state.error();
            EXPECT_FALSE(state->pendingUpdateResult().has_value());
            state->setPendingUpdateResult(R"({"version":"2.0.0","status":"healthy"})");
            EXPECT_EQ(R"({"version":"2.0.0","status":"healthy"})", state->pendingUpdateResult().value_or(""));
        }

        auto reopened = AgentState::open(databasePath);
        ASSERT_TRUE(reopened.has_value()) << reopened.error();
        EXPECT_EQ(R"({"version":"2.0.0","status":"healthy"})", reopened->pendingUpdateResult().value_or(""));
    }

    TEST(AgentState, HasResultSeesQueuedResults)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = AgentState::open(directory.path() / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();

        EXPECT_FALSE(state->hasResult(7));
        state->enqueueResult(7, R"({"status":"succeeded","result":"{}","message":"done"})");
        EXPECT_TRUE(state->hasResult(7));
        EXPECT_FALSE(state->hasResult(8));

        const std::vector<QueuedResult> rows = state->takeResults(10);
        ASSERT_EQ(1U, rows.size());
        state->ackResult(rows[0].id);
        EXPECT_FALSE(state->hasResult(7));
    }

    TEST(AgentState, OpenReturnsChineseReasonWhenPathIsDirectory)
    {
        TestSupport::TemporaryDirectory directory;
        const std::filesystem::path     databasePath = directory.path() / "blocked.db";
        std::filesystem::create_directories(databasePath);

        auto state = AgentState::open(databasePath);
        ASSERT_FALSE(state.has_value());
        EXPECT_NE(std::string::npos, state.error().find("状态库"));
    }
} // namespace EdgePulse::Agent
