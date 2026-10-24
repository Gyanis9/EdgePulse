#include "Base/Coding/Base64.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Database/Database.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/TimeFormat.h"
#include "Base/Security/Hmac.h"
#include "Core/Auth/MfaCrypto.h"
#include "Core/Auth/MfaService.h"
#include "Core/Auth/Totp.h"
#include "Core/Cache/MemoryKeyValueStore.h"
#include "Core/System/TenantScope.h"
#include "Core/System/UserRepository.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    using EdgePulse::Auth::MfaService;
    using EdgePulse::Auth::MfaStatus;
    using EdgePulse::Auth::SessionRecord;
    using EdgePulse::System::TenantScope;
    using EdgePulse::System::UserRepository;
    using EdgePulse::TestSupport::AuthTestContext;
    using EdgePulse::TestSupport::EventLoopRunner;

    [[nodiscard]] std::string rawMfaKey()
    {
        std::string key;
        key.reserve(32);
        for (int index = 0; index < 32; ++index)
        {
            key.push_back(static_cast<char>(index + 1));
        }
        return key;
    }

    [[nodiscard]] std::string encodedMfaKey()
    {
        return AsynGyanis::Base::base64Encode(rawMfaKey());
    }

    [[nodiscard]] std::string setupCacheKey(const std::int64_t userId)
    {
        return "edgepulse:mfa:setup:" + std::to_string(userId);
    }

    void executeOrFail(const AuthTestContext &context, const std::string &sql)
    {
        const auto connection = EdgePulse::Common::Database::openDirectSqlite(context.databasePath().string());
        ASSERT_NE(nullptr, connection->execute(sql)) << connection->lastError();
    }

    [[nodiscard]] std::string readRecoveryJson(const AuthTestContext &context, const std::int64_t userId)
    {
        const auto connection = EdgePulse::Common::Database::openDirectSqlite(context.databasePath().string());
        const auto result     = connection->execute("SELECT recovery_codes_json FROM sys_user_mfa WHERE user_id = " + std::to_string(userId));
        if (!result || !result->next())
        {
            return {};
        }
        return EdgePulse::Sql::text(*result, "recovery_codes_json");
    }

    /// 直接读一行两步验证记录，用于核对启用落库的密文与恢复码
    struct StoredMfaRow
    {
        std::string  secretCiphertext; ///< 秘密密文
        std::string  secretNonce;      ///< nonce
        std::string  recoveryJson;     ///< 恢复码哈希 JSON
        std::int64_t lastUsedStep{0};  ///< 已消费步号
        bool         enabled{false};   ///< 是否已启用
    };

    [[nodiscard]] std::optional<StoredMfaRow> readMfaRow(const AuthTestContext &context, const std::int64_t userId)
    {
        const auto connection = EdgePulse::Common::Database::openDirectSqlite(context.databasePath().string());
        const auto result     = connection->execute("SELECT secret_ciphertext, secret_nonce, recovery_codes_json, last_used_step, enabled FROM sys_user_mfa WHERE user_id = " +
                                                    std::to_string(userId));
        if (!result || !result->next())
        {
            return std::nullopt;
        }
        StoredMfaRow row;
        row.secretCiphertext = EdgePulse::Sql::text(*result, "secret_ciphertext");
        row.secretNonce      = EdgePulse::Sql::text(*result, "secret_nonce");
        row.recoveryJson     = EdgePulse::Sql::text(*result, "recovery_codes_json");
        row.lastUsedStep     = EdgePulse::Sql::integer(*result, "last_used_step");
        row.enabled          = EdgePulse::Sql::integer(*result, "enabled") != 0;
        return row;
    }

    [[nodiscard]] std::vector<std::string> parseHashes(const std::string &json)
    {
        std::vector<std::string>                           hashes;
        const std::optional<AsynGyanis::Base::ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(json);
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
     * @brief 把协程结果搬进 promise，供同一条事件循环上的并发启用各自等待
     */
    template<typename ResultType>
    AsynGyanis::Core::Task<> forwardInto(AsynGyanis::Core::Task<ResultType> task, std::shared_ptr<std::promise<ResultType>> promise)
    {
        try
        {
            if constexpr (std::is_void_v<ResultType>)
            {
                co_await task;
                promise->set_value();
            } else
            {
                promise->set_value(co_await task);
            }
        } catch (...)
        {
            promise->set_exception(std::current_exception());
        }
    }

    /// 一套临时库、事件循环与已注入管理员身份的两步验证服务
    struct MfaFixture
    {
        AuthTestContext                 context;
        EventLoopRunner                 runner;
        std::shared_ptr<UserRepository> users;
        std::shared_ptr<MfaService>     service;
        SessionRecord                   admin;

        MfaFixture()
        {
            users           = std::make_shared<UserRepository>(context.database, TenantScope{1, false});
            const auto user = runner.run(users->findByUsername(runner.loop(), "admin"));
            if (!user.has_value())
            {
                throw std::runtime_error("种子管理员缺失，无法建立两步验证用例");
            }
            admin.userId        = user->id;
            admin.tenantId      = user->tenantId;
            admin.tenantCode    = user->tenantCode;
            admin.tenantName    = user->tenantName;
            admin.platformAdmin = user->platformAdmin;
            admin.username      = user->username;
            admin.nickname      = user->nickname;
            admin.departmentId  = user->departmentId;
            service             = std::make_shared<MfaService>(context.database, context.cache, encodedMfaKey());
        }

        [[nodiscard]] static std::int64_t nowStep()
        {
            return EdgePulse::Auth::Totp::currentStep();
        }
    };
} // namespace

