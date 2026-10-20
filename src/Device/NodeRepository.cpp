#include "Device/NodeRepository.h"

#include "Common/IdGenerator.h"
#include "Common/TimeFormat.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace EdgePulse::Device
{
    namespace
    {
        constexpr const char *kNodeColumns = "id, name, status, agent_version, cert_fingerprint, labels, last_seen, version";

        [[nodiscard]] std::string readText(const AsynGyanis::Database::DatabaseValue &value)
        {
            if (const auto *text = std::get_if<std::string>(&value); text != nullptr)
            {
                return *text;
            }
            return {};
        }

        [[nodiscard]] std::int64_t readInteger(const AsynGyanis::Database::DatabaseValue &value)
        {
            if (const auto *number = std::get_if<std::int64_t>(&value); number != nullptr)
            {
                return *number;
            }
            return 0;
        }

        [[nodiscard]] AsynGyanis::Database::DatabaseValue nullableText(const std::string &text)
        {
            // 空标签按 SQL NULL 落库，读取时才能区分“没有标签”与“标签是空串”
            return text.empty() ? AsynGyanis::Database::DatabaseValue{std::monostate{}} : AsynGyanis::Database::DatabaseValue{text};
        }

        [[nodiscard]] NodeRecord readNode(const AsynGyanis::Database::DatabaseResult &result)
        {
            NodeRecord node;
            node.id              = readInteger(result.getValue("id"));
            node.name            = readText(result.getValue("name"));
            node.status          = readText(result.getValue("status"));
            node.agentVersion    = readText(result.getValue("agent_version"));
            node.certFingerprint = readText(result.getValue("cert_fingerprint"));
            node.labelsJson      = readText(result.getValue("labels"));
            node.lastSeen        = readText(result.getValue("last_seen"));
            node.version         = readInteger(result.getValue("version"));
            return node;
        }

        [[nodiscard]] std::optional<NodeRecord> findByIdOnConnection(AsynGyanis::Database::DatabaseConnection &connection, const std::int64_t nodeId)
        {
            const std::array<AsynGyanis::Database::DatabaseValue, 1>    parameters{nodeId};
            const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result =
                    connection.execute(std::string{"SELECT "} + kNodeColumns + " FROM biz_node WHERE id = ? AND deleted = 0", parameters);
            if (!result)
            {
                throw Common::DatabaseException("查询节点失败：" + connection.lastError());
            }
            if (!result->next())
            {
                return std::nullopt;
            }
            return readNode(*result);
        }

        [[nodiscard]] std::optional<NodeRecord> findByNameOnConnection(AsynGyanis::Database::DatabaseConnection &connection, const std::string &name)
        {
            const std::array<AsynGyanis::Database::DatabaseValue, 1>    parameters{name};
            const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result =
                    connection.execute(std::string{"SELECT "} + kNodeColumns + " FROM biz_node WHERE name = ? AND deleted = 0", parameters);
            if (!result)
            {
                throw Common::DatabaseException("按名称查询节点失败：" + connection.lastError());
            }
            if (!result->next())
            {
                return std::nullopt;
            }
            return readNode(*result);
        }

        /**
         * @brief 写一条新节点；调用方保证名称未占用
         */
        [[nodiscard]] NodeRecord insertNode(AsynGyanis::Database::DatabaseConnection &connection, const std::string &name, const std::string &labelsJson, const std::string &status,
                                            const std::string &agentVersion, const std::string &certFingerprint, const std::string &lastSeen)
        {
            const std::int64_t                                          id  = Common::nextIdentifier();
            const std::string                                           now = Common::nowIso8601Utc();
            std::vector<AsynGyanis::Database::DatabaseValue>            parameters{id,
                                                                                   name,
                                                                                   status,
                                                                                   agentVersion,
                                                                                   certFingerprint,
                                                                                   nullableText(labelsJson),
                                                                                   lastSeen.empty() ? AsynGyanis::Database::DatabaseValue{std::monostate{}}
                                                                                                    : AsynGyanis::Database::DatabaseValue{lastSeen},
                                                                                   now,
                                                                                   now};
            const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(
                    "INSERT INTO biz_node (id, name, status, agent_version, cert_fingerprint, labels, last_seen, version, deleted, created_at, updated_at, created_by, "
                    "updated_by) VALUES (?, ?, ?, ?, ?, ?, ?, 1, 0, ?, ?, 0, 0)",
                    parameters);
            if (!result)
            {
                throw Common::DatabaseException("创建节点失败：" + connection.lastError());
            }
            NodeRecord node;
            node.id              = id;
            node.name            = name;
            node.status          = status;
            node.agentVersion    = agentVersion;
            node.certFingerprint = certFingerprint;
            node.labelsJson      = labelsJson;
            node.lastSeen        = lastSeen;
            node.version         = 1;
            return node;
        }
    } // namespace

    NodeRepository::NodeRepository(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<NodePage> NodeRepository::list(AsynGyanis::Core::EventLoop &loop, Common::PageParameters parameters, std::string keyword, std::string status)
    {
        co_return co_await m_database->withConnection<NodePage>(
                loop,
                [parameters, keyword = std::move(keyword), status = std::move(status)](AsynGyanis::Database::DatabaseConnection &connection) -> NodePage
                {
                    const std::string                                           keywordPattern = "%" + keyword + "%";
                    const std::string                                           filter = " FROM biz_node WHERE deleted = 0 AND (? = '' OR name LIKE ?) AND (? = '' OR status = ?)";
                    std::vector<AsynGyanis::Database::DatabaseValue>            countParameters{keyword, keywordPattern, status, status};
                    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> countResult = connection.execute("SELECT COUNT(*)" + filter, countParameters);
                    if (!countResult)
                    {
                        throw Common::DatabaseException("统计节点失败：" + connection.lastError());
                    }
                    NodePage page;
                    if (countResult->next())
                    {
                        page.total = readInteger(countResult->getValue(0));
                    }

                    std::vector<AsynGyanis::Database::DatabaseValue> rowParameters{
                            keyword, keywordPattern, status, status, static_cast<std::int64_t>(parameters.pageSize), static_cast<std::int64_t>(parameters.offset())};
                    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> rowsResult =
                            connection.execute(std::string{"SELECT "} + kNodeColumns + filter + " ORDER BY id DESC LIMIT ? OFFSET ?", rowParameters);
                    if (!rowsResult)
                    {
                        throw Common::DatabaseException("查询节点列表失败：" + connection.lastError());
                    }
                    while (rowsResult->next())
                    {
                        page.rows.push_back(readNode(*rowsResult));
                    }
                    return page;
                });
    }

    AsynGyanis::Core::Task<std::optional<NodeRecord>> NodeRepository::findById(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        co_return co_await m_database->withConnection<std::optional<NodeRecord>>(loop, [nodeId](AsynGyanis::Database::DatabaseConnection &connection)
                                                                                 { return findByIdOnConnection(connection, nodeId); });
    }

    AsynGyanis::Core::Task<std::optional<NodeRecord>> NodeRepository::findByName(AsynGyanis::Core::EventLoop &loop, std::string name)
    {
        co_return co_await m_database->withConnection<std::optional<NodeRecord>>(loop, [name = std::move(name)](AsynGyanis::Database::DatabaseConnection &connection)
                                                                                 { return findByNameOnConnection(connection, name); });
    }

    AsynGyanis::Core::Task<NodeRecord> NodeRepository::create(AsynGyanis::Core::EventLoop &loop, std::string name, std::string labelsJson)
    {
        co_return co_await m_database->withConnection<NodeRecord>(loop,
                                                                  [name = std::move(name), labelsJson = std::move(labelsJson)](AsynGyanis::Database::DatabaseConnection &connection)
                                                                  { return insertNode(connection, name, labelsJson, "offline", "", "", ""); });
    }

    AsynGyanis::Core::Task<std::expected<NodeRecord, NodeMutationFailure>> NodeRepository::update(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId,
                                                                                                  std::optional<std::string> name, std::optional<std::string> labelsJson,
                                                                                                  const std::int64_t expectedVersion)
    {
        co_return co_await m_database->withConnection<std::expected<NodeRecord, NodeMutationFailure>>(
                loop,
                [nodeId, name = std::move(name), labelsJson = std::move(labelsJson),
                 expectedVersion](AsynGyanis::Database::DatabaseConnection &connection) -> std::expected<NodeRecord, NodeMutationFailure>
                {
                    const std::optional<NodeRecord> current = findByIdOnConnection(connection, nodeId);
                    if (!current.has_value())
                    {
                        return std::unexpected(NodeMutationFailure::NotFound);
                    }
                    const std::string                                           newName   = name.value_or(current->name);
                    const std::string                                           newLabels = labelsJson.value_or(current->labelsJson);
                    const std::string                                           now       = Common::nowIso8601Utc();
                    const std::array<AsynGyanis::Database::DatabaseValue, 5>    parameters{newName, nullableText(newLabels), now, nodeId, expectedVersion};
                    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(
                            "UPDATE biz_node SET name = ?, labels = ?, version = version + 1, updated_at = ? WHERE id = ? AND version = ? AND deleted = 0", parameters);
                    if (!result)
                    {
                        throw Common::DatabaseException("更新节点失败：" + connection.lastError());
                    }
                    // 影响行数为 0：节点在读取之后被删除或版本已变
                    if (result->affectedRowCount() == 0)
                    {
                        return findByIdOnConnection(connection, nodeId).has_value() ? std::unexpected(NodeMutationFailure::Conflict)
                                                                                    : std::unexpected(NodeMutationFailure::NotFound);
                    }
                    NodeRecord updated = *current;
                    updated.name       = newName;
                    updated.labelsJson = newLabels;
                    updated.version    = current->version + 1;
                    return updated;
                });
    }

    AsynGyanis::Core::Task<bool> NodeRepository::remove(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [nodeId](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                                                            {
                                                                const std::array<AsynGyanis::Database::DatabaseValue, 2>    parameters{Common::nowIso8601Utc(), nodeId};
                                                                const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(
                                                                        "UPDATE biz_node SET deleted = 1, updated_at = ? WHERE id = ? AND deleted = 0", parameters);
                                                                if (!result)
                                                                {
                                                                    throw Common::DatabaseException("删除节点失败：" + connection.lastError());
                                                                }
                                                                return result->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<NodeRecord> NodeRepository::upsertAgent(AsynGyanis::Core::EventLoop &loop, AgentUpsert registration)
    {
        co_return co_await m_database->withConnection<NodeRecord>(
                loop,
                [registration = std::move(registration)](AsynGyanis::Database::DatabaseConnection &connection) -> NodeRecord
                {
                    const std::string               now     = Common::nowIso8601Utc();
                    const std::optional<NodeRecord> current = findByNameOnConnection(connection, registration.name);
                    if (!current.has_value())
                    {
                        return insertNode(connection, registration.name, registration.labelsJson, "online", registration.agentVersion, registration.certFingerprint, now);
                    }
                    const std::array<AsynGyanis::Database::DatabaseValue, 6> parameters{
                            registration.agentVersion, registration.certFingerprint, nullableText(registration.labelsJson), now, now, current->id};
                    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(
                            "UPDATE biz_node SET status = 'online', agent_version = ?, cert_fingerprint = ?, labels = ?, last_seen = ?, version = version + 1, updated_at = ? "
                            "WHERE id = ? AND deleted = 0",
                            parameters);
                    if (!result)
                    {
                        throw Common::DatabaseException("注册 Agent 失败：" + connection.lastError());
                    }
                    NodeRecord node      = *current;
                    node.status          = "online";
                    node.agentVersion    = registration.agentVersion;
                    node.certFingerprint = registration.certFingerprint;
                    node.labelsJson      = registration.labelsJson;
                    node.lastSeen        = now;
                    node.version         = current->version + 1;
                    return node;
                });
    }

    AsynGyanis::Core::Task<std::optional<NodeRecord>> NodeRepository::heartbeat(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId, std::string agentVersion)
    {
        co_return co_await m_database->withConnection<std::optional<NodeRecord>>(
                loop,
                [nodeId, agentVersion = std::move(agentVersion)](AsynGyanis::Database::DatabaseConnection &connection) -> std::optional<NodeRecord>
                {
                    const std::string                                           now    = Common::nowIso8601Utc();
                    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(
                            "UPDATE biz_node SET status = 'online', last_seen = ?, agent_version = CASE WHEN ? = '' THEN agent_version ELSE ? END, version = version + 1, "
                            "updated_at = ? WHERE id = ? AND deleted = 0",
                            std::array<AsynGyanis::Database::DatabaseValue, 5>{now, agentVersion, agentVersion, now, nodeId});
                    if (!result)
                    {
                        throw Common::DatabaseException("更新节点心跳失败：" + connection.lastError());
                    }
                    return findByIdOnConnection(connection, nodeId);
                });
    }
} // namespace EdgePulse::Device
