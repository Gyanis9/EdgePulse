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
#include <array>
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
    using EdgePulse::Auth::ApiTokenInfo;
    using EdgePulse::Auth::ApiTokenRepository;
    using EdgePulse::Auth::ApiTokenService;
    using EdgePulse::Auth::AuthError;
    using EdgePulse::Auth::SessionRecord;
    using EdgePulse::System::TenantScope;
    using EdgePulse::System::UserRepository;
    using EdgePulse::TestSupport::AuthTestContext;
    using EdgePulse::TestSupport::EventLoopRunner;

    template<typename Token>
    concept ExposesTokenHash = requires(const Token &token) { token.tokenHash; };

    template<typename Token>
    concept ExposesPreviousHash = requires(const Token &token) { token.previousHash; };

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
        std::int64_t ownerUserId{0};     ///< 所属用户
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
        const auto result     = connection->execute("SELECT tenant_id, owner_user_id, token_hash, previous_hash, previous_valid_until, expires_at, last_used_at, revoked_at "
                                                    "FROM sys_api_token WHERE id = " +
                                                    std::to_string(id));
        if (!result || !result->next())
        {
            return std::nullopt;
        }
        StoredToken token;
        token.tenantId           = EdgePulse::Sql::integer(*result, "tenant_id");
        token.ownerUserId        = EdgePulse::Sql::integer(*result, "owner_user_id");
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

    [[nodiscard]] std::string fakeHash(const std::int64_t seed)
    {
        std::string hash(63, 'a');
        hash.push_back(static_cast<char>('a' + static_cast<int>(seed % 26)));
        return hash;
    }

    void insertTokenRow(const AuthTestContext &context, const std::int64_t id, const std::int64_t ownerUserId, const std::string &name, const std::string &scopesJson,
                        const std::int64_t tenantId = 1)
    {
        const std::string now    = EdgePulse::Common::nowDateTimeUtc();
        const std::string future = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::hours{1});
        executeOrFail(context, "INSERT INTO sys_api_token (id, tenant_id, owner_user_id, name, token_prefix, token_hash, scopes_json, expires_at, created_by, created_at) "
                               "VALUES (" +
                                       std::to_string(id) + ", " + std::to_string(tenantId) + ", " + std::to_string(ownerUserId) + ", '" + name + "', 'ep_api_fixture', '" +
                                       fakeHash(id) + "', '" + scopesJson + "', '" + future + "', " + std::to_string(ownerUserId) + ", '" + now + "')");
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
    EXPECT_EQ(created->plaintext.substr(0, 12), created->token.prefix);
    EXPECT_EQ("ci", created->token.name);
    EXPECT_TRUE(created->previousValidUntil.empty());

    const auto stored = readToken(fixture.context, created->token.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(AsynGyanis::Core::Digest::sha256Hex(created->plaintext), stored->tokenHash);
    EXPECT_EQ(64U, stored->tokenHash.size());
    EXPECT_NE(created->plaintext, stored->tokenHash);
    EXPECT_EQ(1, stored->tenantId);
    EXPECT_EQ(fixture.owner.userId, stored->ownerUserId);

    const auto session = authenticate(fixture, created->plaintext);
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(fixture.owner.userId, session->userId);
    EXPECT_EQ(1, session->tenantId);
    EXPECT_FALSE(session->platformAdmin);
    EXPECT_TRUE(session->apiToken);
    EXPECT_EQ(created->plaintext, session->token);
    EXPECT_EQ("admin", session->username);
    const std::vector<std::string> expectedScopes{"device:node:list"};
    EXPECT_EQ(expectedScopes, session->permissions);
    EXPECT_FALSE(session->roles.empty());

    const auto used = readToken(fixture.context, created->token.id);
    ASSERT_TRUE(used.has_value());
    EXPECT_FALSE(used->lastUsedAt.empty());
}