// 钉住：启用、状态、TOTP 校验与同一步重放、恢复码一次性、错误码拒绝关闭、关闭后失效
TEST(MfaService, EnableVerifyReplayRecoveryAndDisable)
{
    MfaFixture fixture;

    const auto setup = fixture.runner.run(fixture.service->beginSetup(fixture.runner.loop(), fixture.admin));
    ASSERT_TRUE(setup.has_value()) << setup.error().message;
    EXPECT_EQ(32U, setup->secret.size());
    EXPECT_EQ("otpauth://totp/EdgePulse:" + fixture.admin.username + "?secret=" + setup->secret + "&issuer=EdgePulse&digits=6&period=30", setup->otpauthUri);

    const std::int64_t enableStep = MfaFixture::nowStep();
    const std::string  enableCode = EdgePulse::Auth::Totp::code(setup->secret, enableStep);

    // 输错一次不消耗待启用的秘密，改正后仍可启用
    const auto wrongEnable = fixture.runner.run(fixture.service->enable(fixture.runner.loop(), fixture.admin, "abcdef"));
    ASSERT_FALSE(wrongEnable.has_value());
    EXPECT_EQ(401, wrongEnable.error().httpStatus);

    const auto recoveryCodes = fixture.runner.run(fixture.service->enable(fixture.runner.loop(), fixture.admin, enableCode));
    ASSERT_TRUE(recoveryCodes.has_value()) << recoveryCodes.error().message;
    ASSERT_EQ(10U, recoveryCodes->size());
    for (const std::string &recoveryCode: *recoveryCodes)
    {
        ASSERT_EQ(9U, recoveryCode.size());
        EXPECT_EQ('-', recoveryCode[4]);
    }

    MfaStatus status = fixture.runner.run(fixture.service->status(fixture.runner.loop(), fixture.admin.userId));
    EXPECT_TRUE(status.enabled);
    EXPECT_EQ(10U, status.remainingRecoveryCodes);
    EXPECT_TRUE(fixture.runner.run(fixture.service->enabled(fixture.runner.loop(), fixture.admin.userId)));

    // 启用时已经消费了 setup 口令，登录校验不能再用同一枚
    EXPECT_FALSE(fixture.runner.run(fixture.service->verify(fixture.runner.loop(), fixture.admin.userId, enableCode)));
    EXPECT_FALSE(fixture.runner.run(fixture.service->verify(fixture.runner.loop(), fixture.admin.userId, "abcdef")));

    EXPECT_TRUE(fixture.runner.run(fixture.service->verifyRecovery(fixture.runner.loop(), fixture.admin.userId, recoveryCodes->front())));
    EXPECT_FALSE(fixture.runner.run(fixture.service->verifyRecovery(fixture.runner.loop(), fixture.admin.userId, recoveryCodes->front())));
    status = fixture.runner.run(fixture.service->status(fixture.runner.loop(), fixture.admin.userId));
    EXPECT_TRUE(status.enabled);
    EXPECT_EQ(9U, status.remainingRecoveryCodes);

    const auto wrongDisable = fixture.runner.run(fixture.service->disable(fixture.runner.loop(), fixture.admin, "abcdef"));
    ASSERT_FALSE(wrongDisable.has_value());
    EXPECT_EQ(401, wrongDisable.error().httpStatus);
    EXPECT_TRUE(fixture.runner.run(fixture.service->enabled(fixture.runner.loop(), fixture.admin.userId)));

    // 关闭使用窗口内尚未消费的下一步，避免与登录校验用过的步号相撞
    const std::int64_t disableStep = std::max(MfaFixture::nowStep(), enableStep + 1);
    const auto         disabled    = fixture.runner.run(fixture.service->disable(fixture.runner.loop(), fixture.admin, EdgePulse::Auth::Totp::code(setup->secret, disableStep)));
    ASSERT_TRUE(disabled.has_value()) << disabled.error().message;

    status = fixture.runner.run(fixture.service->status(fixture.runner.loop(), fixture.admin.userId));
    EXPECT_FALSE(status.enabled);
    EXPECT_EQ(0U, status.remainingRecoveryCodes);
    EXPECT_FALSE(fixture.runner.run(fixture.service->enabled(fixture.runner.loop(), fixture.admin.userId)));
    EXPECT_FALSE(fixture.runner.run(fixture.service->verify(fixture.runner.loop(), fixture.admin.userId, EdgePulse::Auth::Totp::code(setup->secret, MfaFixture::nowStep()))));
}

