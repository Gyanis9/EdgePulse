#include "Base/Database/Database.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Auth/ApiTokenRepository.h"
#include "Core/Auth/ApiTokenService.h"
#include "Core/Auth/AuthService.h"
#include "Core/Crypto/Digest.h"
#include "Core/System/UserRepository.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using EdgePulse::Auth::ApiTokenCreated;
    using EdgePulse::Auth::ApiTokenService;
    using EdgePulse::Auth::AuthError;
    using EdgePulse::Auth::SessionRecord;
    using EdgePulse::System::TenantScope;
    using EdgePulse::System::UserRepository;
    using EdgePulse::TestSupport::AuthTestContext;
    using EdgePulse::TestSupport::EventLoopRunner;

    /// 一套临时库、事件循环与已注入管理员身份的令牌服务
    struct TokenFixture
    {
        AuthTestContext                  context;
        EventLoopRunner                  runner;
        std::shared_ptr<UserRepository>  users;
        std::shared_ptr<ApiTokenService> tokens;
        SessionRecord                    owner;

        TokenFixture()
        {
            users            = std::make_shared<UserRepository>(context.database, TenantScope{1, false});
            tokens           = std::make_shared<ApiTokenService>(context.database, users, context.tenants);
            const auto admin = runner.run(users->findByUsername(runner.loop(), "admin"));
            if (!admin.has_value())
            {
                throw std::runtime_error("种子管理员缺失，无法建立 API 令牌用例");
            }
            owner.userId        = admin->id;
            owner.tenantId      = admin->tenantId;
            owner.tenantCode    = admin->tenantCode;
            owner.tenantName    = admin->tenantName;
            owner.platformAdmin = admin->platformAdmin;
            owner.username      = admin->username;
            owner.nickname      = admin->nickname;
            owner.departmentId  = admin->departmentId;
            owner.roles         = runner.run(users->rolesFor(runner.loop(), admin->id));
            owner.permissions   = runner.run(users->permissionsFor(runner.loop(), admin->id));
        }

        [[nodiscard]] std::expected<ApiTokenCreated, AuthError> create(const std::string &name, std::vector<std::string> scopes, const int expiresInSeconds = 3600)
        {
            return runner.run(tokens->create(runner.loop(), owner, name, std::move(scopes), expiresInSeconds));
        }
    };

    /// 直接读一条令牌行的生命周期列，核对哈希存储与节流写入
    struct StoredToken
    {
        std::int64_t tenantId{0};        ///< 归属租户
        std::string  tokenHash;          ///< 当前哈希
        std::string  previousHash;       ///< 轮换前的哈希
        std::string  previousValidUntil; ///< 旧哈希可用截止
        std::string  expiresAt;          ///< 到期时间
        std::string  lastUsedAt;         ///< 最近使用时间
        std::string  revokedAt;          ///< 吊销时间
    };

    [[nodiscard]] std::unique_ptr<AsynGyanis::Database::DatabaseConnection> openDirect(const AuthTestContext &context)
    {
        return EdgePulse::Common::Database::openDirectSqlite(context.databasePath().string());
    }

    void executeOrFail(const AuthTestContext &context, const std::string &sql)
    {
        const auto connection = openDirect(context);
        ASSERT_NE(nullptr, connection->execute(sql)) << connection->lastError();
    }

    [[nodiscard]] std::optional<StoredToken> readToken(const AuthTestContext &context, const std::int64_t id)
    {
        const auto connection = openDirect(context);
        const auto result     = connection->execute("SELECT tenant_id, token_hash, previous_hash, previous_valid_until, expires_at, last_used_at, revoked_at "
                                                    "FROM sys_api_token WHERE id = " +
                                                    std::to_string(id));
        if (!result || !result->next())
        {
            return std::nullopt;
        }
        StoredToken token;
        token.tenantId           = EdgePulse::Sql::integer(*result, "tenant_id");
        token.tokenHash          = EdgePulse::Sql::text(*result, "token_hash");
        token.previousHash       = EdgePulse::Sql::text(*result, "previous_hash");
        token.previousValidUntil = EdgePulse::Sql::dateTimeText(*result, "previous_valid_until");
        token.expiresAt          = EdgePulse::Sql::dateTimeText(*result, "expires_at");
        token.lastUsedAt         = EdgePulse::Sql::dateTimeText(*result, "last_used_at");
        token.revokedAt          = EdgePulse::Sql::dateTimeText(*result, "revoked_at");
        return token;
    }

    [[nodiscard]] std::optional<SessionRecord> authenticate(TokenFixture &fixture, const std::string &plaintext)
    {
        return fixture.runner.run(fixture.tokens->authenticate(fixture.runner.loop(), plaintext));
    }
} // namespace

