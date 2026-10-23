#include "Core/Device/NodeRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"
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
        constexpr const char *kNodeColumns = "id, tenant_id, name, status, agent_version, cert_fingerprint, labels, last_seen, version, group_id, owner_dept_id, created_by, "
                                             "(SELECT name FROM biz_node_group g WHERE g.id = biz_node.group_id AND g.deleted = 0) AS group_name";

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
            node.tenantId        = readInteger(result.getValue("tenant_id"));
            node.name            = readText(result.getValue("name"));
            node.status          = readText(result.getValue("status"));
            node.agentVersion    = readText(result.getValue("agent_version"));
            node.certFingerprint = readText(result.getValue("cert_fingerprint"));
            node.labelsJson      = readText(result.getValue("labels"));
            node.lastSeen        = readText(result.getValue("last_seen"));
            node.groupId         = readInteger(result.getValue("group_id"));
            node.groupName       = readText(result.getValue("group_name"));
            node.ownerDeptId     = readInteger(result.getValue("owner_dept_id"));
            node.createdBy       = readInteger(result.getValue("created_by"));
            node.version         = readInteger(result.getValue("version"));
            return node;
        }

        [[nodiscard]] std::optional<NodeRecord> findByIdOnConnection(AsynGyanis::Database::DatabaseConnection &connection, const std::int64_t nodeId,
                                                                     const System::TenantScope &tenant)
        {
            std::vector<AsynGyanis::Database::DatabaseValue> parameters{nodeId};
            std::string                                      clause;
            if (!tenant.allTenants)
            {
                clause = " AND tenant_id = ?";
                parameters.emplace_back(tenant.tenantId);
            }
            const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result =
                    connection.execute(std::string{"SELECT "} + kNodeColumns + " FROM biz_node WHERE id = ? AND deleted = 0" + clause, parameters);
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

        [[nodiscard]] std::optional<NodeRecord> findByNameOnConnection(AsynGyanis::Database::DatabaseConnection &connection, const std::string &name,
                                                                       const System::TenantScope &tenant)
        {
            std::vector<AsynGyanis::Database::DatabaseValue> parameters{name};
            std::string                                      clause;
            if (!tenant.allTenants)
            {
                clause = " AND tenant_id = ?";
                parameters.emplace_back(tenant.tenantId);
            }
            const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result =
                    connection.execute(std::string{"SELECT "} + kNodeColumns + " FROM biz_node WHERE name = ? AND deleted = 0" + clause, parameters);
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
        [[nodiscard]] NodeRecord insertNode(AsynGyanis::Database::DatabaseConnection &connection, const std::int64_t tenantId, const std::string &name,
                                            const std::string &labelsJson, const std::string &status, const std::string &agentVersion, const std::string &certFingerprint,
                                            const std::string &lastSeen, const std::int64_t groupId = 200, const std::int64_t ownerDeptId = 0, const std::int64_t createdBy = 0)
        {
            const std::int64_t                                          id  = Common::nextIdentifier();
            const std::string                                           now = Common::nowDateTimeUtc();
            std::vector<AsynGyanis::Database::DatabaseValue>            parameters{id,
                                                                                   tenantId,
                                                                                   name,
                                                                                   status,
                                                                                   agentVersion,
                                                                                   certFingerprint,
                                                                                   nullableText(labelsJson),
                                                                                   lastSeen.empty() ? AsynGyanis::Database::DatabaseValue{std::monostate{}}
                                                                                                    : AsynGyanis::Database::DatabaseValue{lastSeen},
                                                                                   now,
                                                                                   now,
                                                                                   createdBy,
                                                                                   groupId,
                                                                                   ownerDeptId};
            const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(
                    "INSERT INTO biz_node (id, tenant_id, name, status, agent_version, cert_fingerprint, labels, last_seen, version, deleted, created_at, updated_at, created_by, "
                    "updated_by, group_id, owner_dept_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 1, 0, ?, ?, ?, 0, ?, ?)",
                    parameters);
            if (!result)
            {
                throw Common::DatabaseException("创建节点失败：" + connection.lastError());
            }
            NodeRecord node;
            node.id              = id;
            node.tenantId        = tenantId;
            node.name            = name;
            node.status          = status;
            node.agentVersion    = agentVersion;
            node.certFingerprint = certFingerprint;
            node.labelsJson      = labelsJson;
            node.lastSeen        = lastSeen;
            node.groupId         = groupId;
            node.ownerDeptId     = ownerDeptId;
            node.createdBy       = createdBy;
            node.version         = 1;
            return node;
        }
    } // namespace

    NodeRepository::NodeRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant) : m_database(std::move(database)), m_tenant(tenant)
    {
    }

    std::shared_ptr<NodeRepository> NodeRepository::scoped(const System::TenantScope tenant) const
    {
        return std::make_shared<NodeRepository>(m_database, tenant);
    }

    AsynGyanis::Core::Task<NodePage> NodeRepository::list(AsynGyanis::Core::EventLoop &loop, Common::PageParameters parameters, std::string keyword, std::string status,
                                                          const std::int64_t groupId, const NodeListScope scope)
    {
        co_return co_await m_database->withConnection<NodePage>(
                loop,
                [parameters, keyword = std::move(keyword), status = std::move(status), groupId, scope,
                 tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> NodePage
                {
                    const std::string keywordPattern = "%" + keyword + "%";
                    std::string       filter         = " FROM biz_node WHERE deleted = 0 AND (? = '' OR name LIKE ?) AND (? = '' OR status = ?) AND (? = 0 OR group_id = ?)";
                    std::vector<AsynGyanis::Database::DatabaseValue> countParameters{keyword, keywordPattern, status, status, groupId, groupId};
                    if (!tenant.allTenants)
                    {
                        filter += " AND tenant_id = ?";
                        countParameters.emplace_back(tenant.tenantId);
                    }
                    if (!scope.all)
                    {
                        if (scope.selfOnly)
                        {
                            filter += " AND created_by = ?";
                            countParameters.emplace_back(scope.userId);
                        } else if (scope.allowedDeptIds.empty())
                        {
                            // 受限角色没有任何可见部门时不能退化成不过滤
                            filter += " AND 1 = 0";
                        } else
                        {
                            filter += " AND owner_dept_id IN (";
                            for (std::size_t index = 0; index < scope.allowedDeptIds.size(); ++index)
                            {
                                filter += index == 0 ? "?" : ", ?";
                                countParameters.emplace_back(scope.allowedDeptIds[index]);
                            }
                            filter += ")";
                        }
                    }
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

                    std::vector<AsynGyanis::Database::DatabaseValue> rowParameters = countParameters;
                    rowParameters.emplace_back(static_cast<std::int64_t>(parameters.pageSize));
                    rowParameters.emplace_back(static_cast<std::int64_t>(parameters.offset()));
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
        co_return co_await m_database->withConnection<std::optional<NodeRecord>>(loop, [nodeId, tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection)
                                                                                 { return findByIdOnConnection(connection, nodeId, tenant); });
    }

    AsynGyanis::Core::Task<std::optional<NodeRecord>> NodeRepository::findByName(AsynGyanis::Core::EventLoop &loop, std::string name)
    {
        co_return co_await m_database->withConnection<std::optional<NodeRecord>>(loop,
                                                                                 [name = std::move(name), tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection)
                                                                                 { return findByNameOnConnection(connection, name, tenant); });
    }

    AsynGyanis::Core::Task<NodeRecord> NodeRepository::create(AsynGyanis::Core::EventLoop &loop, std::string name, std::string labelsJson, const std::int64_t groupId,
                                                              const std::int64_t ownerDeptId, const std::int64_t createdBy)
    {
        co_return co_await m_database->withConnection<NodeRecord>(
                loop, [name = std::move(name), labelsJson = std::move(labelsJson), groupId, ownerDeptId, createdBy,
                       tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection)
                { return insertNode(connection, tenant.tenantId, name, labelsJson, "offline", "", "", "", groupId, ownerDeptId, createdBy); });
    }

    AsynGyanis::Core::Task<std::expected<NodeRecord, NodeMutationFailure>> NodeRepository::update(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId,
                                                                                                  std::optional<std::string> name, std::optional<std::string> labelsJson,
                                                                                                  std::optional<std::int64_t> groupId, const std::int64_t expectedVersion)
    {
        co_return co_await m_database->withConnection<std::expected<NodeRecord, NodeMutationFailure>>(
                loop,
                [nodeId, name = std::move(name), labelsJson = std::move(labelsJson), groupId, expectedVersion,
                 tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> std::expected<NodeRecord, NodeMutationFailure>
                {
                    const std::optional<NodeRecord> current = findByIdOnConnection(connection, nodeId, tenant);
                    if (!current.has_value())
                    {
                        return std::unexpected(NodeMutationFailure::NotFound);
                    }
                    const std::string                                newName   = name.value_or(current->name);
                    const std::string                                newLabels = labelsJson.value_or(current->labelsJson);
                    const std::int64_t                               newGroup  = groupId.value_or(current->groupId);
                    const std::string                                now       = Common::nowDateTimeUtc();
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters{newName, nullableText(newLabels), newGroup, now, nodeId, expectedVersion};
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(
                            "UPDATE biz_node SET name = ?, labels = ?, group_id = ?, version = version + 1, updated_at = ? WHERE id = ? AND version = ? AND deleted = 0" + clause,
                            parameters);
                    if (!result)
                    {
                        throw Common::DatabaseException("更新节点失败：" + connection.lastError());
                    }
                    // 影响行数为 0：节点在读取之后被删除或版本已变
                    if (result->affectedRowCount() == 0)
                    {
                        return findByIdOnConnection(connection, nodeId, tenant).has_value() ? std::unexpected(NodeMutationFailure::Conflict)
                                                                                            : std::unexpected(NodeMutationFailure::NotFound);
                    }
                    NodeRecord updated = *current;
                    updated.name       = newName;
                    updated.labelsJson = newLabels;
                    updated.groupId    = newGroup;
                    updated.version    = current->version + 1;
                    return updated;
                });
    }

    AsynGyanis::Core::Task<bool> NodeRepository::remove(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [nodeId, tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                                                            {
                                                                std::vector<AsynGyanis::Database::DatabaseValue> parameters{Common::nowDateTimeUtc(), nodeId};
                                                                std::string                                      clause;
                                                                if (!tenant.allTenants)
                                                                {
                                                                    clause = " AND tenant_id = ?";
                                                                    parameters.emplace_back(tenant.tenantId);
                                                                }
                                                                const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(
                                                                        "UPDATE biz_node SET deleted = 1, updated_at = ? WHERE id = ? AND deleted = 0" + clause, parameters);
                                                                if (!result)
                                                                {
                                                                    throw Common::DatabaseException("删除节点失败：" + connection.lastError());
                                                                }
                                                                return result->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<std::expected<AgentUpsertOutcome, AgentUpsertFailure>> NodeRepository::upsertAgent(AsynGyanis::Core::EventLoop &loop, AgentUpsert registration)
    {
        co_return co_await m_database->withConnection<std::expected<AgentUpsertOutcome, AgentUpsertFailure>>(
                loop,
                [registration = std::move(registration)](AsynGyanis::Database::DatabaseConnection &connection) -> std::expected<AgentUpsertOutcome, AgentUpsertFailure>
                {
                    const std::string               now = Common::nowDateTimeUtc();
                    const System::TenantScope       tenant{registration.tenantId, false};
                    const std::optional<NodeRecord> current = findByNameOnConnection(connection, registration.name, tenant);
                    if (!current.has_value())
                    {
                        NodeRecord node = insertNode(connection, registration.tenantId, registration.name, registration.labelsJson, "online", registration.agentVersion,
                                                     registration.certFingerprint, now);
                        return AgentUpsertOutcome{std::move(node), true, true};
                    }
                    const bool                                               becameOnline = current->status != "online";
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
                    return AgentUpsertOutcome{std::move(node), becameOnline, false};
                });
    }

    AsynGyanis::Core::Task<std::optional<HeartbeatOutcome>> NodeRepository::heartbeat(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId, std::string agentVersion)
    {
        co_return co_await m_database->withConnection<std::optional<HeartbeatOutcome>>(
                loop,
                [nodeId, agentVersion = std::move(agentVersion), tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> std::optional<HeartbeatOutcome>
                {
                    // 先读旧状态再更新：更新后的 status 恒为 online，转换与否只能从更新前的值判断
                    const std::optional<NodeRecord> current = findByIdOnConnection(connection, nodeId, tenant);
                    if (!current.has_value())
                    {
                        return std::nullopt;
                    }
                    const std::string                                now = Common::nowDateTimeUtc();
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters{now, agentVersion, agentVersion, now, nodeId};
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(
                            "UPDATE biz_node SET status = 'online', last_seen = ?, agent_version = CASE WHEN ? = '' THEN agent_version ELSE ? END, version = version + 1, "
                            "updated_at = ? WHERE id = ? AND deleted = 0" +
                                    clause,
                            parameters);
                    if (!result)
                    {
                        throw Common::DatabaseException("更新节点心跳失败：" + connection.lastError());
                    }
                    const std::optional<NodeRecord> updated = findByIdOnConnection(connection, nodeId, tenant);
                    if (!updated.has_value())
                    {
                        return std::nullopt;
                    }
                    return HeartbeatOutcome{*updated, current->status != "online"};
                });
    }

    AsynGyanis::Core::Task<std::vector<NodeRecord>> NodeRepository::markStaleOffline(AsynGyanis::Core::EventLoop &loop, std::string cutoff, const std::size_t limit)
    {
        co_return co_await m_database->withConnection<std::vector<NodeRecord>>(
                loop,
                [cutoff = std::move(cutoff), limit, tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> std::vector<NodeRecord>
                {
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters{cutoff};
                    std::string                                      filter = " WHERE status = 'online' AND last_seen IS NOT NULL AND last_seen < ?";
                    if (!tenant.allTenants)
                    {
                        filter += " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    // LIMIT 占位符在筛选条件之后，参数必须按语句里的出现顺序追加
                    parameters.emplace_back(static_cast<std::int64_t>(limit));
                    const auto                rows = Sql::executeOrThrow(connection, "SELECT id FROM biz_node" + filter + " ORDER BY last_seen LIMIT ?", parameters);
                    std::vector<std::int64_t> ids;
                    while (rows->next())
                    {
                        ids.push_back(Sql::integer(*rows, "id"));
                    }

                    std::vector<NodeRecord> changed;
                    for (const std::int64_t id: ids)
                    {
                        // 选中与更新之间可能有心跳把节点拉回在线；条件更新保证每个离线过程只转换一次
                        const std::array<AsynGyanis::Database::DatabaseValue, 2> updateParameters{Common::nowDateTimeUtc(), id};
                        const auto update = Sql::executeOrThrow(connection,
                                                                "UPDATE biz_node SET status = 'offline', version = version + 1, updated_at = ? "
                                                                "WHERE id = ? AND status = 'online'",
                                                                updateParameters);
                        if (update->affectedRowCount() == 0)
                        {
                            continue;
                        }
                        if (const auto node = findByIdOnConnection(connection, id, tenant); node.has_value())
                        {
                            changed.push_back(*node);
                        }
                    }
                    return changed;
                });
    }

    AsynGyanis::Core::Task<NodeStatusCounts> NodeRepository::countByStatus(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await m_database->withConnection<NodeStatusCounts>(
                loop,
                [tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> NodeStatusCounts
                {
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters;
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(connection, "SELECT status, COUNT(*) AS total FROM biz_node WHERE deleted = 0" + clause + " GROUP BY status", parameters);
                    NodeStatusCounts counts;
                    while (rows->next())
                    {
                        const std::string status = Sql::text(*rows, "status");
                        if (status == "online")
                        {
                            counts.online = Sql::integer(*rows, "total");
                        } else if (status == "offline")
                        {
                            counts.offline = Sql::integer(*rows, "total");
                        }
                    }
                    return counts;
                });
    }
} // namespace EdgePulse::Device