// 钉住：未配置密钥时开始设置与启用返回 503 文案，状态与校验路径不崩溃
TEST(MfaService, MissingKeyRejectsSetupAndEnable)
{
    MfaFixture fixture;
    MfaService unconfigured(fixture.context.database, fixture.context.cache, "");

    const auto setup = fixture.runner.run(unconfigured.beginSetup(fixture.runner.loop(), fixture.admin));
    ASSERT_FALSE(setup.has_value());
    EXPECT_EQ(503, setup.error().httpStatus);
    EXPECT_NE(std::string::npos, setup.error().message.find("未配置两步验证加密密钥"));

    const auto enabled = fixture.runner.run(unconfigured.enable(fixture.runner.loop(), fixture.admin, "123456"));
    ASSERT_FALSE(enabled.has_value());
    EXPECT_EQ(503, enabled.error().httpStatus);

    const MfaStatus status = fixture.runner.run(unconfigured.status(fixture.runner.loop(), fixture.admin.userId));
    EXPECT_FALSE(status.enabled);
    EXPECT_EQ(0U, status.remainingRecoveryCodes);
    EXPECT_FALSE(fixture.runner.run(unconfigured.enabled(fixture.runner.loop(), fixture.admin.userId)));
    EXPECT_FALSE(fixture.runner.run(unconfigured.verify(fixture.runner.loop(), fixture.admin.userId, "123456")));
    EXPECT_FALSE(fixture.runner.run(unconfigured.verifyRecovery(fixture.runner.loop(), fixture.admin.userId, "abcd-efgh")));
}

// 钉住：管理员重置删除已启用记录，第二次重置返回 false
TEST(MfaService, ResetRemovesEnabledRecord)
{
    MfaFixture fixture;

    const auto setup = fixture.runner.run(fixture.service->beginSetup(fixture.runner.loop(), fixture.admin));
    ASSERT_TRUE(setup.has_value()) << setup.error().message;
    const auto codes = fixture.runner.run(fixture.service->enable(fixture.runner.loop(), fixture.admin, EdgePulse::Auth::Totp::code(setup->secret, MfaFixture::nowStep())));
    ASSERT_TRUE(codes.has_value()) << codes.error().message;
    EXPECT_TRUE(fixture.runner.run(fixture.service->enabled(fixture.runner.loop(), fixture.admin.userId)));

    EXPECT_TRUE(fixture.runner.run(fixture.service->reset(fixture.runner.loop(), fixture.admin.userId)));
    EXPECT_FALSE(fixture.runner.run(fixture.service->enabled(fixture.runner.loop(), fixture.admin.userId)));
    EXPECT_FALSE(fixture.runner.run(fixture.service->reset(fixture.runner.loop(), fixture.admin.userId)));
}

