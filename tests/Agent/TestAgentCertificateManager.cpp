#include "AgentCertificateManager.h"

#include "AgentConfiguration.h"
#include "AgentIdentity.h"
#include "CertificateFixture.h"
#include "TestSupport/TemporaryDirectory.h"

#include <gtest/gtest.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace EdgePulse::Agent
{
    namespace
    {
        using BioHandle     = std::unique_ptr<BIO, decltype(&BIO_free)>;
        using RequestHandle = std::unique_ptr<X509_REQ, decltype(&X509_REQ_free)>;
        using KeyHandle     = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

        /**
         * @brief 构造一套指向临时目录的身份配置
         * @param directory 临时目录
         * @param caFile 信任锚路径
         * @return AgentConfiguration 证书、私钥落在 agent.pem 与 agent-key.pem
         */
        [[nodiscard]] AgentConfiguration managerConfiguration(const TestSupport::TemporaryDirectory &directory, const std::filesystem::path &caFile)
        {
            AgentConfiguration configuration;
            configuration.caFile          = caFile;
            configuration.certificateFile = directory.path() / "agent.pem";
            configuration.privateKeyFile  = directory.path() / "agent-key.pem";
            configuration.name            = "edge-01";
            return configuration;
        }

        /**
         * @brief 读取整个文件
         * @param path 文件路径
         * @return std::string 文件内容；打不开时为空
         */
        [[nodiscard]] std::string readText(const std::filesystem::path &path)
        {
            std::ifstream stream(path, std::ios::binary);
            return std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        }

        /**
         * @brief 解析 CSR 并校验自签与 EC P-256
         * @param csrPem CSR 文本
         * @return bool 校验通过返回 true
         */
        [[nodiscard]] bool isEcP256CertificateRequest(const std::string &csrPem)
        {
            BioHandle csrBio(BIO_new_mem_buf(csrPem.data(), static_cast<int>(csrPem.size())), &BIO_free);
            if (!csrBio)
            {
                return false;
            }
            RequestHandle request(PEM_read_bio_X509_REQ(csrBio.get(), nullptr, nullptr, nullptr), &X509_REQ_free);
            if (!request)
            {
                return false;
            }
            KeyHandle key(X509_REQ_get_pubkey(request.get()), &EVP_PKEY_free);
            if (!key)
            {
                return false;
            }
            return X509_REQ_verify(request.get(), key.get()) == 1 && EVP_PKEY_base_id(key.get()) == EVP_PKEY_EC && EVP_PKEY_bits(key.get()) == 256;
        }

        /**
         * @brief 改掉证书 PEM 签名段的最后一个 base64 字符，得到可解析但验签失败的证书
         * @param certificatePem 原证书
         * @return std::string 篡改后的 PEM；结构不符时为空
         * @note 直接改内存里的 X509 主题不会改变输出：OpenSSL 会复用解析时缓存的 TBS 编码
         */
        [[nodiscard]] std::string tamperCertificateSignature(const std::string &certificatePem)
        {
            const std::string endMarker   = "-----END CERTIFICATE-----";
            const auto        endPosition = certificatePem.rfind(endMarker);
            if (endPosition == std::string::npos || endPosition == 0)
            {
                return {};
            }
            auto lineEnd = certificatePem.rfind('\n', endPosition);
            if (lineEnd == std::string::npos)
            {
                return {};
            }
            auto lineBegin = certificatePem.rfind('\n', lineEnd - 1);
            lineBegin      = lineBegin == std::string::npos ? 0 : lineBegin + 1;
            if (lineBegin >= lineEnd)
            {
                return {};
            }
            std::string tampered = certificatePem;
            tampered[lineBegin]  = tampered[lineBegin] == 'A' ? 'B' : 'A';
            return tampered;
        }
    } // namespace

    TEST(AgentCertificateManager, GeneratesEcP256KeyAndSignedCsr)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      authority = Testing::writeCertificateAuthority(directory, "ca");

        AgentCertificateManager manager(managerConfiguration(directory, authority.certificateFile));
        const auto              request = manager.generateKeyAndCsr();
        ASSERT_TRUE(request.has_value()) << request.error();

        EXPECT_NE(std::string::npos, request->privateKeyPem.find("BEGIN PRIVATE KEY"));
        EXPECT_NE(std::string::npos, request->csrPem.find("BEGIN CERTIFICATE REQUEST"));
        EXPECT_TRUE(isEcP256CertificateRequest(request->csrPem));
    }

    TEST(AgentCertificateManager, InstallsSignedCertificateAndReloadsIdentity)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      authority = Testing::writeCertificateAuthority(directory, "ca");
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        AgentCertificateManager manager(managerConfiguration(directory, authority.certificateFile));
        const auto              request = manager.generateKeyAndCsr();
        ASSERT_TRUE(request.has_value()) << request.error();

        const auto issued    = Testing::signCertificateRequest(authority, request->csrPem, 90L * 24L * 60L * 60L);
        const auto installed = manager.install(issued.certificatePem);
        ASSERT_TRUE(installed.has_value()) << installed.error();
        EXPECT_EQ(issued.fingerprint, installed->fingerprint);
        EXPECT_EQ(issued.expiresAt, installed->expiresAt);

        const auto loaded = AgentIdentity::load(managerConfiguration(directory, authority.certificateFile));
        ASSERT_TRUE(loaded.has_value()) << loaded.error();
        EXPECT_EQ(issued.fingerprint, loaded->fingerprint);
        EXPECT_EQ(issued.expiresAt, loaded->expiresAt);

        const auto reloaded = AgentIdentity::reload(managerConfiguration(directory, authority.certificateFile));
        ASSERT_TRUE(reloaded.has_value()) << reloaded.error();
        EXPECT_EQ(issued.fingerprint, reloaded->fingerprint);

