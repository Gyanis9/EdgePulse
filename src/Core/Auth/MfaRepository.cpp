#include "Core/Auth/MfaRepository.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Exception/EdgePulseException.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::Auth
{
    namespace
    {
        constexpr const char *kMfaColumns =
                "user_id, tenant_id, secret_ciphertext, secret_nonce, key_version, last_used_step, recovery_codes_json, enabled, created_at, updated_at";
        constexpr int kRecoveryCasAttempts = 3; ///< CAS 并发冲突时的有界重试次数

        /**
         * @brief 把恢复码哈希数组序列化成 JSON
         * @param hashes SHA-256 十六进制
         * @return std::string JSON 数组文本
         */
        [[nodiscard]] std::string serializeRecoveryCodes(const std::vector<std::string> &hashes)
        {
            AsynGyanis::Base::ConfigValue array = AsynGyanis::Base::ConfigValue::array();
            for (const std::string &hash: hashes)
            {
                array.push_back(hash);
            }
            return array.dump();
        }

        /**
         * @brief 解析恢复码哈希 JSON
         * @param text 库里的 JSON 文本
         * @return std::vector<std::string> 字符串项；格式损坏时为空
         */
        [[nodiscard]] std::vector<std::string> parseRecoveryCodes(const std::string_view text)
        {
            std::vector<std::string>                           hashes;
            const std::optional<AsynGyanis::Base::ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(text);
            if (!parsed.has_value() || !parsed->is_array())
            {
                return hashes;
            }
            for (const AsynGyanis::Base::ConfigValue &item: *parsed)
            {
                if (item.is_string())
                {
                    hashes.push_back(item.get<std::string>());
                }
            }
            return hashes;
        }

        /**
         * @brief 从结果集读一条记录
         * @param result 已定位到目标行的结果集
         * @return MfaRecord 记录
         */
        [[nodiscard]] MfaRecord readRecord(const AsynGyanis::Database::DatabaseResult &result)
        {
            MfaRecord record;
            record.userId           = Sql::integer(result, "user_id");
            record.tenantId         = Sql::integer(result, "tenant_id");
            record.secretCiphertext = Sql::text(result, "secret_ciphertext");
            record.secretNonce      = Sql::text(result, "secret_nonce");
            record.keyVersion       = static_cast<int>(Sql::integer(result, "key_version"));
            record.lastUsedStep     = Sql::integer(result, "last_used_step");
            record.recoveryHashes   = parseRecoveryCodes(Sql::text(result, "recovery_codes_json"));
            record.enabled          = Sql::integer(result, "enabled") != 0;
            record.createdAt        = Sql::dateTimeText(result, "created_at");
            record.updatedAt        = Sql::dateTimeText(result, "updated_at");
            return record;
        }
    } // namespace

    MfaRepository::MfaRepository(std::shared_ptr<Common::Database> database, const System::TenantScope tenant) : m_database(std::move(database)), m_tenant(tenant)
    {
    }

    std::shared_ptr<MfaRepository> MfaRepository::scoped(const System::TenantScope tenant) const
    {
        return std::make_shared<MfaRepository>(m_database, tenant);
    }

    AsynGyanis::Core::Task<std::optional<MfaRecord>> MfaRepository::findByUser(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        co_return co_await m_database->withConnection<std::optional<MfaRecord>>(
                loop,
                [userId, tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> std::optional<MfaRecord>
                {
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters{userId};
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto result = Sql::executeOrThrow(connection, std::string{"SELECT "} + kMfaColumns + " FROM sys_user_mfa WHERE user_id = ?" + clause, parameters);
                    if (!result->next())
                    {
                        return std::nullopt;
                    }
                    return readRecord(*result);
                });
    }

    AsynGyanis::Core::Task<> MfaRepository::upsertSecret(AsynGyanis::Core::EventLoop &loop, MfaRecord record)
    {
        static_cast<void>(co_await m_database->withConnection<bool>(
                loop,
                [record = std::move(record), tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                {
                    // 仓储视图比服务层更靠内，越界写入必须在落库前拒绝
                    if (!tenant.includes(record.tenantId))
                    {
                        throw Common::DatabaseException("两步验证归属租户与当前租户边界不一致");
                    }

                    std::vector<AsynGyanis::Database::DatabaseValue> parameters{record.secretCiphertext, record.secretNonce, record.keyVersion, record.updatedAt, record.userId};
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(record.tenantId);
                    }
                    // 只覆盖未启用行；已启用行保持原秘密，避免并发开始设置把启用状态静默降级
                    const auto updated = Sql::executeOrThrow(
                            connection,
                            "UPDATE sys_user_mfa SET secret_ciphertext = ?, secret_nonce = ?, key_version = ?, last_used_step = 0, recovery_codes_json = '[]', enabled = 0, "
                            "updated_at = ? WHERE user_id = ? AND enabled = 0" +
                                    clause,
                            parameters);
                    if (updated->affectedRowCount() > 0)
                    {
                        return true;
                    }

                    const std::vector<AsynGyanis::Database::DatabaseValue> insertParameters{record.userId,     record.tenantId,  record.secretCiphertext, record.secretNonce,
                                                                                            record.keyVersion, record.createdAt, record.updatedAt};
                    Sql::executeOrThrowVoid(connection,
                                            "INSERT INTO sys_user_mfa (user_id, tenant_id, secret_ciphertext, secret_nonce, key_version, last_used_step, recovery_codes_json, "
                                            "enabled, created_at, updated_at) VALUES (?, ?, ?, ?, ?, 0, '[]', 0, ?, ?)",
                                            insertParameters);
                    return true;
                }));
        co_return;
    }

    AsynGyanis::Core::Task<bool> MfaRepository::enable(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId, std::string recoveryCodesJson, std::string now)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [userId, recoveryCodesJson = std::move(recoveryCodesJson), now = std::move(now), tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                {
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters{recoveryCodesJson, now, userId};
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    // 只有未启用行可以转入启用；并发启用时后到的一方拿到 false
                    const auto result = Sql::executeOrThrow(
                            connection, "UPDATE sys_user_mfa SET enabled = 1, recovery_codes_json = ?, updated_at = ? WHERE user_id = ? AND enabled = 0" + clause, parameters);
                    return result->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<bool> MfaRepository::remove(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [userId, tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                                                            {
                                                                std::vector<AsynGyanis::Database::DatabaseValue> parameters{userId};
                                                                std::string                                      clause;
                                                                if (!tenant.allTenants)
                                                                {
                                                                    clause = " AND tenant_id = ?";
                                                                    parameters.emplace_back(tenant.tenantId);
                                                                }
                                                                const auto result =
                                                                        Sql::executeOrThrow(connection, "DELETE FROM sys_user_mfa WHERE user_id = ?" + clause, parameters);
                                                                return result->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<bool> MfaRepository::consumeStep(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId, const std::int64_t step, std::string now)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [userId, step, now = std::move(now), tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                {
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters{step, now, userId, step};
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    // 条件里再比一次步号：两个并发请求同时读到旧步号时只有先到的能写入
                    const auto result = Sql::executeOrThrow(
                            connection, "UPDATE sys_user_mfa SET last_used_step = ?, updated_at = ? WHERE user_id = ? AND enabled = 1 AND last_used_step < ?" + clause, parameters);
                    return result->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<bool> MfaRepository::consumeRecoveryCode(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId, std::string hash, std::string now)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [userId, hash = std::move(hash), now = std::move(now), tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                {
                    std::vector<AsynGyanis::Database::DatabaseValue> selectParameters{userId};
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        selectParameters.emplace_back(tenant.tenantId);
                    }

                    for (int attempt = 0; attempt < kRecoveryCasAttempts; ++attempt)
                    {
                        std::string previousJson;
                        {
                            const auto rows =
                                    Sql::executeOrThrow(connection, std::string{"SELECT recovery_codes_json FROM sys_user_mfa WHERE user_id = ?"} + clause, selectParameters);
                            if (!rows->next())
                            {
                                return false;
                            }
                            previousJson = Sql::text(*rows, "recovery_codes_json");
                        }

                        std::vector<std::string> hashes  = parseRecoveryCodes(previousJson);
                        const auto               matched = std::find(hashes.begin(), hashes.end(), hash);
                        if (matched == hashes.end())
                        {
                            return false;
                        }
                        hashes.erase(matched);

                        std::vector<AsynGyanis::Database::DatabaseValue> updateParameters{serializeRecoveryCodes(hashes), now, userId, previousJson};
                        std::string                                      updateClause;
                        if (!tenant.allTenants)
                        {
                            updateClause = " AND tenant_id = ?";
                            updateParameters.emplace_back(tenant.tenantId);
                        }
                        // 旧 JSON 文本作为乐观锁：另一枚恢复码恰好并发消费时本轮 CAS 失败，重读后再试
                        const auto result = Sql::executeOrThrow(
                                connection, "UPDATE sys_user_mfa SET recovery_codes_json = ?, updated_at = ? WHERE user_id = ? AND recovery_codes_json = ?" + updateClause,
                                updateParameters);
                        if (result->affectedRowCount() > 0)
                        {
                            return true;
                        }
                    }
                    return false;
                });
    }

    AsynGyanis::Core::Task<bool> MfaRepository::updateRecoveryCodes(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId, std::string recoveryCodesJson, std::string now)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [userId, recoveryCodesJson = std::move(recoveryCodesJson), now = std::move(now), tenant = m_tenant](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                {
                    std::vector<AsynGyanis::Database::DatabaseValue> parameters{recoveryCodesJson, now, userId};
                    std::string                                      clause;
                    if (!tenant.allTenants)
                    {
                        clause = " AND tenant_id = ?";
                        parameters.emplace_back(tenant.tenantId);
                    }
                    const auto result = Sql::executeOrThrow(connection, "UPDATE sys_user_mfa SET recovery_codes_json = ?, updated_at = ? WHERE user_id = ?" + clause, parameters);
                    return result->affectedRowCount() > 0;
                });
    }
} // namespace EdgePulse::Auth