// 钉住：密钥不是 base64 的 32 字节时按未配置处理，管理员重置仍可清掉残留行
TEST(MfaService, InvalidKeyBehavesAsUnconfiguredAndResetClearsRow)
{
    MfaFixture fixture;
    MfaService brokenKey(fixture.context.database, fixture.context.cache, "not-a-valid-key");

    const auto setup = fixture.runner.run(brokenKey.beginSetup(fixture.runner.loop(), fixture.admin));
    ASSERT_FALSE(setup.has_value());
    EXPECT_EQ(503, setup.error().httpStatus);

    const bool removed = fixture.runner.run(brokenKey.reset(fixture.runner.loop(), fixture.admin.userId));
    EXPECT_FALSE(removed);
}

// 钉住：登录校验消费过的步号不能再用于关闭两步验证
TEST(MfaService, DisableRejectsCodeConsumedByVerify)
{
    MfaFixture fixture;

    const auto setup = fixture.runner.run(fixture.service->beginSetup(fixture.runner.loop(), fixture.admin));
    ASSERT_TRUE(setup.has_value()) << setup.error().message;
    const std::int64_t setupStep = MfaFixture::nowStep();
    const auto codes = fixture.runner.run(fixture.service->enable(fixture.runner.loop(), fixture.admin, EdgePulse::Auth::Totp::code(setup->secret, setupStep)));
    ASSERT_TRUE(codes.has_value()) << codes.error().message;

    const std::int64_t loginStep = std::max(MfaFixture::nowStep(), setupStep + 1);
    const std::string  loginCode = EdgePulse::Auth::Totp::code(setup->secret, loginStep);
    ASSERT_TRUE(fixture.runner.run(fixture.service->verify(fixture.runner.loop(), fixture.admin.userId, loginCode)));

    const auto disabled = fixture.runner.run(fixture.service->disable(fixture.runner.loop(), fixture.admin, loginCode));
    ASSERT_FALSE(disabled.has_value());
    EXPECT_EQ(401, disabled.error().httpStatus);
    EXPECT_TRUE(fixture.runner.run(fixture.service->enabled(fixture.runner.loop(), fixture.admin.userId)));
}

// 钉住：恢复码存的是以主密钥为键的 HMAC-SHA256，不是明文，也不是无键 SHA-256
TEST(MfaService, RecoveryCodesAreHmacHashedNotPlaintext)
{
    MfaFixture fixture;

    const auto setup = fixture.runner.run(fixture.service->beginSetup(fixture.runner.loop(), fixture.admin));
    ASSERT_TRUE(setup.has_value()) << setup.error().message;
    const auto recoveryCodes =
            fixture.runner.run(fixture.service->enable(fixture.runner.loop(), fixture.admin, EdgePulse::Auth::Totp::code(setup->secret, MfaFixture::nowStep())));
    ASSERT_TRUE(recoveryCodes.has_value()) << recoveryCodes.error().message;

    const std::string json = readRecoveryJson(fixture.context, fixture.admin.userId);
    const std::optional<AsynGyanis::Base::ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(json);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->is_array());
    std::vector<std::string> hashes;
    for (const AsynGyanis::Base::ConfigValue &item: *parsed)
    {
        hashes.push_back(item.get<std::string>());
    }
    ASSERT_EQ(recoveryCodes->size(), hashes.size());

    for (const std::string &recoveryCode: *recoveryCodes)
    {
        const std::string expected = EdgePulse::Common::hmacSha256Hex(rawMfaKey(), recoveryCode);
        EXPECT_EQ(64U, expected.size());
        EXPECT_NE(hashes.end(), std::find(hashes.begin(), hashes.end(), expected)) << recoveryCode;
        EXPECT_EQ(std::string::npos, json.find(recoveryCode));
    }

    EXPECT_TRUE(fixture.runner.run(fixture.service->verifyRecovery(fixture.runner.loop(), fixture.admin.userId, recoveryCodes->front())));
    EXPECT_FALSE(fixture.runner.run(fixture.service->verifyRecovery(fixture.runner.loop(), fixture.admin.userId, recoveryCodes->front())));
}

// 钉住：待启用秘密在缓存里是密文，只有持有主密钥才能还原
TEST(MfaService, PendingSetupSecretIsEncryptedInCache)
{
    MfaFixture fixture;

    const auto setup = fixture.runner.run(fixture.service->beginSetup(fixture.runner.loop(), fixture.admin));
    ASSERT_TRUE(setup.has_value()) << setup.error().message;

    const std::optional<std::string> cached = fixture.runner.run(fixture.context.cache->get(fixture.runner.loop(), setupCacheKey(fixture.admin.userId)));
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(std::string::npos, cached->find(setup->secret));

    const std::optional<AsynGyanis::Base::ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(*cached);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->is_object());
    const std::string nonce      = parsed->value("nonce", std::string{});
    const std::string ciphertext = parsed->value("ciphertext", std::string{});
    const std::optional<std::string> decrypted = EdgePulse::Auth::MfaCrypto::decrypt(ciphertext, nonce, rawMfaKey());
    ASSERT_TRUE(decrypted.has_value());
    EXPECT_EQ(setup->secret, *decrypted);
}

