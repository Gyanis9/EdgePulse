#include "Core/Device/NodeRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Database/SqlRetry.h"
#include "Base/Log/LogMacros.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace EdgePulse::Device
{
    namespace
    {
        constexpr const char *kNodeColumns = "id, tenant_id, name, status, agent_version, cert_fingerprint, cert_not_after, cert_revoked_at, labels, last_seen, version, group_id, "
                                             "owner_dept_id, created_by, "
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

        constexpr int                       kRegistrationAttempts = 2; ///< 并发注册最多尝试几次
        constexpr std::chrono::milliseconds kRegistrationBackoff{2};   ///< 两次尝试之间的退避，只在工作线程上等

        /**
         * @brief 回滚当前事务；回滚本身失败时不让它盖过真正的失败原因
         */
        void rollbackQuietly(AsynGyanis::Database::DatabaseConnection &connection)
        {
            try
            {
                Sql::executeOrThrowVoid(connection, "ROLLBACK");
            } catch (...)
            {
            }
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
            node.certNotAfter    = readText(result.getValue("cert_not_after"));
            node.certRevokedAt   = readText(result.getValue("cert_revoked_at"));
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
         * @brief 判断是否已有同名软删除节点占着全局唯一索引
         * @param connection 当前数据库连接
         * @param name 注册声明的节点名
         * @return true 表示插入冲突来自软删除记录，重试也不会成功
         */
        [[nodiscard]] bool hasDeletedNodeWithName(AsynGyanis::Database::DatabaseConnection &connection, const std::string &name)
        {
            const std::vector<AsynGyanis::Database::DatabaseValue>      parameters{name};
            const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute("SELECT 1 FROM biz_node WHERE name = ? AND deleted = 1 LIMIT 1", parameters);
            if (!result)
            {
                throw Common::DatabaseException("查询已删除同名节点失败：" + connection.lastError());
            }
            return result->next();
        }

        /**
         * @brief 写一条新节点；调用方保证名称未占用
         */
        [[nodiscard]] NodeRecord insertNode(AsynGyanis::Database::DatabaseConnection &connection, const std::int64_t tenantId, const std::string &name,
                                            const std::string &labelsJson, const std::string &status, const std::string &agentVersion, const std::string &certFingerprint,
                                            const std::string &certNotAfter, const std::string &lastSeen, const std::int64_t groupId = 200, const std::int64_t ownerDeptId = 0,
                                            const std::int64_t createdBy = 0)
        {
            const std::int64_t                                          id  = Common::nextIdentifier();
            const std::string                                           now = Common::nowDateTimeUtc();
            std::vector<AsynGyanis::Database::DatabaseValue>            parameters{id,
                                                                                   tenantId,
                                                                                   name,
                                                                                   status,
                                                                                   agentVersion,
                                                                                   certFingerprint,
                                                                                   nullableText(certNotAfter),
                                                                                   nullableText(labelsJson),
                                                                                   lastSeen.empty() ? AsynGyanis::Database::DatabaseValue{std::monostate{}}
                                                                                                    : AsynGyanis::Database::DatabaseValue{lastSeen},
                                                                                   now,
                                                                                   now,
                                                                                   createdBy,
                                                                                   groupId,
                                                                                   ownerDeptId};
            const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result =
                    connection.execute("INSERT INTO biz_node (id, tenant_id, name, status, agent_version, cert_fingerprint, cert_not_after, labels, last_seen, version, deleted, "
                                       "created_at, updated_at, "
                                       "created_by, updated_by, group_id, owner_dept_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 1, 0, ?, ?, ?, 0, ?, ?)",
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
            node.certNotAfter    = certNotAfter;
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
                { return insertNode(connection, tenant.tenantId, name, labelsJson, "offline", "", "", "", "", groupId, ownerDeptId, createdBy); });
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
                    const System::TenantScope tenant{registration.tenantId, false};
                    // 新建、改绑与同指纹重注册的判定必须在事务里完成：调用方读到的是旧快照，
                    // 只有把令牌消费和节点写入放进同一笔事务，才能挡住两次读之间的改绑或吊销
                    for (int attempt = 0; attempt < kRegistrationAttempts; ++attempt)
                    {
                        bool        insertingNode = false;
                        std::string consumedAction;
                        try
                        {
                            Sql::executeOrThrowVoid(connection, "BEGIN");
                            const std::string               now             = Common::nowDateTimeUtc();
                            const std::optional<NodeRecord> current         = findByNameOnConnection(connection, registration.name, tenant);
                            const bool                      revoked         = current.has_value() && !current->certRevokedAt.empty();
                            const bool                      sameCertificate = current.has_value() && !revoked && current->certFingerprint == registration.certFingerprint;
                            if (current.has_value() && current->certFingerprint != registration.certFingerprint && registration.enrollmentMode != "required")
                            {
                                // off 模式没有令牌约束；改绑仍要留下可追溯的告警日志
                                LOG_WARN_FMT("off 模式允许证书指纹改绑：node={} name={}", current->id, registration.name);
                            }
                            if (!sameCertificate && registration.enrollmentMode == "required")
                            {
                                // 动作由事务内读到的节点状态决定：enroll 只给新节点，rebind 只给已有节点
                                const std::string action = current.has_value() ? "rebind" : "enroll";
                                if (registration.enrollmentTokenHash.empty())
                                {
                                    rollbackQuietly(connection);
                                    return std::unexpected(AgentUpsertFailure::TokenRejected);
                                }
                                // 令牌消费与节点写入必须同成同败；条件里钉住未使用、未吊销、未过期、动作与节点名
                                const std::vector<AsynGyanis::Database::DatabaseValue> tokenParameters{
                                        now, registration.enrollmentTokenHash, registration.tenantId, action, registration.name, now};
                                const auto tokenUpdate = Sql::executeOrThrow(connection,
                                                                             "UPDATE biz_agent_enrollment SET used_at = ? "
                                                                             "WHERE token_hash = ? AND tenant_id = ? AND action = ? AND (node_name = '' OR node_name = ?) "
                                                                             "AND used_at IS NULL AND revoked_at IS NULL AND expires_at > ?",
                                                                             tokenParameters);
                                if (tokenUpdate->affectedRowCount() == 0)
                                {
                                    rollbackQuietly(connection);
                                    return std::unexpected(AgentUpsertFailure::TokenRejected);
                                }
                                // 令牌与节点同成同败，提交后再记日志，避免回滚时留下已消费的假记录
                                consumedAction = action;
                            }
                            if (!current.has_value())
                            {
                                insertingNode   = true;
                                NodeRecord node = insertNode(connection, registration.tenantId, registration.name, registration.labelsJson, "online", registration.agentVersion,
                                                             registration.certFingerprint, registration.certNotAfter, now);
                                Sql::executeOrThrowVoid(connection, "COMMIT");
                                if (!consumedAction.empty())
                                {
                                    LOG_INFO_FMT("消费注册令牌：tenant={} action={} node={}", registration.tenantId, consumedAction, registration.name);
                                }
                                return std::expected<AgentUpsertOutcome, AgentUpsertFailure>{AgentUpsertOutcome{std::move(node), true, true}};
                            }
                            const bool                                               becameOnline = current->status != "online";
                            const std::array<AsynGyanis::Database::DatabaseValue, 8> parameters{registration.agentVersion,
                                                                                                registration.certFingerprint,
                                                                                                nullableText(registration.certNotAfter),
                                                                                                nullableText(registration.labelsJson),
                                                                                                now,
                                                                                                now,
                                                                                                current->id,
                                                                                                current->version};
                            // 走到这里说明本次出示的证书已被事务内确认允许写入（同指纹、已消费令牌或 off 模式），才清掉旧的吊销时间
                            // 版本条件把读取与写入绑成一次比较交换：期间提交的吊销或改绑会让这次更新落空，而不是被覆盖
                            const auto update = Sql::executeOrThrow(
                                    connection,
                                    "UPDATE biz_node SET status = 'online', agent_version = ?, cert_fingerprint = ?, cert_not_after = ?, cert_revoked_at = NULL, "
                                    "labels = ?, last_seen = ?, version = version + 1, updated_at = ? WHERE id = ? AND version = ? AND deleted = 0",
                                    parameters);
                            if (update->affectedRowCount() == 0)
                            {
                                // 节点在读取后被软删除，或版本被吊销/改绑推进：回滚后整套判定重来，不能把旧快照写回去
                                rollbackQuietly(connection);
                                if (attempt + 1 < kRegistrationAttempts)
                                {
                                    continue;
                                }
                                return std::unexpected(AgentUpsertFailure::ConcurrentRegistration);
                            }
                            Sql::executeOrThrowVoid(connection, "COMMIT");
                            if (!consumedAction.empty())
                            {
                                LOG_INFO_FMT("消费注册令牌：tenant={} action={} node={}", registration.tenantId, consumedAction, registration.name);
                            }
                            NodeRecord node      = *current;
                            node.status          = "online";
                            node.agentVersion    = registration.agentVersion;
                            node.certFingerprint = registration.certFingerprint;
                            node.certNotAfter    = registration.certNotAfter;
                            node.certRevokedAt.clear();
                            node.labelsJson = registration.labelsJson;
                            node.lastSeen   = now;
                            node.version    = current->version + 1;
                            return std::expected<AgentUpsertOutcome, AgentUpsertFailure>{AgentUpsertOutcome{std::move(node), becameOnline, false}};
                        } catch (const Common::DatabaseException &failure)
                        {
                            rollbackQuietly(connection);
                            if (!Sql::isRetryableSqlFailure(failure.what()))
                            {
                                throw;
                            }
                            // 插入撞唯一索引且同名节点只是被软删除时，重试永远不会成功；返回可操作的名字冲突而不是并发冲突
                            bool deletedNameConflict = false;
                            if (insertingNode && Sql::isUniqueViolation(failure.what()))
                            {
                                try
                                {
                                    deletedNameConflict = hasDeletedNodeWithName(connection, registration.name);
                                } catch (const Common::DatabaseException &)
                                {
                                    // 判断本身失败时仍按可重试的并发冲突处理，不让它盖过原始失败
                                }
                            }
                            if (deletedNameConflict)
                            {
                                return std::unexpected(AgentUpsertFailure::NameConflict);
                            }
                            if (attempt + 1 < kRegistrationAttempts)
                            {
                                // 工作线程上的短暂退避，避开另一条注册刚拿到的写锁；不能在事件循环线程上睡
                                std::this_thread::sleep_for(kRegistrationBackoff);
                                continue;
                            }
                            return std::unexpected(AgentUpsertFailure::ConcurrentRegistration);
                        } catch (...)
                        {
                            rollbackQuietly(connection);
                            throw;
                        }
                    }
                    // 循环体要么返回，要么在最后一次失败后返回；这里只兜住理论上的落空
                    return std::unexpected(AgentUpsertFailure::ConcurrentRegistration);
                });
    }

    AsynGyanis::Core::Task<std::optional<NodeRecord>> NodeRepository::revokeCertificate(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        co_return co_await m_database->withConnection<std::optional<NodeRecord>>(
                loop,
                [nodeId, tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> std::optional<NodeRecord>
                {
                    const std::string                                now = Common::nowDateTimeUtc();
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters{now, now, nodeId};
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    // 清空指纹是吊销的核心：之后 requireAgentNode 与 WebSocket hello 都无法再绑定成功
                    const auto result =
                            Sql::executeOrThrow(connection,
                                                "UPDATE biz_node SET cert_fingerprint = '', cert_revoked_at = ?, status = 'offline', version = version + 1, updated_at = ? "
                                                "WHERE id = ? AND deleted = 0" +
                                                        clause,
                                                parameters);
                    if (result->affectedRowCount() == 0)
                    {
                        return std::nullopt;
                    }
                    return findByIdOnConnection(connection, nodeId, tenant);
                });
    }

    AsynGyanis::Core::Task<std::int64_t> NodeRepository::countExpiringCertificates(AsynGyanis::Core::EventLoop &loop, const int withinDays)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [withinDays, tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> std::int64_t
                {
                    const std::string cutoff =
                            Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::seconds{static_cast<std::int64_t>(withinDays) * 24 * 3600});
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters{cutoff};
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(connection,
                                                          "SELECT COUNT(*) AS total FROM biz_node WHERE deleted = 0 AND cert_revoked_at IS NULL AND cert_not_after IS NOT NULL AND "
                                                          "cert_not_after <= ?" +
                                                                  clause,
                                                          parameters);
                    return rows->next() ? Sql::integer(*rows, "total") : 0;
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
                            "updated_at = ? WHERE id = ? AND deleted = 0 AND cert_revoked_at IS NULL" +
                                    clause,
                            parameters);
                    if (!result)
                    {
                        throw Common::DatabaseException("更新节点心跳失败：" + connection.lastError());
                    }
                    // 读取之后证书被吊销：这条在途心跳不能把节点重新拉回在线
                    if (result->affectedRowCount() == 0)
                    {
                        return std::nullopt;
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
                    std::string                                      filter = " WHERE status = 'online' AND deleted = 0 AND last_seen IS NOT NULL AND last_seen < ?";
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
                                                                "WHERE id = ? AND status = 'online' AND deleted = 0",
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

    AsynGyanis::Core::Task<std::vector<NodeRecord>> NodeRepository::listOfflineWithoutSystemAlarm(AsynGyanis::Core::EventLoop &loop, const std::size_t limit)
    {
        co_return co_await m_database->withConnection<std::vector<NodeRecord>>(
                loop,
                [limit, tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> std::vector<NodeRecord>
                {
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters;
                    // 已吊销证书的节点永远不会再心跳，补告警只会制造一条无法销案的 critical 告警
                    std::string filter = " WHERE status = 'offline' AND deleted = 0 AND cert_revoked_at IS NULL AND last_seen IS NOT NULL";
                    if (!tenant.allTenants)
                    {
                        filter += " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    // LIMIT 占位符在筛选条件之后，参数必须按语句里的出现顺序追加
                    parameters.emplace_back(static_cast<std::int64_t>(limit));
                    const auto rows = Sql::executeOrThrow(
                            connection,
                            std::string{"SELECT "} + kNodeColumns + " FROM biz_node" + filter +
                                    " AND NOT EXISTS (SELECT 1 FROM biz_alarm a WHERE a.node_id = biz_node.id AND a.rule_id = 0 AND a.metric = 'node.online' AND a.status "
                                    "<> 'resolved')"
                                    " ORDER BY last_seen LIMIT ?",
                            parameters);
                    std::vector<NodeRecord> nodes;
                    while (rows->next())
                    {
                        nodes.push_back(readNode(*rows));
                    }
                    return nodes;
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
