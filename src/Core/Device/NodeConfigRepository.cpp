#include "Core/Device/NodeConfigRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Database/SqlRetry.h"
#include "Base/Exception/EdgePulseException.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace EdgePulse::Device
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        constexpr const char *kConfigColumns   = "node_id, tenant_id, desired_json, desired_version, desired_hash, desired_updated_at, desired_updated_by, reported_json, "
                                                 "reported_version, reported_hash, reported_status, reported_message, reported_at";
        constexpr const char *kRevisionColumns = "id, node_id, version, content_json, content_hash, note, created_by, created_at";

        /// 修订保留窗口：每个节点只保留最新该数量的修订
        constexpr std::int64_t kRevisionRetentionWindow = 100;
        /// 整段配置写入在可重试的 SQL 并发失败后最多尝试几次
        constexpr int kConfigWriteAttempts = 3;
        /// 两次尝试之间的退避，只在工作线程上等
        constexpr std::chrono::milliseconds kConfigWriteBackoff{2};

        /**
         * @brief 回滚当前事务；回滚本身失败时不让它盖过真正的失败原因
         */
        void rollbackQuietly(DatabaseConnection &connection)
        {
            try
            {
                Sql::executeOrThrowVoid(connection, "ROLLBACK");
            } catch (...)
            {
            }
        }

        [[nodiscard]] NodeConfigRecord readConfig(const DatabaseResult &result)
        {
            NodeConfigRecord record;
            record.nodeId           = Sql::integer(result, "node_id");
            record.tenantId         = Sql::integer(result, "tenant_id");
            record.desiredJson      = Sql::text(result, "desired_json");
            record.desiredVersion   = Sql::integer(result, "desired_version");
            record.desiredHash      = Sql::text(result, "desired_hash");
            record.desiredUpdatedAt = Sql::dateTimeText(result, "desired_updated_at");
            record.desiredUpdatedBy = Sql::integer(result, "desired_updated_by");
            record.reportedJson     = Sql::text(result, "reported_json");
            record.reportedVersion  = Sql::integer(result, "reported_version");
            record.reportedHash     = Sql::text(result, "reported_hash");
            record.reportedStatus   = Sql::text(result, "reported_status");
            record.reportedMessage  = Sql::text(result, "reported_message");
            record.reportedAt       = Sql::dateTimeText(result, "reported_at");
            // 版本、内容摘要与上报状态三者同时相符才算同步；只比版本会漏掉“同版本改了内容”
            record.inSync = record.desiredVersion == record.reportedVersion && record.desiredHash == record.reportedHash && record.reportedStatus == "applied";
            return record;
        }

        [[nodiscard]] NodeConfigRevision readRevision(const DatabaseResult &result)
        {
            NodeConfigRevision revision;
            revision.id          = Sql::integer(result, "id");
            revision.nodeId      = Sql::integer(result, "node_id");
            revision.version     = Sql::integer(result, "version");
            revision.contentJson = Sql::text(result, "content_json");
            revision.contentHash = Sql::text(result, "content_hash");
            revision.note        = Sql::text(result, "note");
            revision.createdBy   = Sql::integer(result, "created_by");
            revision.createdAt   = Sql::dateTimeText(result, "created_at");
            return revision;
        }

        /**
         * @brief 按租户边界追加过滤条件
         * @param tenant 租户边界
         * @param parameters 参数列表；需要过滤时追加租户标识
         * @return 形如 " AND tenant_id = ?" 的子句；全租户视图返回空串
         */
        [[nodiscard]] std::string tenantClause(const System::TenantScope &tenant, std::vector<DatabaseValue> &parameters)
        {
            if (tenant.allTenants)
            {
                return {};
            }
            parameters.emplace_back(tenant.tenantId);
            return " AND tenant_id = ?";
        }

        [[nodiscard]] std::optional<NodeConfigRecord> findConfigOnConnection(DatabaseConnection &connection, const std::int64_t nodeId, const System::TenantScope &tenant)
        {
            std::vector<DatabaseValue> parameters{nodeId};
            const std::string          clause = tenantClause(tenant, parameters);
            const auto rows = Sql::executeOrThrow(connection, std::string{"SELECT "} + kConfigColumns + " FROM biz_node_config WHERE node_id = ?" + clause, parameters);
            if (!rows->next())
            {
                return std::nullopt;
            }
            return readConfig(*rows);
        }

        /**
         * @brief 在租户边界内读节点归属租户；节点不存在或不可见时为空
         */
        [[nodiscard]] std::optional<std::int64_t> findNodeTenantOnConnection(DatabaseConnection &connection, const std::int64_t nodeId, const System::TenantScope &tenant)
        {
            std::vector<DatabaseValue> parameters{nodeId};
            const std::string          clause = tenantClause(tenant, parameters);
            const auto                 rows   = Sql::executeOrThrow(connection, "SELECT tenant_id FROM biz_node WHERE id = ? AND deleted = 0" + clause, parameters);
            if (!rows->next())
            {
                return std::nullopt;
            }
            return Sql::integer(*rows, "tenant_id");
        }
    } // namespace

    NodeConfigRepository::NodeConfigRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant) : m_database(std::move(database)), m_tenant(tenant)
    {
    }

    std::shared_ptr<NodeConfigRepository> NodeConfigRepository::scoped(const System::TenantScope tenant) const
    {
        return std::make_shared<NodeConfigRepository>(m_database, tenant);
    }

    AsynGyanis::Core::Task<std::optional<NodeConfigRecord>> NodeConfigRepository::find(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        co_return co_await m_database->withConnection<std::optional<NodeConfigRecord>>(loop, [nodeId, tenant = m_tenant](DatabaseConnection &connection)
                                                                                       { return findConfigOnConnection(connection, nodeId, tenant); });
    }

    AsynGyanis::Core::Task<std::int64_t> NodeConfigRepository::desiredVersion(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(loop,
                                                                    [nodeId, tenant = m_tenant](DatabaseConnection &connection) -> std::int64_t
                                                                    {
                                                                        std::vector<DatabaseValue> parameters{nodeId};
                                                                        const std::string          clause = tenantClause(tenant, parameters);
                                                                        const auto                 rows   = Sql::executeOrThrow(
                                                                                connection, "SELECT desired_version FROM biz_node_config WHERE node_id = ?" + clause, parameters);
                                                                        return rows->next() ? Sql::integer(*rows, "desired_version") : 0;
                                                                    });
    }

    AsynGyanis::Core::Task<std::expected<NodeConfigRecord, NodeConfigFailure>> NodeConfigRepository::applyDesired(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId,
                                                                                                                  std::string contentJson, std::string contentHash,
                                                                                                                  const std::int64_t expectedVersion, std::string note,
                                                                                                                  const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<std::expected<NodeConfigRecord, NodeConfigFailure>>(
                loop,
                [nodeId, contentJson = std::move(contentJson), contentHash = std::move(contentHash), expectedVersion, note = std::move(note), operatorId,
                 tenant = m_tenant](DatabaseConnection &connection) -> std::expected<NodeConfigRecord, NodeConfigFailure>
                {
                    for (int attempt = 0; attempt < kConfigWriteAttempts; ++attempt)
                    {
                        try
                        {
                            Sql::executeOrThrowVoid(connection, "BEGIN");
                            const std::optional<std::int64_t> nodeTenant = findNodeTenantOnConnection(connection, nodeId, tenant);
                            if (!nodeTenant.has_value())
                            {
                                rollbackQuietly(connection);
                                return std::unexpected(NodeConfigFailure::NotFound);
                            }
                            // 配置行只认节点自己的租户：全租户视图写入时也不能落在调用方的默认租户
                            const System::TenantScope ownerScope{*nodeTenant, false};
                            const auto                current = Sql::executeOrThrow(connection, "SELECT desired_version FROM biz_node_config WHERE node_id = ? AND tenant_id = ?",
                                                                                    std::vector<DatabaseValue>{nodeId, *nodeTenant});
                            const bool                exists  = current->next();
                            std::int64_t              currentVersion = exists ? Sql::integer(*current, "desired_version") : 0;
                            if (expectedVersion != currentVersion)
                            {
                                rollbackQuietly(connection);
                                return std::unexpected(NodeConfigFailure::VersionConflict);
                            }
                            const std::string  now        = Common::nowDateTimeUtc();
                            const std::int64_t newVersion = currentVersion + 1;
                            try
                            {
                                if (!exists)
                                {
                                    const std::vector<DatabaseValue> parameters{nodeId,     *nodeTenant,   contentJson,   contentHash,  now,
                                                                                operatorId, std::string{}, std::string{}, std::string{}};
                                    Sql::executeOrThrowVoid(connection,
                                                            "INSERT INTO biz_node_config (node_id, tenant_id, desired_json, desired_version, desired_hash, desired_updated_at, "
                                                            "desired_updated_by, reported_json, reported_version, reported_hash, reported_status) "
                                                            "VALUES (?, ?, ?, 1, ?, ?, ?, ?, 0, ?, ?)",
                                                            parameters);
                                } else
                                {
                                    const std::vector<DatabaseValue> parameters{contentJson, contentHash, now, operatorId, nodeId, *nodeTenant, currentVersion};
                                    const auto                       updated =
                                            Sql::executeOrThrow(connection,
                                                                "UPDATE biz_node_config SET desired_json = ?, desired_version = desired_version + 1, desired_hash = ?, "
                                                                "desired_updated_at = ?, desired_updated_by = ? WHERE node_id = ? AND tenant_id = ? AND desired_version = ?",
                                                                parameters);
                                    // 条件更新是乐观锁的最后一道：读与写之间版本被推进时不能覆盖别人的配置
                                    if (updated->affectedRowCount() == 0)
                                    {
                                        rollbackQuietly(connection);
                                        return std::unexpected(NodeConfigFailure::VersionConflict);
                                    }
                                }
                                const std::vector<DatabaseValue> revisionParameters{
                                        Common::nextIdentifier(), *nodeTenant, nodeId, newVersion, contentJson, contentHash, note, operatorId, now};
                                Sql::executeOrThrowVoid(connection,
                                                        "INSERT INTO biz_node_config_revision (id, tenant_id, node_id, version, content_json, content_hash, note, created_by, "
                                                        "created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                                        revisionParameters);
                            } catch (const Common::DatabaseException &failure)
                            {
                                // 唯一约束兜底两次首次写入或同一版本修订的并发竞争
                                rollbackQuietly(connection);
                                if (Sql::isUniqueViolation(failure.what()))
                                {
                                    return std::unexpected(NodeConfigFailure::VersionConflict);
                                }
                                throw;
                            }
                            // 保留最新一条在内共 100 条修订；更早的版本和本次写入同成同败
                            Sql::executeOrThrowVoid(connection, "DELETE FROM biz_node_config_revision WHERE node_id = ? AND tenant_id = ? AND version <= ?",
                                                    std::vector<DatabaseValue>{nodeId, *nodeTenant, newVersion - kRevisionRetentionWindow});
                            const std::optional<NodeConfigRecord> record = findConfigOnConnection(connection, nodeId, ownerScope);
                            if (!record.has_value())
                            {
                                rollbackQuietly(connection);
                                throw Common::DatabaseException("配置写入后读回失败：节点 " + std::to_string(nodeId));
                            }
                            Sql::executeOrThrowVoid(connection, "COMMIT");
                            return *record;
                        } catch (const Common::DatabaseException &failure)
                        {
                            rollbackQuietly(connection);
                            if (!Sql::isRetryableSqlFailure(failure.what()))
                            {
                                throw;
                            }
                            if (attempt + 1 < kConfigWriteAttempts)
                            {
                                // 工作线程上的短暂退避，避开另一条写入刚拿到的写锁；不能在事件循环线程上睡
                                std::this_thread::sleep_for(kConfigWriteBackoff);
                                continue;
                            }
                            // 重试用尽：两个写入者都没能推进版本，按冲突收口让 HTTP 回 409 而不是 500
                            return std::unexpected(NodeConfigFailure::VersionConflict);
                        } catch (...)
                        {
                            rollbackQuietly(connection);
                            throw;
                        }
                    }
                    // 循环体要么返回，要么在最后一次失败后返回；这里只兜住理论上的落空
                    return std::unexpected(NodeConfigFailure::VersionConflict);
                });
    }

    AsynGyanis::Core::Task<std::expected<NodeConfigRecord, NodeConfigFailure>> NodeConfigRepository::report(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId,
                                                                                                            const std::int64_t version, std::string contentJson,
                                                                                                            std::string contentHash, std::string status, std::string message)
    {
        co_return co_await m_database->withConnection<std::expected<NodeConfigRecord, NodeConfigFailure>>(
                loop,
                [nodeId, version, contentJson = std::move(contentJson), contentHash = std::move(contentHash), status = std::move(status), message = std::move(message),
                 tenant = m_tenant](DatabaseConnection &connection) -> std::expected<NodeConfigRecord, NodeConfigFailure>
                {
                    for (int attempt = 0; attempt < kConfigWriteAttempts; ++attempt)
                    {
                        try
                        {
                            Sql::executeOrThrowVoid(connection, "BEGIN");
                            const std::optional<std::int64_t> nodeTenant = findNodeTenantOnConnection(connection, nodeId, tenant);
                            if (!nodeTenant.has_value())
                            {
                                rollbackQuietly(connection);
                                return std::unexpected(NodeConfigFailure::NotFound);
                            }
                            const System::TenantScope ownerScope{*nodeTenant, false};
                            const auto                existing = Sql::executeOrThrow(connection, "SELECT node_id FROM biz_node_config WHERE node_id = ? AND tenant_id = ?",
                                                                                     std::vector<DatabaseValue>{nodeId, *nodeTenant});
                            if (!existing->next())
                            {
                                rollbackQuietly(connection);
                                return std::unexpected(NodeConfigFailure::NotFound);
                            }
                            const std::string now = Common::nowDateTimeUtc();
                            if (contentJson.empty())
                            {
                                // 失败上报通常不带内容：保留上次上报正文与摘要，只推进状态与说明
                                Sql::executeOrThrowVoid(connection,
                                                        "UPDATE biz_node_config SET reported_version = ?, reported_status = ?, reported_message = ?, reported_at = ? "
                                                        "WHERE node_id = ? AND tenant_id = ?",
                                                        std::vector<DatabaseValue>{version, status, message, now, nodeId, *nodeTenant});
                            } else
                            {
                                Sql::executeOrThrowVoid(connection,
                                                        "UPDATE biz_node_config SET reported_json = ?, reported_version = ?, reported_hash = ?, reported_status = ?, "
                                                        "reported_message = ?, reported_at = ? WHERE node_id = ? AND tenant_id = ?",
                                                        std::vector<DatabaseValue>{contentJson, version, contentHash, status, message, now, nodeId, *nodeTenant});
                            }
                            const std::optional<NodeConfigRecord> record = findConfigOnConnection(connection, nodeId, ownerScope);
                            if (!record.has_value())
                            {
                                rollbackQuietly(connection);
                                throw Common::DatabaseException("配置上报后读回失败：节点 " + std::to_string(nodeId));
                            }
                            Sql::executeOrThrowVoid(connection, "COMMIT");
                            return *record;
                        } catch (const Common::DatabaseException &failure)
                        {
                            rollbackQuietly(connection);
                            if (!Sql::isRetryableSqlFailure(failure.what()))
                            {
                                throw;
                            }
                            if (attempt + 1 < kConfigWriteAttempts)
                            {
                                // 工作线程上的短暂退避，避开另一条写入刚拿到的写锁；不能在事件循环线程上睡
                                std::this_thread::sleep_for(kConfigWriteBackoff);
                                continue;
                            }
                            // 上报没有版本前置条件可以收口，重试用尽时把最后一次并发失败交回上层
                            throw;
                        } catch (...)
                        {
                            rollbackQuietly(connection);
                            throw;
                        }
                    }
                    // 循环体要么返回，要么在最后一次失败后抛出；这里只兜住理论上的落空
                    throw Common::DatabaseException("配置上报重试未产生结果：节点 " + std::to_string(nodeId));
                });
    }

    AsynGyanis::Core::Task<Common::PageRows<NodeConfigRevision>> NodeConfigRepository::listRevisions(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId,
                                                                                                     const Common::PageParameters &page)
    {
        co_return co_await m_database->withConnection<Common::PageRows<NodeConfigRevision>>(
                loop,
                [nodeId, page, tenant = m_tenant](DatabaseConnection &connection) -> Common::PageRows<NodeConfigRevision>
                {
                    std::vector<DatabaseValue> parameters{nodeId};
                    const std::string          clause = tenantClause(tenant, parameters);
                    const auto count = Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM biz_node_config_revision WHERE node_id = ?" + clause, parameters);
                    Common::PageRows<NodeConfigRevision> pageRows;
                    if (count->next())
                    {
                        pageRows.total = Sql::integer(*count, "total");
                    }
                    if (pageRows.total == 0)
                    {
                        return pageRows;
                    }
                    // LIMIT 占位符排在筛选条件之后，参数必须按语句中的出现顺序追加
                    parameters.emplace_back(static_cast<std::int64_t>(page.pageSize));
                    parameters.emplace_back(static_cast<std::int64_t>(page.offset()));
                    const auto rows = Sql::executeOrThrow(connection,
                                                          std::string{"SELECT "} + kRevisionColumns + " FROM biz_node_config_revision WHERE node_id = ?" + clause +
                                                                  " ORDER BY version DESC LIMIT ? OFFSET ?",
                                                          parameters);
                    while (rows->next())
                    {
                        pageRows.rows.push_back(readRevision(*rows));
                    }
                    return pageRows;
                });
    }

    AsynGyanis::Core::Task<std::optional<NodeConfigRevision>> NodeConfigRepository::findRevision(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId,
                                                                                                 const std::int64_t revisionId)
    {
        co_return co_await m_database->withConnection<std::optional<NodeConfigRevision>>(
                loop,
                [nodeId, revisionId, tenant = m_tenant](DatabaseConnection &connection) -> std::optional<NodeConfigRevision>
                {
                    std::vector<DatabaseValue> parameters{revisionId, nodeId};
                    const std::string          clause = tenantClause(tenant, parameters);
                    const auto                 rows   = Sql::executeOrThrow(
                            connection, std::string{"SELECT "} + kRevisionColumns + " FROM biz_node_config_revision WHERE id = ? AND node_id = ?" + clause, parameters);
                    if (!rows->next())
                    {
                        return std::nullopt;
                    }
                    return readRevision(*rows);
                });
    }
} // namespace EdgePulse::Device