// 钉住：对外摘要不含任何哈希列，改由类型在编译期挡住误序列化
TEST(ApiTokenService, ServiceResultsCarryNoTokenHashes)
{
    static_assert(!ExposesTokenHash<ApiTokenInfo>);
    static_assert(!ExposesPreviousHash<ApiTokenInfo>);
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
    EXPECT_TRUE(fixture.runner.run(fixture.tokens->revoke(fixture.runner.loop(), fixture.owner, revoked->token.id)));
    EXPECT_FALSE(authenticate(fixture, revoked->plaintext).has_value());
    const auto revokedRow = readToken(fixture.context, revoked->token.id);
    ASSERT_TRUE(revokedRow.has_value());
    EXPECT_FALSE(revokedRow->revokedAt.empty());
    EXPECT_FALSE(fixture.runner.run(fixture.tokens->revoke(fixture.runner.loop(), fixture.owner, revoked->token.id)));

    const auto listed = fixture.runner.run(fixture.tokens->list(fixture.runner.loop(), fixture.owner));
    const auto found  = std::find_if(listed.begin(), listed.end(), [id = revoked->token.id](const ApiTokenInfo &row) { return row.id == id; });
    EXPECT_NE(listed.end(), found);

    const auto expired = fixture.create("expired", {"device:node:list"});
    ASSERT_TRUE(expired.has_value()) << expired.error().message;
    executeOrFail(fixture.context, "UPDATE sys_api_token SET expires_at = '2000-01-01 00:00:00' WHERE id = " + std::to_string(expired->token.id));
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

// 钉住：令牌行归属租户与 owner 租户不一致时拒绝鉴权
TEST(ApiTokenService, RejectsTenantMismatchOnAuthentication)
{
    TokenFixture fixture;
    const auto   created = fixture.create("tenant-mismatch", {"device:node:list"});
    ASSERT_TRUE(created.has_value()) << created.error().message;

    executeOrFail(fixture.context, "UPDATE sys_api_token SET tenant_id = 2 WHERE id = " + std::to_string(created->token.id));
    EXPECT_FALSE(authenticate(fixture, created->plaintext).has_value());
}

// 钉住：轮换保留原到期时间，旧哈希在重叠窗口内可用，窗口关闭后立即失效
TEST(ApiTokenService, RotatesWithOneHourOverlapAndKeepsExpiry)
{
    TokenFixture fixture;
    const auto   created = fixture.create("rotate", {"device:node:list"}, 7200);
    ASSERT_TRUE(created.has_value()) << created.error().message;
    const auto before = readToken(fixture.context, created->token.id);
    ASSERT_TRUE(before.has_value());

    const auto rotated = fixture.runner.run(fixture.tokens->rotate(fixture.runner.loop(), fixture.owner, created->token.id));
    ASSERT_TRUE(rotated.has_value()) << rotated.error().message;
    EXPECT_NE(created->plaintext, rotated->plaintext);
    EXPECT_EQ(created->token.expiresAt, rotated->token.expiresAt);
    EXPECT_EQ(created->token.id, rotated->token.id);

    const std::string lower = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::minutes{59});
    const std::string upper = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::minutes{61});
    const auto        after = readToken(fixture.context, created->token.id);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(before->tokenHash, after->previousHash);
    EXPECT_GE(after->previousValidUntil, lower);
    EXPECT_LE(after->previousValidUntil, upper);
    EXPECT_EQ(after->previousValidUntil, rotated->previousValidUntil);
    EXPECT_EQ(AsynGyanis::Core::Digest::sha256Hex(rotated->plaintext), after->tokenHash);
    EXPECT_EQ(rotated->plaintext.substr(0, 12), rotated->token.prefix);

    const auto oldSession = authenticate(fixture, created->plaintext);
    ASSERT_TRUE(oldSession.has_value());
    EXPECT_EQ(created->plaintext, oldSession->token);
    ASSERT_TRUE(authenticate(fixture, rotated->plaintext).has_value());

    executeOrFail(fixture.context, "UPDATE sys_api_token SET previous_valid_until = '2000-01-01 00:00:00' WHERE id = " + std::to_string(created->token.id));
    EXPECT_FALSE(authenticate(fixture, created->plaintext).has_value());
    EXPECT_TRUE(authenticate(fixture, rotated->plaintext).has_value());

    const auto stored = readToken(fixture.context, created->token.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(AsynGyanis::Core::Digest::sha256Hex(rotated->plaintext), stored->tokenHash);
    EXPECT_EQ(0U, rotated->plaintext.rfind("ep_api_", 0));
    EXPECT_EQ(rotated->plaintext.substr(0, 12), rotated->token.prefix);
}

