#include "Core/Auth/ApiTokenRepository.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Exception/EdgePulseException.h"
#include "Base/Runtime/IdGenerator.h"

#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Auth
{
    namespace
    {
        constexpr const char *kTokenColumns = "id, tenant_id, owner_user_id, name, token_prefix, token_hash, previous_hash, previous_valid_until, scopes_json, expires_at, "
                                              "last_used_at, revoked_at, created_by, created_at";

        [[nodiscard]] std::string serializeScopes(const std::vector<std::string> &scopes)
        {
            AsynGyanis::Base::ConfigValue array = AsynGyanis::Base::ConfigValue::array();
            for (const std::string &scope: scopes)
            {
                array.push_back(scope);
            }
            return array.dump();
        }

        [[nodiscard]] std::vector<std::string> deserializeScopes(const std::string_view text)
        {
            std::vector<std::string>                           scopes;
            const std::optional<AsynGyanis::Base::ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(text);
            if (!parsed.has_value() || !parsed->is_array())
            {
                return scopes;
            }
            for (const AsynGyanis::Base::ConfigValue &scope: *parsed)
            {
                if (scope.is_string())
                {
                    scopes.push_back(scope.get<std::string>());
                }
            }
            return scopes;
        }

        [[nodiscard]] ApiTokenRecord readToken(const AsynGyanis::Database::DatabaseResult &result)
        {
            ApiTokenRecord record;
            record.id                 = Sql::integer(result, "id");
            record.tenantId           = Sql::integer(result, "tenant_id");
            record.ownerUserId        = Sql::integer(result, "owner_user_id");
            record.name               = Sql::text(result, "name");
            record.tokenPrefix        = Sql::text(result, "token_prefix");
            record.tokenHash          = Sql::text(result, "token_hash");
            record.previousHash       = Sql::text(result, "previous_hash");
            record.previousValidUntil = Sql::dateTimeText(result, "previous_valid_until");
            record.scopes             = deserializeScopes(Sql::text(result, "scopes_json"));
            record.expiresAt          = Sql::dateTimeText(result, "expires_at");
            record.lastUsedAt         = Sql::dateTimeText(result, "last_used_at");
            record.revokedAt          = Sql::dateTimeText(result, "revoked_at");
            record.createdBy          = Sql::integer(result, "created_by");
            record.createdAt          = Sql::dateTimeText(result, "created_at");
            return record;
        }
    } // namespace

    ApiTokenRepository::ApiTokenRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant) : m_database(std::move(database)), m_tenant(tenant)
    {
    }

    std::shared_ptr<ApiTokenRepository> ApiTokenRepository::scoped(const System::TenantScope tenant) const
    {
        return std::make_shared<ApiTokenRepository>(m_database, tenant);
    }

    AsynGyanis::Core::Task<ApiTokenRecord> ApiTokenRepository::create(AsynGyanis::Core::EventLoop &loop, ApiTokenRecord record)
    {
        co_return co_await m_database->withConnection<ApiTokenRecord>(
                loop,
                [record = std::move(record), tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) mutable -> ApiTokenRecord
                {
                    // 仓储视图比服务层更靠内，越界写入必须在落库前拒绝
                    if (!tenant.includes(record.tenantId))
                    {
                        throw Common::DatabaseException("令牌归属租户与当前租户边界不一致");
                    }
                    record.id = Common::nextIdentifier();
                    const std::vector<AsynGyanis::Database::DatabaseValue> parameters{
                            record.id,        record.tenantId,  record.ownerUserId, record.name, record.tokenPrefix, record.tokenHash, serializeScopes(record.scopes),
                            record.expiresAt, record.createdBy, record.createdAt};
                    Sql::executeOrThrowVoid(
                            connection,
                            "INSERT INTO sys_api_token (id, tenant_id, owner_user_id, name, token_prefix, token_hash, scopes_json, expires_at, created_by, created_at) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                            parameters);
                    return record;
                });
    }

    AsynGyanis::Core::Task<std::optional<ApiTokenRecord>> ApiTokenRepository::findById(AsynGyanis::Core::EventLoop &loop, const std::int64_t id)
    {
        co_return co_await m_database->withConnection<std::optional<ApiTokenRecord>>(
                loop,
                [id, tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> std::optional<ApiTokenRecord>
                {
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters{id};
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto result = Sql::executeOrThrow(connection, std::string{"SELECT "} + kTokenColumns + " FROM sys_api_token WHERE id = ?" + clause, parameters);
                    if (!result->next())
                    {
                        return std::nullopt;
                    }
                    return readToken(*result);
                });
    }

    AsynGyanis::Core::Task<std::optional<ApiTokenRecord>> ApiTokenRepository::findByHash(AsynGyanis::Core::EventLoop &loop, std::string hash, std::string now)
    {
        co_return co_await m_database->withConnection<std::optional<ApiTokenRecord>>(
                loop,
                [hash = std::move(hash), now = std::move(now), tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> std::optional<ApiTokenRecord>
                {
                    std::string                                      clause;
                    std::vector<AsynGyanis::Database::DatabaseValue> activeParameters{hash, now};
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        activeParameters.emplace_back(tenant.tenantId);
                    }
                    const auto active = Sql::executeOrThrow(connection,
                                                            std::string{"SELECT "} + kTokenColumns +
                                                                    " FROM sys_api_token WHERE token_hash = ? AND revoked_at IS NULL AND expires_at > ?" + clause + " LIMIT 1",
                                                            activeParameters);
                    if (active->next())
                    {
                        return readToken(*active);
                    }

                    std::vector<AsynGyanis::Database::DatabaseValue> previousParameters{hash, now, now};
                    if (!tenant.allTenants)
                    {
                        previousParameters.emplace_back(tenant.tenantId);
                    }
                    const auto previous = Sql::executeOrThrow(
                            connection,
                            std::string{"SELECT "} + kTokenColumns +
                                    " FROM sys_api_token WHERE previous_hash = ? AND previous_valid_until IS NOT NULL AND previous_valid_until > ? AND revoked_at IS NULL AND "
                                    "expires_at > ?" +
                                    clause + " LIMIT 1",
                            previousParameters);
                    if (!previous->next())
                    {
                        return std::nullopt;
                    }
                    return readToken(*previous);
                });
    }

    AsynGyanis::Core::Task<std::vector<ApiTokenRecord>> ApiTokenRepository::list(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await m_database->withConnection<std::vector<ApiTokenRecord>>(
                loop,
                [tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> std::vector<ApiTokenRecord>
                {
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters;
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " WHERE tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto rows = Sql::executeOrThrow(
                            connection, std::string{"SELECT "} + kTokenColumns + " FROM sys_api_token" + clause + " ORDER BY created_at DESC, id DESC", parameters);
                    std::vector<ApiTokenRecord> records;
                    while (rows->next())
                    {
                        records.push_back(readToken(*rows));
                    }
                    return records;
                });
    }

    AsynGyanis::Core::Task<bool> ApiTokenRepository::revoke(AsynGyanis::Core::EventLoop &loop, const std::int64_t id, std::string revokedAt)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [id, revokedAt = std::move(revokedAt), tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                {
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters{revokedAt, id};
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    // 已吊销的行不再改写，保留第一次吊销的时间用于诊断
                    const auto result = Sql::executeOrThrow(connection, "UPDATE sys_api_token SET revoked_at = ? WHERE id = ? AND revoked_at IS NULL" + clause, parameters);
                    return result->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<bool> ApiTokenRepository::replaceHash(AsynGyanis::Core::EventLoop &loop, const std::int64_t id, std::string expectedHash, std::string newHash,
                                                                 std::string newPrefix, std::string previousValidUntil, std::string now)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [id, expectedHash = std::move(expectedHash), newHash = std::move(newHash), newPrefix = std::move(newPrefix), previousValidUntil = std::move(previousValidUntil),
                 now = std::move(now), tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                {
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters{newHash, newPrefix, expectedHash, previousValidUntil, id, expectedHash, now};
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    // 期望哈希作为乐观锁；到期条件与期望哈希一起放在 UPDATE 里，堵住读出后过期的竞态
                    const auto result = Sql::executeOrThrow(connection,
                                                            "UPDATE sys_api_token SET token_hash = ?, token_prefix = ?, previous_hash = ?, previous_valid_until = ? WHERE id = ? "
                                                            "AND token_hash = ? AND revoked_at IS NULL AND expires_at > ?" +
                                                                    clause,
                                                            parameters);
                    return result->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<bool> ApiTokenRepository::touchLastUsed(AsynGyanis::Core::EventLoop &loop, const std::int64_t tenantId, const std::int64_t id, std::string usedAt,
                                                                   std::string cutoff)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [tenantId, id, usedAt = std::move(usedAt), cutoff = std::move(cutoff)](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                {
                    const std::vector<AsynGyanis::Database::DatabaseValue> parameters{usedAt, id, tenantId, cutoff};
                    // 条件更新保证同一令牌一分钟内最多写一次，且不会覆盖并发请求刚写入的时间
                    const auto result = Sql::executeOrThrow(
                            connection,
                            "UPDATE sys_api_token SET last_used_at = ? WHERE id = ? AND tenant_id = ? AND revoked_at IS NULL AND (last_used_at IS NULL OR last_used_at < ?)",
                            parameters);
                    return result->affectedRowCount() > 0;
                });
    }
} // namespace EdgePulse::Auth
