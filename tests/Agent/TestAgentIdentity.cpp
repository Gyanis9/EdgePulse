#include "AgentIdentity.h"

#include "AgentConfiguration.h"
#include "CertificateFixture.h"
#include "TestSupport/TemporaryDirectory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace EdgePulse::Agent
{
    namespace
    {
        /**
         * @brief 用生成的证书材料拼出身份加载需要的配置
         * @param material 证书材料
         * @return AgentConfiguration 只填证书相关路径的配置
         */
        [[nodiscard]] AgentConfiguration configurationFor(const Testing::CertificateMaterial &material)
        {
            AgentConfiguration configuration;
            configuration.caFile          = material.certificateFile;
            configuration.certificateFile = material.certificateFile;
            configuration.privateKeyFile  = material.privateKeyFile;
            configuration.name            = "edge-01";
            return configuration;
        }

        /**
         * @brief 判断文本是否全为小写十六进制
         * @param text 待检查文本
         * @return bool 全部字符都在 0 到 9、a 到 f 之间
         */
        [[nodiscard]] bool isLowerHex(const std::string &text)
        {
            return std::all_of(text.begin(), text.end(), [](const unsigned char character) { return std::isdigit(character) != 0 || (character >= 'a' && character <= 'f'); });
        }
    } // namespace

    TEST(AgentIdentity, LoadsLowercaseFingerprintAndExpiry)
    {
        TestSupport::TemporaryDirectory    directory;
        const Testing::CertificateMaterial material = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");

        const auto identity = AgentIdentity::load(configurationFor(material));
        ASSERT_TRUE(identity.has_value()) << identity.error();
        EXPECT_EQ(64U, identity->fingerprint.size());
        EXPECT_TRUE(isLowerHex(identity->fingerprint));
        EXPECT_EQ(material.fingerprint, identity->fingerprint);
        EXPECT_EQ(material.expiresAt, identity->expiresAt);
    }

    TEST(AgentIdentity, AllowsExpiredCertificate)
    {
        TestSupport::TemporaryDirectory    directory;
        const Testing::CertificateMaterial material = Testing::writeSelfSignedCertificate(directory, -24L * 60L * 60L, "expired");

        const auto identity = AgentIdentity::load(configurationFor(material));
        ASSERT_TRUE(identity.has_value()) << identity.error();
        EXPECT_EQ(material.fingerprint, identity->fingerprint);
        EXPECT_EQ(material.expiresAt, identity->expiresAt);
    }

    TEST(AgentIdentity, RejectsMissingPrivateKey)
    {
        TestSupport::TemporaryDirectory directory;
        Testing::CertificateMaterial    material      = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        AgentConfiguration              configuration = configurationFor(material);
        configuration.privateKeyFile                  = directory.path() / "missing-key.pem";

        const auto identity = AgentIdentity::load(configuration);
        ASSERT_FALSE(identity.has_value());
        EXPECT_NE(std::string::npos, identity.error().find("私钥"));
        EXPECT_NE(std::string::npos, identity.error().find("不存在"));
    }

    TEST(AgentIdentity, RejectsMalformedCertificate)
    {
        TestSupport::TemporaryDirectory directory;
        Testing::CertificateMaterial    material      = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        AgentConfiguration              configuration = configurationFor(material);
        configuration.certificateFile                 = directory.write("broken.pem", "this is not a certificate\n");

        const auto identity = AgentIdentity::load(configuration);
        ASSERT_FALSE(identity.has_value());
        EXPECT_NE(std::string::npos, identity.error().find("证书"));
        EXPECT_NE(std::string::npos, identity.error().find("解析"));
    }

    TEST(AgentIdentity, RejectsGroupOrWorldReadablePrivateKeyOnPosix)
    {
#ifdef _WIN32
        GTEST_SKIP() << "Windows 只记录私钥 ACL 警告，不做拒绝";
#else
        TestSupport::TemporaryDirectory    directory;
        const Testing::CertificateMaterial material = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        ASSERT_EQ(0, ::chmod(material.privateKeyFile.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH));

        const auto identity = AgentIdentity::load(configurationFor(material));
        ASSERT_FALSE(identity.has_value());
        EXPECT_NE(std::string::npos, identity.error().find("私钥"));
        EXPECT_NE(std::string::npos, identity.error().find("权限"));
#endif
    }
} // namespace EdgePulse::Agent