// 钉住：已吊销或已过期的令牌都不能轮换，且都返回 409
TEST(ApiTokenService, RejectsRotateAfterRevokeOrExpiry)
{
    TokenFixture fixture;
    const auto   revoked = fixture.create("rotate-revoked", {"device:node:list"});
    ASSERT_TRUE(revoked.has_value()) << revoked.error().message;
    ASSERT_TRUE(fixture.runner.run(fixture.tokens->revoke(fixture.runner.loop(), fixture.owner, revoked->token.id)));
    const auto revokedRotate = fixture.runner.run(fixture.tokens->rotate(fixture.runner.loop(), fixture.owner, revoked->token.id));
    ASSERT_FALSE(revokedRotate.has_value());
    EXPECT_EQ(409, revokedRotate.error().httpStatus);
    EXPECT_NE(std::string::npos, revokedRotate.error().message.find("吊销"));

    const auto expired = fixture.create("rotate-expired", {"device:node:list"});
    ASSERT_TRUE(expired.has_value()) << expired.error().message;
    executeOrFail(fixture.context, "UPDATE sys_api_token SET expires_at = '2000-01-01 00:00:00' WHERE id = " + std::to_string(expired->token.id));
    const auto expiredRotate = fixture.runner.run(fixture.tokens->rotate(fixture.runner.loop(), fixture.owner, expired->token.id));
    ASSERT_FALSE(expiredRotate.has_value());
    EXPECT_EQ(409, expiredRotate.error().httpStatus);
    EXPECT_NE(std::string::npos, expiredRotate.error().message.find("过期"));
}

// 钉住：乐观锁未命中时轮换返回 409，而不是把旧明文或错误状态交出去
TEST(ApiTokenService, RotateReportsConflictWhenOptimisticLockMisses)
{
    TokenFixture fixture;
    const auto   created = fixture.create("conflict", {"device:node:list"});
    ASSERT_TRUE(created.has_value()) << created.error().message;

    // 模拟并发轮换：让 UPDATE 静默不命中，等价于另一个请求先改掉了哈希
    executeOrFail(fixture.context, "CREATE TRIGGER api_token_rotate_guard BEFORE UPDATE OF token_hash ON sys_api_token BEGIN SELECT RAISE(IGNORE); END");
    const auto conflicted = fixture.runner.run(fixture.tokens->rotate(fixture.runner.loop(), fixture.owner, created->token.id));
    ASSERT_FALSE(conflicted.has_value());
    EXPECT_EQ(409, conflicted.error().httpStatus);
    EXPECT_NE(std::string::npos, conflicted.error().message.find("并发"));
}

// 钉住：仓储的轮换 UPDATE 自己带到期条件，读出后过期也不会续命
TEST(ApiTokenService, ReplaceHashRejectsRowExpiredAfterRead)
{
    TokenFixture fixture;
    const auto   created = fixture.create("toctou", {"device:node:list"}, 300);
    ASSERT_TRUE(created.has_value()) << created.error().message;
    const auto stored = readToken(fixture.context, created->token.id);
    ASSERT_TRUE(stored.has_value());

    const std::string expiresSoon   = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::minutes{1});
    const std::string later         = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::minutes{2});
    const std::string previousUntil = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::hours{1});
    executeOrFail(fixture.context, "UPDATE sys_api_token SET expires_at = '" + expiresSoon + "' WHERE id = " + std::to_string(created->token.id));

    const auto repository = std::make_shared<ApiTokenRepository>(fixture.context.database, TenantScope{1, false});
    const bool replaced   = fixture.runner.run(repository->replaceHash(fixture.runner.loop(), created->token.id, stored->tokenHash,
                                                                       AsynGyanis::Core::Digest::sha256Hex("ep_api_replacement"), "ep_api_repl", previousUntil, later));
    EXPECT_FALSE(replaced);
}