// 钉住：两个启用流程并发时只有一方命中条件更新，密文与恢复码不会来自两方
TEST(MfaService, ConcurrentEnableKeepsWinnerSecretAndRecoveryCodes)
{
    MfaFixture fixture;

    // 两个独立缓存模拟两套设置流程：待启用秘密不同，最后写入的停用行属于流程 B
    auto                cacheA   = std::make_shared<EdgePulse::Cache::MemoryKeyValueStore>();
    auto                cacheB   = std::make_shared<EdgePulse::Cache::MemoryKeyValueStore>();
    EdgePulse::Auth::MfaService serviceA(fixture.context.database, cacheA, encodedMfaKey());
    EdgePulse::Auth::MfaService serviceB(fixture.context.database, cacheB, encodedMfaKey());

    const auto setupA = fixture.runner.run(serviceA.beginSetup(fixture.runner.loop(), fixture.admin));
    ASSERT_TRUE(setupA.has_value()) << setupA.error().message;
    const auto setupB = fixture.runner.run(serviceB.beginSetup(fixture.runner.loop(), fixture.admin));
    ASSERT_TRUE(setupB.has_value()) << setupB.error().message;
    ASSERT_NE(setupA->secret, setupB->secret);

    using EnableOutcome = std::expected<std::vector<std::string>, EdgePulse::Auth::MfaError>;
    auto first          = std::make_shared<std::promise<EnableOutcome>>();
    auto second         = std::make_shared<std::promise<EnableOutcome>>();
    auto firstResult    = first->get_future();
    auto secondResult   = second->get_future();

    const std::int64_t step   = MfaFixture::nowStep();
    AsynGyanis::Core::Task<> firstTask  = forwardInto(serviceA.enable(fixture.runner.loop(), fixture.admin, EdgePulse::Auth::Totp::code(setupA->secret, step)), first);
    AsynGyanis::Core::Task<> secondTask = forwardInto(serviceB.enable(fixture.runner.loop(), fixture.admin, EdgePulse::Auth::Totp::code(setupB->secret, step)), second);
    fixture.runner.loop().scheduler().scheduleRemote(firstTask.handle());
    fixture.runner.loop().scheduler().scheduleRemote(secondTask.handle());

    ASSERT_EQ(std::future_status::ready, firstResult.wait_for(std::chrono::seconds{10}));
    ASSERT_EQ(std::future_status::ready, secondResult.wait_for(std::chrono::seconds{10}));
    const EnableOutcome outcomeA = firstResult.get();
    const EnableOutcome outcomeB = secondResult.get();
    ASSERT_NE(outcomeA.has_value(), outcomeB.has_value()) << "并发启用必须一方成功、一方 409";

    const EnableOutcome &winner       = outcomeA.has_value() ? outcomeA : outcomeB;
    const EnableOutcome &loser        = outcomeA.has_value() ? outcomeB : outcomeA;
    const std::string   &winnerSecret = outcomeA.has_value() ? setupA->secret : setupB->secret;
    ASSERT_FALSE(loser.has_value());
    EXPECT_EQ(409, loser.error().httpStatus);

    const std::optional<StoredMfaRow> row = readMfaRow(fixture.context, fixture.admin.userId);
    ASSERT_TRUE(row.has_value());
    EXPECT_TRUE(row->enabled);
    const std::optional<std::string> decrypted = EdgePulse::Auth::MfaCrypto::decrypt(row->secretCiphertext, row->secretNonce, rawMfaKey());
    ASSERT_TRUE(decrypted.has_value());
    EXPECT_EQ(winnerSecret, *decrypted) << "落库密文必须来自获胜流程";

    const std::vector<std::string> storedHashes = parseHashes(row->recoveryJson);
    ASSERT_EQ(winner->size(), storedHashes.size());
    std::unordered_set<std::string> expectedHashes;
    for (const std::string &code: *winner)
    {
        expectedHashes.insert(EdgePulse::Common::hmacSha256Hex(rawMfaKey(), code));
    }
    const std::unordered_set<std::string> actualHashes(storedHashes.begin(), storedHashes.end());
    EXPECT_EQ(expectedHashes, actualHashes) << "恢复码也必须来自获胜流程";
}

