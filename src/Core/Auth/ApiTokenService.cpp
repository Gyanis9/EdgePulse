#include "Core/Auth/ApiTokenService.h"

#include "Base/Api/ErrorCatalog.h"
#include "Base/Log/LogMacros.h"
#include "Base/Runtime/TimeFormat.h"
#include "Base/Security/RandomToken.h"
#include "Core/Crypto/Digest.h"
#include "Core/System/TenantRepository.h"
#include "Core/System/UserRepository.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace EdgePulse::Auth
{
    namespace
    {
        constexpr std::string_view     kTokenPrefix           = "ep_api_"; ///< 明文令牌的固定前缀
        constexpr std::size_t          kTokenPrefixLength     = 12;        ///< 落库的辨识前缀长度
        constexpr int                  kDefaultExpiresSeconds = 2592000;   ///< 默认 30 天
        constexpr int                  kMinimumExpiresSeconds = 300;       ///< 最短 5 分钟
        constexpr int                  kMaximumExpiresSeconds = 31536000;  ///< 最长 365 天
        constexpr std::chrono::seconds kPreviousOverlapSeconds{3600};      ///< 轮换旧哈希的重叠窗口
        constexpr std::chrono::seconds kLastUsedInterval{60};              ///< 最近使用时间的最小写入间隔

        [[nodiscard]] std::string newPlaintext()
        {
            return std::string{kTokenPrefix} + Common::randomTokenHex(32);
        }

        [[nodiscard]] std::string prefixOf(const std::string &plaintext)
        {
            return plaintext.substr(0, kTokenPrefixLength);
        }

        [[nodiscard]] AuthError badRequest(std::string message)
        {
            return AuthError{Common::kCodeBadRequest, Common::kCodeBadRequest, std::move(message)};
        }

        [[nodiscard]] AuthError unauthorized(std::string message)
        {
            return AuthError{Common::kCodeUnauthorized, Common::kCodeUnauthorized, std::move(message)};
        }

        [[nodiscard]] AuthError conflict(std::string message)
        {
            return AuthError{Common::kCodeConflict, Common::kCodeConflict, std::move(message)};
        }
    } // namespace

    ApiTokenService::ApiTokenService(std::shared_ptr<Common::Database> database, std::shared_ptr<System::UserRepository> users, std::shared_ptr<System::TenantRepository> tenants) :
        m_users(std::move(users)), m_tenants(std::move(tenants)), m_tokens(std::move(database))
    {
    }

    AsynGyanis::Core::Task<std::expected<ApiTokenCreated, AuthError>> ApiTokenService::create(AsynGyanis::Core::EventLoop &loop, const SessionRecord &session, std::string name,
                                                                                              std::vector<std::string> scopes, const int expiresInSeconds)
    {
        if (name.empty() || name.size() > 64)
        {
            co_return std::unexpected(badRequest("令牌名称长度必须在 1 到 64 个字符之间"));
        }
        if (scopes.empty())
        {
            co_return std::unexpected(badRequest("至少选择一个权限范围"));
        }
        std::unordered_set<std::string> uniqueScopes;
        for (const std::string &scope: scopes)
        {
            if (scope.empty())
            {
                co_return std::unexpected(badRequest("权限范围不能为空"));
            }
            if (!uniqueScopes.insert(scope).second)
            {
                co_return std::unexpected(badRequest("权限范围不能重复：" + scope));
            }
        }
        const int effectiveSeconds = expiresInSeconds == 0 ? kDefaultExpiresSeconds : expiresInSeconds;
        if (effectiveSeconds < kMinimumExpiresSeconds || effectiveSeconds > kMaximumExpiresSeconds)
        {
            co_return std::unexpected(badRequest("有效期必须在 300 到 31536000 秒之间"));
        }

        const std::optional<System::UserRecord> owner = co_await m_users->scoped({session.tenantId, false})->findById(loop, session.userId);
        if (!owner.has_value() || owner->status != 1)
        {
            co_return std::unexpected(unauthorized("令牌所属用户不存在或已停用"));
        }
        const std::vector<std::string>        permissions = co_await m_users->permissionsFor(loop, owner->id);
        const std::unordered_set<std::string> allowed(permissions.begin(), permissions.end());
        for (const std::string &scope: scopes)
        {
            if (!allowed.contains(scope))
            {
                co_return std::unexpected(badRequest("权限范围不在当前用户权限内：" + scope));
            }
        }

        const std::string plaintext = newPlaintext();
        ApiTokenRecord    record;
        record.tenantId             = session.tenantId;
        record.ownerUserId          = owner->id;
        record.name                 = std::move(name);
        record.tokenPrefix          = prefixOf(plaintext);
        record.tokenHash            = AsynGyanis::Core::Digest::sha256Hex(plaintext);
        record.scopes               = std::move(scopes);
        record.expiresAt            = Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::seconds{effectiveSeconds});
        record.createdBy            = owner->id;
        record.createdAt            = Common::nowDateTimeUtc();
        const ApiTokenRecord stored = co_await m_tokens.scoped({session.tenantId, false})->create(loop, std::move(record));
        co_return ApiTokenCreated{stored, plaintext};
    }

    AsynGyanis::Core::Task<std::vector<ApiTokenRecord>> ApiTokenService::list(AsynGyanis::Core::EventLoop &loop, const SessionRecord &session)
    {
        // 平台管理员也只看当前租户，跨租户管理不在令牌接口的职责内
        co_return co_await m_tokens.scoped({session.tenantId, false})->list(loop);
    }

    AsynGyanis::Core::Task<bool> ApiTokenService::revoke(AsynGyanis::Core::EventLoop &loop, const std::int64_t tenantId, const std::int64_t id)
    {
        co_return co_await m_tokens.scoped({tenantId, false})->revoke(loop, id, Common::nowDateTimeUtc());
    }

    AsynGyanis::Core::Task<std::expected<ApiTokenCreated, AuthError>> ApiTokenService::rotate(AsynGyanis::Core::EventLoop &loop, const SessionRecord &session,
                                                                                              const std::int64_t id)
    {
        const std::optional<ApiTokenRecord> existing = co_await m_tokens.scoped({session.tenantId, false})->findById(loop, id);
        if (!existing.has_value())
        {
            co_return std::unexpected(AuthError{Common::kCodeNotFound, Common::kCodeNotFound, "令牌不存在或不属于当前租户"});
        }
        if (!existing->revokedAt.empty())
        {
            co_return std::unexpected(conflict("令牌已吊销，无法轮换"));
        }
        const std::string now = Common::nowDateTimeUtc();
        if (existing->expiresAt <= now)
        {
            co_return std::unexpected(conflict("令牌已过期，无法轮换"));
        }

        const std::string plaintext          = newPlaintext();
        const std::string newHash            = AsynGyanis::Core::Digest::sha256Hex(plaintext);
        const std::string previousValidUntil = Common::formatDateTimeUtc(std::chrono::system_clock::now() + kPreviousOverlapSeconds);
        const bool rotated = co_await m_tokens.scoped({session.tenantId, false})->replaceHash(loop, id, existing->tokenHash, newHash, prefixOf(plaintext), previousValidUntil);
        if (!rotated)
        {
            co_return std::unexpected(conflict("令牌已被并发轮换或吊销，请刷新后重试"));
        }

        ApiTokenRecord record     = *existing;
        record.tokenPrefix        = prefixOf(plaintext);
        record.tokenHash          = newHash;
        record.previousHash       = existing->tokenHash;
        record.previousValidUntil = previousValidUntil;
        co_return ApiTokenCreated{std::move(record), plaintext};
    }

    AsynGyanis::Core::Task<std::optional<SessionRecord>> ApiTokenService::authenticate(AsynGyanis::Core::EventLoop &loop, const std::string_view token)
    {
        // 会话令牌不带此前缀，先做形状判断，避免每个失效 cookie 都去查一次令牌表
        if (token.size() <= kTokenPrefix.size() || token.substr(0, kTokenPrefix.size()) != kTokenPrefix)
        {
            co_return std::nullopt;
        }
        const std::string now  = Common::nowDateTimeUtc();
        const std::string hash = AsynGyanis::Core::Digest::sha256Hex(token);
        // 令牌哈希全局唯一，鉴权必须跨租户查找，归属租户在读到行之后再校验
        const std::optional<ApiTokenRecord> record = co_await m_tokens.scoped({0, true})->findByHash(loop, hash, now);
        if (!record.has_value())
        {
            co_return std::nullopt;
        }

        const std::optional<System::UserRecord> owner = co_await m_users->scoped({record->tenantId, false})->findById(loop, record->ownerUserId);
        if (!owner.has_value() || owner->status != 1 || owner->tenantId != record->tenantId)
        {
            co_return std::nullopt;
        }
        const std::optional<System::TenantRecord> tenant = co_await m_tenants->findById(loop, record->tenantId);
        if (!tenant.has_value() || tenant->status != 1)
        {
            co_return std::nullopt;
        }

        const std::vector<std::string>        granted = co_await m_users->permissionsFor(loop, owner->id);
        const std::unordered_set<std::string> allowed(granted.begin(), granted.end());
        std::vector<std::string>              effective;
        effective.reserve(record->scopes.size());
        for (const std::string &scope: record->scopes)
        {
            if (allowed.contains(scope))
            {
                effective.push_back(scope);
            }
        }
        // owner 的权限被全部收回时令牌立即失效；部分收回只缩小范围
        if (effective.empty())
        {
            co_return std::nullopt;
        }

        SessionRecord session;
        session.token          = std::string{token};
        session.userId         = owner->id;
        session.tenantId       = record->tenantId;
        session.tenantCode     = tenant->code;
        session.tenantName     = tenant->name;
        session.platformAdmin  = false;
        session.username       = owner->username;
        session.nickname       = owner->nickname;
        session.departmentId   = owner->departmentId;
        session.departmentName = owner->departmentName;
        session.roles          = co_await m_users->rolesFor(loop, owner->id);
        session.permissions    = std::move(effective);

        // 使用时间只用于诊断；写失败或节流跳过都不能推翻已经成立的鉴权
        try
        {
            const std::string cutoff = Common::formatDateTimeUtc(std::chrono::system_clock::now() - kLastUsedInterval);
            static_cast<void>(co_await m_tokens.touchLastUsed(loop, record->id, now, cutoff));
        } catch (const std::exception &failure)
        {
            LOG_WARN_FMT("API 令牌使用时间未更新：id={} 原因={}", record->id, failure.what());
        }
        co_return session;
    }
} // namespace EdgePulse::Auth