// 钉住：列表只返回当前租户的令牌，别家租户的行不出现在同一条查询里
TEST(ApiTokenService, ListsOnlyCurrentTenant)
{
    TokenFixture fixture;
    const auto   first = fixture.create("first", {"device:node:list"});
    ASSERT_TRUE(first.has_value()) << first.error().message;
    const auto second = fixture.create("second", {"device:node:list"});
    ASSERT_TRUE(second.has_value()) << second.error().message;

    insertTokenRow(fixture.context, 990001, 1, "other", "[\"device:node:list\"]", 2);

    const auto rows = fixture.runner.run(fixture.tokens->list(fixture.runner.loop(), fixture.owner));
    ASSERT_EQ(2U, rows.size());
    for (const ApiTokenInfo &row: rows)
    {
        EXPECT_NE("other", row.name);
        EXPECT_EQ(12U, row.prefix.size());
    }
}

// 钉住：名称、scope 与有效期在落库前校验，默认有效期为 30 天，边界按 UTF-8 码点计
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

    EXPECT_TRUE(fixture.create(std::string(64, 'a'), {"device:node:list"}).has_value());
    const auto tooLong = fixture.create(std::string(65, 'a'), {"device:node:list"});
    ASSERT_FALSE(tooLong.has_value());
    EXPECT_EQ(400, tooLong.error().httpStatus);
    EXPECT_NE(std::string::npos, tooLong.error().message.find("64"));

    std::string chineseName;
    for (int index = 0; index < 64; ++index)
    {
        chineseName += "名";
    }
    EXPECT_TRUE(fixture.create(chineseName, {"device:node:list"}).has_value()) << "64 个中文字符按码点计应被接受";

    EXPECT_FALSE(fixture.create("ci", {"device:node:list"}, 299).has_value());
    EXPECT_FALSE(fixture.create("ci", {"device:node:list"}, 31536001).has_value());
    EXPECT_TRUE(fixture.create("ci", {"device:node:list"}, 300).has_value());
    EXPECT_TRUE(fixture.create("ci", {"device:node:list"}, 31536000).has_value());

    const auto defaulted = fixture.create("ci", {"device:node:list"}, 0);
    ASSERT_TRUE(defaulted.has_value()) << defaulted.error().message;
    const std::string lower = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::days{29});
    const std::string upper = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::days{31});
    EXPECT_GT(defaulted->token.expiresAt, lower);
    EXPECT_LE(defaulted->token.expiresAt, upper);
}

// 钉住：last_used_at 最多每 60 秒写一次，窗口外的下一次鉴权会补写
TEST(ApiTokenService, TouchesLastUsedAtAtMostOncePerMinute)
{
    TokenFixture fixture;
    const auto   created = fixture.create("touch", {"device:node:list"});
    ASSERT_TRUE(created.has_value()) << created.error().message;

    ASSERT_TRUE(authenticate(fixture, created->plaintext).has_value());
    const auto firstTouch = readToken(fixture.context, created->token.id);
    ASSERT_TRUE(firstTouch.has_value());
    ASSERT_FALSE(firstTouch->lastUsedAt.empty());

    ASSERT_TRUE(authenticate(fixture, created->plaintext).has_value());
    const auto secondTouch = readToken(fixture.context, created->token.id);
    ASSERT_TRUE(secondTouch.has_value());
    EXPECT_EQ(firstTouch->lastUsedAt, secondTouch->lastUsedAt);

    const std::string stale = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() - std::chrono::seconds{120});
    executeOrFail(fixture.context, "UPDATE sys_api_token SET last_used_at = '" + stale + "' WHERE id = " + std::to_string(created->token.id));
    ASSERT_TRUE(authenticate(fixture, created->plaintext).has_value());
    const auto thirdTouch = readToken(fixture.context, created->token.id);
    ASSERT_TRUE(thirdTouch.has_value());
    EXPECT_GT(thirdTouch->lastUsedAt, stale);
}