// 钉住：启用失败按用户累计，达到 5 次后正确验证码也被 429 挡住；管理员重置清空计数
TEST(MfaService, EnableAttemptLimitAcrossRetries)
{
    MfaFixture fixture;

    const auto         setup = fixture.runner.run(fixture.service->beginSetup(fixture.runner.loop(), fixture.admin));
    ASSERT_TRUE(setup.has_value()) << setup.error().message;
    for (int attempt = 0; attempt < MfaService::kAttemptMaxFailures; ++attempt)
    {
        const auto wrong = fixture.runner.run(fixture.service->enable(fixture.runner.loop(), fixture.admin, "abcdef"));
        ASSERT_FALSE(wrong.has_value()) << "attempt " << attempt;
        EXPECT_EQ(401, wrong.error().httpStatus) << "attempt " << attempt;
    }
    const auto blocked =
            fixture.runner.run(fixture.service->enable(fixture.runner.loop(), fixture.admin, EdgePulse::Auth::Totp::code(setup->secret, MfaFixture::nowStep())));
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(429, blocked.error().httpStatus);

    // 管理员重置同时清空失败计数，重新开始设置后正确验证码可以启用
    EXPECT_TRUE(fixture.runner.run(fixture.service->reset(fixture.runner.loop(), fixture.admin.userId)));
    EXPECT_FALSE(fixture.runner.run(fixture.service->attemptLimitReached(fixture.runner.loop(), fixture.admin.userId)));
    const auto restarted = fixture.runner.run(fixture.service->beginSetup(fixture.runner.loop(), fixture.admin));
    ASSERT_TRUE(restarted.has_value()) << restarted.error().message;
    const auto enabled =
            fixture.runner.run(fixture.service->enable(fixture.runner.loop(), fixture.admin, EdgePulse::Auth::Totp::code(restarted->secret, MfaFixture::nowStep())));
    ASSERT_TRUE(enabled.has_value()) << enabled.error().message;
    EXPECT_EQ(10U, enabled->size());
}

// 钉住：仓储的恢复码 CAS 只在 enabled 行上生效，停用期间不消费
TEST(MfaService, RecoveryCasRequiresEnabledRow)
{
    MfaFixture fixture;

    const auto setup = fixture.runner.run(fixture.service->beginSetup(fixture.runner.loop(), fixture.admin));
    ASSERT_TRUE(setup.has_value()) << setup.error().message;
    const auto recoveryCodes =
            fixture.runner.run(fixture.service->enable(fixture.runner.loop(), fixture.admin, EdgePulse::Auth::Totp::code(setup->secret, MfaFixture::nowStep())));
    ASSERT_TRUE(recoveryCodes.has_value()) << recoveryCodes.error().message;

    const std::string before = readRecoveryJson(fixture.context, fixture.admin.userId);
    const std::optional<AsynGyanis::Base::ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(before);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->is_array());
    ASSERT_FALSE(parsed->empty());
    const std::string storedHash = parsed->front().get<std::string>();

    const std::string userIdText = std::to_string(fixture.admin.userId);
    executeOrFail(fixture.context, "UPDATE sys_user_mfa SET enabled = 0 WHERE user_id = " + userIdText);

    auto repository = EdgePulse::Auth::MfaRepository(fixture.context.database, TenantScope{1, false});
    EXPECT_FALSE(fixture.runner.run(repository.consumeRecoveryCode(fixture.runner.loop(), fixture.admin.userId, storedHash, EdgePulse::Common::nowDateTimeUtc())));
    EXPECT_EQ(before, readRecoveryJson(fixture.context, fixture.admin.userId));

    executeOrFail(fixture.context, "UPDATE sys_user_mfa SET enabled = 1 WHERE user_id = " + userIdText);
    EXPECT_TRUE(fixture.runner.run(repository.consumeRecoveryCode(fixture.runner.loop(), fixture.admin.userId, storedHash, EdgePulse::Common::nowDateTimeUtc())));
    EXPECT_NE(before, readRecoveryJson(fixture.context, fixture.admin.userId));
}
