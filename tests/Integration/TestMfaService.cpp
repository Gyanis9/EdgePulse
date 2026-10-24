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
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
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