#ifndef _WIN32
        struct stat metadata{};
        ASSERT_EQ(0, ::stat((directory.path() / "agent-key.pem").string().c_str(), &metadata));
        EXPECT_EQ(static_cast<mode_t>(S_IRUSR | S_IWUSR), metadata.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO));
#endif
    }

    TEST(AgentCertificateManager, RejectsCertificateFromAnotherCaAndKeepsFiles)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      trustedAuthority   = Testing::writeCertificateAuthority(directory, "trusted");
        const auto                      otherAuthority     = Testing::writeCertificateAuthority(directory, "other");
        const auto                      initial            = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        const std::string               initialCertificate = readText(initial.certificateFile);
        const std::string               initialKey         = readText(initial.privateKeyFile);

        AgentCertificateManager manager(managerConfiguration(directory, trustedAuthority.certificateFile));
        const auto              request = manager.generateKeyAndCsr();
        ASSERT_TRUE(request.has_value()) << request.error();
        const auto issued = Testing::signCertificateRequest(otherAuthority, request->csrPem, 90L * 24L * 60L * 60L);

        const auto installed = manager.install(issued.certificatePem);
        ASSERT_FALSE(installed.has_value());
        EXPECT_NE(std::string::npos, installed.error().find("CA"));
        EXPECT_EQ(initialCertificate, readText(initial.certificateFile));
        EXPECT_EQ(initialKey, readText(initial.privateKeyFile));
    }

    TEST(AgentCertificateManager, RejectsCertificateMismatchingPendingKeyAndKeepsFiles)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      authority          = Testing::writeCertificateAuthority(directory, "ca");
        const auto                      initial            = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        const std::string               initialCertificate = readText(initial.certificateFile);
        const std::string               initialKey         = readText(initial.privateKeyFile);

        AgentCertificateManager foreignManager(managerConfiguration(directory, authority.certificateFile));
        const auto              foreignRequest = foreignManager.generateKeyAndCsr();
        ASSERT_TRUE(foreignRequest.has_value()) << foreignRequest.error();
        const auto issued = Testing::signCertificateRequest(authority, foreignRequest->csrPem, 90L * 24L * 60L * 60L);

        AgentCertificateManager manager(managerConfiguration(directory, authority.certificateFile));
        const auto              ownRequest = manager.generateKeyAndCsr();
        ASSERT_TRUE(ownRequest.has_value()) << ownRequest.error();

        const auto installed = manager.install(issued.certificatePem);
        ASSERT_FALSE(installed.has_value());
        EXPECT_NE(std::string::npos, installed.error().find("私钥"));
        EXPECT_EQ(initialCertificate, readText(initial.certificateFile));
        EXPECT_EQ(initialKey, readText(initial.privateKeyFile));
    }

    TEST(AgentCertificateManager, RejectsExpiredCertificateAndKeepsFiles)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      authority          = Testing::writeCertificateAuthority(directory, "ca");
        const auto                      initial            = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        const std::string               initialCertificate = readText(initial.certificateFile);
        const std::string               initialKey         = readText(initial.privateKeyFile);

        AgentCertificateManager manager(managerConfiguration(directory, authority.certificateFile));
        const auto              request = manager.generateKeyAndCsr();
        ASSERT_TRUE(request.has_value()) << request.error();
        const auto issued = Testing::signCertificateRequest(authority, request->csrPem, -3600L);

        const auto installed = manager.install(issued.certificatePem);
        ASSERT_FALSE(installed.has_value());
        EXPECT_NE(std::string::npos, installed.error().find("过期"));
        EXPECT_EQ(initialCertificate, readText(initial.certificateFile));
        EXPECT_EQ(initialKey, readText(initial.privateKeyFile));
    }

    TEST(AgentCertificateManager, RejectsTamperedCertificateAndKeepsFiles)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      authority          = Testing::writeCertificateAuthority(directory, "ca");
        const auto                      initial            = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        const std::string               initialCertificate = readText(initial.certificateFile);
        const std::string               initialKey         = readText(initial.privateKeyFile);

        AgentCertificateManager manager(managerConfiguration(directory, authority.certificateFile));
        const auto              request = manager.generateKeyAndCsr();
        ASSERT_TRUE(request.has_value()) << request.error();
        const auto issued   = Testing::signCertificateRequest(authority, request->csrPem, 90L * 24L * 60L * 60L);
        const auto tampered = tamperCertificateSignature(issued.certificatePem);
        ASSERT_FALSE(tampered.empty());
        ASSERT_NE(issued.certificatePem, tampered);

        const auto installed = manager.install(tampered);
        ASSERT_FALSE(installed.has_value());
        EXPECT_EQ(initialCertificate, readText(initial.certificateFile));
        EXPECT_EQ(initialKey, readText(initial.privateKeyFile));
    }

    TEST(AgentCertificateManager, RestoresPreviousMaterialAfterSuccessfulInstall)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      authority          = Testing::writeCertificateAuthority(directory, "ca");
        const auto                      initial            = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        const std::string               initialCertificate = readText(initial.certificateFile);
        const std::string               initialKey         = readText(initial.privateKeyFile);

        AgentCertificateManager manager(managerConfiguration(directory, authority.certificateFile));
        const auto              request = manager.generateKeyAndCsr();
        ASSERT_TRUE(request.has_value()) << request.error();
        const auto issued = Testing::signCertificateRequest(authority, request->csrPem, 90L * 24L * 60L * 60L);

        const auto installed = manager.install(issued.certificatePem);
        ASSERT_TRUE(installed.has_value()) << installed.error();
        EXPECT_NE(initialCertificate, readText(initial.certificateFile));

        const auto restored = manager.restorePrevious();
        ASSERT_TRUE(restored.has_value()) << restored.error();
        EXPECT_EQ(initialCertificate, readText(initial.certificateFile));
        EXPECT_EQ(initialKey, readText(initial.privateKeyFile));

        const auto reloaded = AgentIdentity::reload(managerConfiguration(directory, authority.certificateFile));
        ASSERT_TRUE(reloaded.has_value()) << reloaded.error();
        EXPECT_EQ(initial.fingerprint, reloaded->fingerprint);
    }
} // namespace EdgePulse::Agent