// 钉住：创建只返回一次明文，落库只有 SHA-256，按令牌可以换回限定 scope 的身份
TEST(ApiTokenService, CreatesHashedTokenAndAuthenticatesWithScope)
{
    TokenFixture fixture;
    const auto   created = fixture.create("ci", {"device:node:list"});
    ASSERT_TRUE(created.has_value()) << created.error().message;
    EXPECT_EQ(71U, created->plaintext.size());
    EXPECT_EQ(0U, created->plaintext.rfind("ep_api_", 0));
    EXPECT_EQ(created->plaintext.substr(0, 12), created->record.tokenPrefix);
    EXPECT_NE(created->plaintext, created->record.tokenHash);
    EXPECT_EQ(AsynGyanis::Core::Digest::sha256Hex(created->plaintext), created->record.tokenHash);

    const auto stored = readToken(fixture.context, created->record.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(created->record.tokenHash, stored->tokenHash);
    EXPECT_EQ(64U, stored->tokenHash.size());
    EXPECT_NE(created->plaintext, stored->tokenHash);
    EXPECT_EQ(1, stored->tenantId);

    const auto session = authenticate(fixture, created->plaintext);
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(fixture.owner.userId, session->userId);
    EXPECT_EQ(1, session->tenantId);
    EXPECT_FALSE(session->platformAdmin);
    EXPECT_EQ(created->plaintext, session->token);
    EXPECT_EQ("admin", session->username);
    const std::vector<std::string> expectedScopes{"device:node:list"};
    EXPECT_EQ(expectedScopes, session->permissions);
    EXPECT_FALSE(session->roles.empty());

    const auto used = readToken(fixture.context, created->record.id);
    ASSERT_TRUE(used.has_value());
    EXPECT_FALSE(used->lastUsedAt.empty());
}

// 钉住：权限按 owner 当前角色实时求交集；全部 scope 被收回后令牌立即失效
TEST(ApiTokenService, IntersectsScopesWithCurrentPermissions)
{
    TokenFixture             fixture;
    std::vector<std::string> scopes{"device:node:list", "device:node:write"};
    const auto               created = fixture.create("ci", scopes);
    ASSERT_TRUE(created.has_value()) << created.error().message;

    executeOrFail(fixture.context, "DELETE FROM sys_role_menu WHERE menu_id IN (SELECT id FROM sys_menu WHERE permission = 'device:node:write')");
    const auto partial = authenticate(fixture, created->plaintext);
    ASSERT_TRUE(partial.has_value());
    const std::vector<std::string> expectedScopes{"device:node:list"};
    EXPECT_EQ(expectedScopes, partial->permissions);

    executeOrFail(fixture.context, "DELETE FROM sys_role_menu WHERE menu_id IN (SELECT id FROM sys_menu WHERE permission = 'device:node:list')");
    EXPECT_FALSE(authenticate(fixture, created->plaintext).has_value());
}

// 钉住：吊销写 revoked_at 且不物理删除；过期令牌直接失效
TEST(ApiTokenService, RejectsRevokedAndExpiredTokens)
{
    TokenFixture fixture;
    const auto   revoked = fixture.create("revoked", {"device:node:list"});
    ASSERT_TRUE(revoked.has_value()) << revoked.error().message;
    EXPECT_TRUE(fixture.runner.run(fixture.tokens->revoke(fixture.runner.loop(), fixture.owner.tenantId, revoked->record.id)));
    EXPECT_FALSE(authenticate(fixture, revoked->plaintext).has_value());
    const auto revokedRow = readToken(fixture.context, revoked->record.id);
    ASSERT_TRUE(revokedRow.has_value());
    EXPECT_FALSE(revokedRow->revokedAt.empty());
    EXPECT_FALSE(fixture.runner.run(fixture.tokens->revoke(fixture.runner.loop(), fixture.owner.tenantId, revoked->record.id)));

    const auto listed = fixture.runner.run(fixture.tokens->list(fixture.runner.loop(), fixture.owner));
    const auto found  = std::find_if(listed.begin(), listed.end(), [id = revoked->record.id](const EdgePulse::Auth::ApiTokenRecord &row) { return row.id == id; });
    EXPECT_NE(listed.end(), found);

    const auto expired = fixture.create("expired", {"device:node:list"});
    ASSERT_TRUE(expired.has_value()) << expired.error().message;
    executeOrFail(fixture.context, "UPDATE sys_api_token SET expires_at = '2000-01-01 00:00:00' WHERE id = " + std::to_string(expired->record.id));
    EXPECT_FALSE(authenticate(fixture, expired->plaintext).has_value());
}

// 钉住：停用 owner 与停用租户都会让令牌失效，恢复 owner 后同一令牌重新可用
TEST(ApiTokenService, RejectsDisabledOwnerAndTenant)
{
    TokenFixture fixture;
    const auto   created = fixture.create("owner", {"device:node:list"});
    ASSERT_TRUE(created.has_value()) << created.error().message;

    executeOrFail(fixture.context, "UPDATE sys_user SET status = 0 WHERE id = " + std::to_string(fixture.owner.userId));
    EXPECT_FALSE(authenticate(fixture, created->plaintext).has_value());
    executeOrFail(fixture.context, "UPDATE sys_user SET status = 1 WHERE id = " + std::to_string(fixture.owner.userId));
    EXPECT_TRUE(authenticate(fixture, created->plaintext).has_value()) << "owner 停用不应改写令牌本身";

    executeOrFail(fixture.context, "UPDATE sys_tenant SET status = 0 WHERE id = 1");
    EXPECT_FALSE(authenticate(fixture, created->plaintext).has_value());
}

// 钉住：轮换保留原到期时间，旧哈希在重叠窗口内可用，窗口关闭后立即失效
TEST(ApiTokenService, RotatesWithOneHourOverlapAndKeepsExpiry)
{
    TokenFixture fixture;
    const auto   created = fixture.create("rotate", {"device:node:list"}, 7200);
    ASSERT_TRUE(created.has_value()) << created.error().message;

    const auto rotated = fixture.runner.run(fixture.tokens->rotate(fixture.runner.loop(), fixture.owner, created->record.id));
    ASSERT_TRUE(rotated.has_value()) << rotated.error().message;
    EXPECT_NE(created->plaintext, rotated->plaintext);
    EXPECT_EQ(created->record.expiresAt, rotated->record.expiresAt);
    EXPECT_EQ(created->record.tokenHash, rotated->record.previousHash);
    EXPECT_FALSE(rotated->record.previousValidUntil.empty());

    const auto oldSession = authenticate(fixture, created->plaintext);
    ASSERT_TRUE(oldSession.has_value());
    EXPECT_EQ(created->plaintext, oldSession->token);
    ASSERT_TRUE(authenticate(fixture, rotated->plaintext).has_value());

    executeOrFail(fixture.context, "UPDATE sys_api_token SET previous_valid_until = '2000-01-01 00:00:00' WHERE id = " + std::to_string(created->record.id));
    EXPECT_FALSE(authenticate(fixture, created->plaintext).has_value());
    EXPECT_TRUE(authenticate(fixture, rotated->plaintext).has_value());

    const auto stored = readToken(fixture.context, created->record.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(rotated->record.tokenHash, stored->tokenHash);
    EXPECT_EQ(created->plaintext.rfind("ep_api_", 0), 0U);
}

// 钉住：列表只返回当前租户的令牌，别家租户的行不出现在同一条查询里
TEST(ApiTokenService, ListsOnlyCurrentTenant)
{
    TokenFixture fixture;
    const auto   first = fixture.create("first", {"device:node:list"});
    ASSERT_TRUE(first.has_value()) << first.error().message;
    const auto second = fixture.create("second", {"device:node:list"});
    ASSERT_TRUE(second.has_value()) << second.error().message;

    const std::string now    = EdgePulse::Common::nowDateTimeUtc();
    const std::string future = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::hours{1});
    executeOrFail(fixture.context, "INSERT INTO sys_api_token (id, tenant_id, owner_user_id, name, token_prefix, token_hash, scopes_json, expires_at, created_by, created_at) "
                                   "VALUES (990001, 2, 1, 'other', 'ep_api_other', '" +
                                           std::string(64, 'f') + "', '[\"device:node:list\"]', '" + future + "', 1, '" + now + "')");

    const auto rows = fixture.runner.run(fixture.tokens->list(fixture.runner.loop(), fixture.owner));
    ASSERT_EQ(2U, rows.size());
    for (const EdgePulse::Auth::ApiTokenRecord &row: rows)
    {
        EXPECT_EQ(1, row.tenantId);
        EXPECT_NE("other", row.name);
    }
}

// 钉住：名称、scope 与有效期在落库前校验，默认有效期为 30 天
TEST(ApiTokenService, RejectsInvalidNameScopesAndExpiry)
{
    TokenFixture fixture;

    const auto emptyName = fixture.create("", {"device:node:list"});
    ASSERT_FALSE(emptyName.has_value());
    EXPECT_EQ(400, emptyName.error().httpStatus);

    const auto emptyScopes = fixture.create("ci", {});
    ASSERT_FALSE(emptyScopes.has_value());
    EXPECT_EQ(400, emptyScopes.error().httpStatus);
    EXPECT_NE(std::string::npos, emptyScopes.error().message.find("权限范围"));

    const auto duplicate = fixture.create("ci", {"device:node:list", "device:node:list"});
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_NE(std::string::npos, duplicate.error().message.find("重复"));

    const auto unknown = fixture.create("ci", {"system:not-granted"});
    ASSERT_FALSE(unknown.has_value());
    EXPECT_NE(std::string::npos, unknown.error().message.find("权限"));

    EXPECT_FALSE(fixture.create("ci", {"device:node:list"}, 299).has_value());
    EXPECT_FALSE(fixture.create("ci", {"device:node:list"}, 31536001).has_value());

    const auto defaulted = fixture.create("ci", {"device:node:list"}, 0);
    ASSERT_TRUE(defaulted.has_value()) << defaulted.error().message;
    const std::string lower = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::days{29});
    const std::string upper = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::days{31});
    EXPECT_GT(defaulted->record.expiresAt, lower);
    EXPECT_LE(defaulted->record.expiresAt, upper);
}

