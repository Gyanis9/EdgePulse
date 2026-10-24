#include "Core/Device/AgentCertificateRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Device
{
    namespace
    {
        constexpr const char *kCertificateColumns = "id, tenant_id, node_id, serial, fingerprint, subject, not_before, not_after, status, issued_at, revoked_at";

        [[nodiscard]] AgentCertificateRecord readCertificate(const AsynGyanis::Database::DatabaseResult &result)
        {
            AgentCertificateRecord record;
            record.id          = Sql::integer(result, "id");
            record.tenantId    = Sql::integer(result, "tenant_id");
            record.nodeId      = Sql::integer(result, "node_id");
            record.serial      = Sql::text(result, "serial");
            record.fingerprint = Sql::text(result, "fingerprint");
            record.subject     = Sql::text(result, "subject");
            record.notBefore   = Sql::dateTimeText(result, "not_before");
            record.notAfter    = Sql::dateTimeText(result, "not_after");
            record.status      = Sql::text(result, "status");
            record.issuedAt    = Sql::dateTimeText(result, "issued_at");
            record.revokedAt   = Sql::dateTimeText(result, "revoked_at");
            return record;
        }
    } // namespace

    AgentCertificateRepository::AgentCertificateRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant) :
        m_database(std::move(database)), m_tenant(tenant)
    {
    }

    std::shared_ptr<AgentCertificateRepository> AgentCertificateRepository::scoped(const System::TenantScope tenant) const
    {
        return std::make_shared<AgentCertificateRepository>(m_database, tenant);
    }

    AsynGyanis::Core::Task<std::vector<AgentCertificateRecord>> AgentCertificateRepository::listByNode(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        co_return co_await m_database->withConnection<std::vector<AgentCertificateRecord>>(
                loop,
                [nodeId, tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> std::vector<AgentCertificateRecord>
                {
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters{nodeId};
                    std::string                                      filter = " WHERE node_id = ?";
                    if (!tenant.allTenants)
                    {
                        filter += " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(
                            connection, std::string{"SELECT "} + kCertificateColumns + " FROM biz_agent_certificate" + filter + " ORDER BY issued_at DESC, id DESC", parameters);
                    std::vector<AgentCertificateRecord> records;
                    while (rows->next())
                    {
                        records.push_back(readCertificate(*rows));
                    }
                    return records;
                });
    }
} // namespace EdgePulse::Device
