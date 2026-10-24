#include "Base/Coding/Base64.h"
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

    [[nodiscard]] std::string encodedMfaKey()
    {
        std::string key;
        key.reserve(32);
        for (int index = 0; index < 32; ++index)
        {
            key.push_back(static_cast<char>(index + 1));
        }
        return AsynGyanis::Base::base64Encode(key);
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

    EXPECT_TRUE(fixture.runner.run(fixture.service->verify(fixture.runner.loop(), fixture.admin.userId, enableCode)));
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