// 钉住：last_used_at 最多每 60 秒写一次，窗口外的下一次鉴权会补写
TEST(ApiTokenService, TouchesLastUsedAtAtMostOncePerMinute)
{
    TokenFixture fixture;
    const auto   created = fixture.create("touch", {"device:node:list"});
    ASSERT_TRUE(created.has_value()) << created.error().message;

    ASSERT_TRUE(authenticate(fixture, created->plaintext).has_value());
    const auto firstTouch = readToken(fixture.context, created->record.id);
    ASSERT_TRUE(firstTouch.has_value());
    ASSERT_FALSE(firstTouch->lastUsedAt.empty());

    ASSERT_TRUE(authenticate(fixture, created->plaintext).has_value());
    const auto secondTouch = readToken(fixture.context, created->record.id);
    ASSERT_TRUE(secondTouch.has_value());
    EXPECT_EQ(firstTouch->lastUsedAt, secondTouch->lastUsedAt);

    const std::string stale = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() - std::chrono::seconds{120});
    executeOrFail(fixture.context, "UPDATE sys_api_token SET last_used_at = '" + stale + "' WHERE id = " + std::to_string(created->record.id));
    ASSERT_TRUE(authenticate(fixture, created->plaintext).has_value());
    const auto thirdTouch = readToken(fixture.context, created->record.id);
    ASSERT_TRUE(thirdTouch.has_value());
    EXPECT_GT(thirdTouch->lastUsedAt, stale);
}

