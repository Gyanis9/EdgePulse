#include "Core/Device/EnrollmentRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Exception/EdgePulseException.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"
#include "Database/Common/DatabaseValue.h"

#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Device
{
    namespace
    {
        constexpr const char *kEnrollmentColumns = "id, tenant_id, node_name, action, expires_at, used_at, revoked_at, created_by, created_at";

        [[nodiscard]] EnrollmentTokenRecord readEnrollment(const AsynGyanis::Database::DatabaseResult &result)
        {
            EnrollmentTokenRecord record;
            record.id        = Sql::integer(result, "id");
            record.tenantId  = Sql::integer(result, "tenant_id");
            record.nodeName  = Sql::text(result, "node_name");
            record.action    = Sql::text(result, "action");
            record.expiresAt = Sql::dateTimeText(result, "expires_at");
            record.usedAt    = Sql::dateTimeText(result, "used_at");
            record.revokedAt = Sql::dateTimeText(result, "revoked_at");
            record.createdBy = Sql::integer(result, "created_by");
            record.createdAt = Sql::dateTimeText(result, "created_at");
            return record;
        }
    } // namespace

    EnrollmentRepository::EnrollmentRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant) : m_database(std::move(database)), m_tenant(tenant)
    {
    }

    std::shared_ptr<EnrollmentRepository> EnrollmentRepository::scoped(const System::TenantScope tenant) const
    {
        return std::make_shared<EnrollmentRepository>(m_database, tenant);
    }

    AsynGyanis::Core::Task<EnrollmentTokenRecord> EnrollmentRepository::create(AsynGyanis::Core::EventLoop &loop, EnrollmentTokenRecord record, std::string tokenHash)
    {
        co_return co_await m_database->withConnection<EnrollmentTokenRecord>(
                loop,
                [record = std::move(record), tokenHash = std::move(tokenHash),
                 tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) mutable -> EnrollmentTokenRecord
                {
                    // 仓储视图比服务层更靠内，越界写入必须在这里拒绝，而不是落成一条别家租户的令牌
                    if (!tenant.includes(record.tenantId))
                    {
                        throw Common::DatabaseException("令牌归属租户与当前租户边界不一致");
                    }
                    record.id = Common::nextIdentifier();
                    const std::vector<AsynGyanis::Database::DatabaseValue> parameters{record.id, record.tenantId,  record.nodeName,  record.action,
                                                                                      tokenHash, record.expiresAt, record.createdBy, record.createdAt};
                    Sql::executeOrThrowVoid(connection,
                                            "INSERT INTO biz_agent_enrollment (id, tenant_id, node_name, action, token_hash, expires_at, created_by, created_at) "
                                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                                            parameters);
                    return record;
                });
    }

    AsynGyanis::Core::Task<std::vector<EnrollmentTokenRecord>> EnrollmentRepository::list(AsynGyanis::Core::EventLoop &loop, std::string status)
    {
        co_return co_await m_database->withConnection<std::vector<EnrollmentTokenRecord>>(
                loop,
                [status = std::move(status), tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> std::vector<EnrollmentTokenRecord>
                {
                    const std::string                                now = Common::nowDateTimeUtc();
                    std::string                                      filter;
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters;
                    // 状态由使用、吊销与到期三个时间列派生；未知状态不能退化成“全部”
                    if (status == "active")
                    {
                        filter = " AND used_at IS NULL AND revoked_at IS NULL AND expires_at > ?";
                        parameters.emplace_back(now);
                    } else if (status == "used")
                    {
                        filter = " AND used_at IS NOT NULL";
                    } else if (status == "expired")
                    {
                        filter = " AND used_at IS NULL AND revoked_at IS NULL AND expires_at <= ?";
                        parameters.emplace_back(now);
                    } else if (status == "revoked")
                    {
                        filter = " AND revoked_at IS NOT NULL";
                    } else if (!status.empty())
                    {
                        filter = " AND 1 = 0";
                    }
                    if (!tenant.allTenants)
                    {
                        filter += " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(
                            connection, std::string{"SELECT "} + kEnrollmentColumns + " FROM biz_agent_enrollment WHERE 1 = 1" + filter + " ORDER BY created_at DESC, id DESC",
                            parameters);
                    std::vector<EnrollmentTokenRecord> records;
                    while (rows->next())
                    {
                        records.push_back(readEnrollment(*rows));
                    }
                    return records;
                });
    }

    AsynGyanis::Core::Task<bool> EnrollmentRepository::revoke(AsynGyanis::Core::EventLoop &loop, const std::int64_t tokenId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [tokenId, tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                {
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters{Common::nowDateTimeUtc(), tokenId};
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    // 已使用或已吊销的令牌不再改写：历史状态比“再吊销一次”更有诊断价值
                    const auto result = Sql::executeOrThrow(
                            connection, "UPDATE biz_agent_enrollment SET revoked_at = ? WHERE id = ? AND used_at IS NULL AND revoked_at IS NULL" + clause, parameters);
                    return result->affectedRowCount() > 0;
                });
    }
} // namespace EdgePulse::Device
