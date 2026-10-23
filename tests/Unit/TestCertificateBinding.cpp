#include "Core/Device/CertificateBinding.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
    [[nodiscard]] AsynGyanis::Core::TlsPeerIdentityPtr makeIdentity(const std::string &fingerprint, const bool verified = true)
    {
        return std::make_shared<const AsynGyanis::Core::TlsPeerIdentity>(verified, std::vector<std::uint8_t>{}, fingerprint);
    }
} // namespace

// 钉住：非 TLS 链路不做绑定，开发与测试的明文监听器照常工作
TEST(CertificateBinding, NonTlsConnectionPasses)
{
    EXPECT_FALSE(EdgePulse::Device::certificateBindingFailure(nullptr, ""));
    EXPECT_FALSE(EdgePulse::Device::certificateBindingFailure(nullptr, std::string(64, 'a')));
}

// 钉住：登记值与连接证书一致（大小写不敏感）时放行
TEST(CertificateBinding, MatchingCertificatePasses)
{
    const AsynGyanis::Core::TlsPeerIdentityPtr identity = makeIdentity(std::string(64, 'a'));
    EXPECT_FALSE(EdgePulse::Device::certificateBindingFailure(identity, std::string(64, 'A')));
}

// 钉住：连接证书与登记值不同即拒绝
TEST(CertificateBinding, MismatchedCertificateIsRejected)
{
    const AsynGyanis::Core::TlsPeerIdentityPtr identity = makeIdentity(std::string(64, 'a'));
    const std::optional<std::string>           failure  = EdgePulse::Device::certificateBindingFailure(identity, std::string(64, 'b'));
    ASSERT_TRUE(failure.has_value());
    EXPECT_NE(std::string::npos, failure->find("不一致"));
}

// 钉住：TLS 连接上没有可用登记值时拒绝
TEST(CertificateBinding, MissingRegistrationIsRejected)
{
    const AsynGyanis::Core::TlsPeerIdentityPtr identity = makeIdentity(std::string(64, 'a'));
    for (const std::string &registered: {std::string{}, std::string(63, 'a'), std::string(64, 'g')})
    {
        const std::optional<std::string> failure = EdgePulse::Device::certificateBindingFailure(identity, registered);
        ASSERT_TRUE(failure.has_value()) << registered;
        EXPECT_NE(std::string::npos, failure->find("登记"));
    }
}

// 钉住：未通过校验的证书不参与绑定
TEST(CertificateBinding, UnverifiedCertificateIsRejected)
{
    const AsynGyanis::Core::TlsPeerIdentityPtr identity = makeIdentity(std::string(64, 'a'), false);
    const std::optional<std::string>           failure  = EdgePulse::Device::certificateBindingFailure(identity, std::string(64, 'a'));
    ASSERT_TRUE(failure.has_value());
    EXPECT_NE(std::string::npos, failure->find("未通过"));
}

// 钉住：归一化只接受 64 位十六进制，并统一成小写
TEST(CertificateBinding, FingerprintNormalization)
{
    EXPECT_FALSE(EdgePulse::Device::normalizeCertificateFingerprint(std::string(63, 'a')).has_value());
    EXPECT_FALSE(EdgePulse::Device::normalizeCertificateFingerprint(std::string(64, 'g')).has_value());
    EXPECT_FALSE(EdgePulse::Device::normalizeCertificateFingerprint(std::string{}).has_value());
    const std::optional<std::string> normalized = EdgePulse::Device::normalizeCertificateFingerprint(std::string(64, 'A'));
    ASSERT_TRUE(normalized.has_value());
    EXPECT_EQ(std::string(64, 'a'), *normalized);
}