// 钉住：AuthService 会话未命中时汇入令牌认证，会话路径与令牌路径互不影响
TEST(ApiTokenService, AuthServiceUsesApiTokenWhenSessionMisses)
{
    TokenFixture fixture;
    const auto   created = fixture.create("api", {"device:node:list"});
    ASSERT_TRUE(created.has_value()) << created.error().message;
    fixture.context.auth->setApiTokenService(fixture.tokens);

    const auto tokenSession = fixture.runner.run(fixture.context.auth->authenticate(fixture.runner.loop(), created->plaintext));
    ASSERT_TRUE(tokenSession.has_value());
    EXPECT_FALSE(tokenSession->platformAdmin);
    const std::vector<std::string> expectedScopes{"device:node:list"};
    EXPECT_EQ(expectedScopes, tokenSession->permissions);

    const auto challenge = fixture.runner.run(fixture.context.captcha->issue(fixture.runner.loop(), "127.0.0.1"));
    ASSERT_TRUE(challenge.has_value());
    const auto loggedIn = fixture.runner.run(fixture.context.auth->login(
            fixture.runner.loop(), EdgePulse::Auth::LoginCredentials{"admin", "admin123", challenge->code, challenge->uuid, "127.0.0.1", "api-token-test"}));
    ASSERT_TRUE(loggedIn.has_value()) << loggedIn.error().message;
    const auto loginSession = fixture.runner.run(fixture.context.auth->authenticate(fixture.runner.loop(), loggedIn->token));
    ASSERT_TRUE(loginSession.has_value());
    EXPECT_TRUE(loginSession->platformAdmin);

    ASSERT_TRUE(fixture.runner.run(fixture.tokens->revoke(fixture.runner.loop(), fixture.owner.tenantId, created->record.id)));
    EXPECT_FALSE(fixture.runner.run(fixture.context.auth->authenticate(fixture.runner.loop(), created->plaintext)).has_value());
    EXPECT_TRUE(fixture.runner.run(fixture.context.auth->authenticate(fixture.runner.loop(), loggedIn->token)).has_value());
}