// 钉住：使用时间写入显式带租户边界，别的租户写不动这一行
TEST(ApiTokenService, TouchLastUsedStaysInsideTenant)
{
    TokenFixture fixture;
    const auto   created = fixture.create("touch-tenant", {"device:node:list"});
    ASSERT_TRUE(created.has_value()) << created.error().message;

    const auto        repository = std::make_shared<ApiTokenRepository>(fixture.context.database, TenantScope{1, false});
    const std::string now        = EdgePulse::Common::nowDateTimeUtc();
    const std::string cutoff     = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() - std::chrono::seconds{60});
    EXPECT_FALSE(fixture.runner.run(repository->touchLastUsed(fixture.runner.loop(), 2, created->token.id, now, cutoff)));
    const auto untouched = readToken(fixture.context, created->token.id);
    ASSERT_TRUE(untouched.has_value());
    EXPECT_TRUE(untouched->lastUsedAt.empty());

    EXPECT_TRUE(fixture.runner.run(repository->touchLastUsed(fixture.runner.loop(), 1, created->token.id, now, cutoff)));
}

// 钉住：令牌会话不能签发任何子令牌，即使自身带 system:token:add；浏览器会话仍可创建
TEST(ApiTokenService, RejectsTokenSessionCreate)
{
    TokenFixture fixture;
    const auto   delegated = fixture.create("delegate", {"system:token:add", "device:node:list"});
    ASSERT_TRUE(delegated.has_value()) << delegated.error().message;
    const auto restricted = authenticate(fixture, delegated->plaintext);
    ASSERT_TRUE(restricted.has_value());
    EXPECT_TRUE(restricted->apiToken);

    const auto child = fixture.runner.run(fixture.tokens->create(fixture.runner.loop(), *restricted, "child", {"system:token:add"}, 3600));
    ASSERT_FALSE(child.has_value());
    EXPECT_EQ(403, child.error().httpStatus);
    EXPECT_NE(std::string::npos, child.error().message.find("API 令牌"));
    EXPECT_NE(std::string::npos, child.error().message.find("浏览器会话"));

    // 缩到空 scope 之外也不行：拒绝发生在校验 scope 之前，令牌会话没有签发入口
    const auto narrower = fixture.runner.run(fixture.tokens->create(fixture.runner.loop(), *restricted, "child", {"device:node:list"}, 3600));
    ASSERT_FALSE(narrower.has_value());
    EXPECT_EQ(403, narrower.error().httpStatus);

    const auto browserChild = fixture.create("browser-child", {"system:token:add"});
    ASSERT_TRUE(browserChild.has_value()) << browserChild.error().message;
    const auto stored = readToken(fixture.context, browserChild->token.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(fixture.owner.userId, stored->ownerUserId);
}

// 钉住：令牌会话不能轮换任何令牌，包括自己；浏览器会话仍可轮换与吊销
TEST(ApiTokenService, RejectsTokenSessionRotation)
{
    TokenFixture fixture;
    const auto   delegated = fixture.create("delegate", {"system:token:edit", "system:token:remove"});
    ASSERT_TRUE(delegated.has_value()) << delegated.error().message;
    const auto restricted = authenticate(fixture, delegated->plaintext);
    ASSERT_TRUE(restricted.has_value());

    insertTokenRow(fixture.context, 990010, 2, "other-owner", "[\"system:token:edit\"]");
    insertTokenRow(fixture.context, 990011, fixture.owner.userId, "wider-scope", "[\"device:node:write\"]");
    const auto otherBefore = readToken(fixture.context, 990010);
    ASSERT_TRUE(otherBefore.has_value());

    for (const std::int64_t id: std::array<std::int64_t, 3>{990010, 990011, delegated->token.id})
    {
        const auto rotated = fixture.runner.run(fixture.tokens->rotate(fixture.runner.loop(), *restricted, id));
        ASSERT_FALSE(rotated.has_value()) << "令牌会话不得轮换 " << id;
        EXPECT_EQ(403, rotated.error().httpStatus);
        EXPECT_NE(std::string::npos, rotated.error().message.find("API 令牌"));
        EXPECT_NE(std::string::npos, rotated.error().message.find("浏览器会话"));
    }
    EXPECT_FALSE(fixture.runner.run(fixture.tokens->revoke(fixture.runner.loop(), *restricted, 990010))) << "不得吊销他人的令牌";
    EXPECT_FALSE(fixture.runner.run(fixture.tokens->revoke(fixture.runner.loop(), *restricted, 990011))) << "不得吊销 scope 更宽的令牌";

    const auto other = readToken(fixture.context, 990010);
    ASSERT_TRUE(other.has_value());
    EXPECT_EQ(otherBefore->tokenHash, other->tokenHash) << "被拒的轮换不得留下新哈希";
    EXPECT_TRUE(other->revokedAt.empty());
    const auto wider = readToken(fixture.context, 990011);
    ASSERT_TRUE(wider.has_value());
    EXPECT_TRUE(wider->revokedAt.empty());

    const auto browserRotate = fixture.runner.run(fixture.tokens->rotate(fixture.runner.loop(), fixture.owner, 990011));
    ASSERT_TRUE(browserRotate.has_value()) << browserRotate.error().message;
    EXPECT_EQ(browserRotate->plaintext.substr(0, 12), browserRotate->token.prefix);
    EXPECT_TRUE(fixture.runner.run(fixture.tokens->revoke(fixture.runner.loop(), fixture.owner, 990010)));
    const auto revokedOther = readToken(fixture.context, 990010);
    ASSERT_TRUE(revokedOther.has_value());
    EXPECT_FALSE(revokedOther->revokedAt.empty());
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
    EXPECT_TRUE(tokenSession->apiToken);
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
    EXPECT_FALSE(loginSession->apiToken);
    EXPECT_TRUE(loginSession->platformAdmin);

    ASSERT_TRUE(fixture.runner.run(fixture.tokens->revoke(fixture.runner.loop(), fixture.owner, created->token.id)));
    EXPECT_FALSE(fixture.runner.run(fixture.context.auth->authenticate(fixture.runner.loop(), created->plaintext)).has_value());
    EXPECT_TRUE(fixture.runner.run(fixture.context.auth->authenticate(fixture.runner.loop(), loggedIn->token)).has_value());
}

// 钉住：令牌会话不能改账户口令，避免令牌泄露连带接管账户
TEST(ApiTokenService, TokenSessionCannotChangePassword)
{
    TokenFixture fixture;
    const auto   created = fixture.create("no-password", {"device:node:list"});
    ASSERT_TRUE(created.has_value()) << created.error().message;
    fixture.context.auth->setApiTokenService(fixture.tokens);

    const auto session = fixture.runner.run(fixture.context.auth->authenticate(fixture.runner.loop(), created->plaintext));
    ASSERT_TRUE(session.has_value());
    const auto changed = fixture.runner.run(fixture.context.auth->changePassword(fixture.runner.loop(), *session, "admin123", "newpass1"));
    ASSERT_FALSE(changed.has_value());
    EXPECT_EQ(403, changed.error().httpStatus);
    EXPECT_NE(std::string::npos, changed.error().message.find("API 令牌"));
}
