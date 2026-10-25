#include "AgentCertificateManager.h"

#include "AgentConfiguration.h"
#include "AgentIdentity.h"
#include "CertificateFixture.h"
#include "TestSupport/TemporaryDirectory.h"

#include "Platform/FileSystem/FileSystem.h"

#include <gtest/gtest.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
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

        /**
         * @brief 取 CSR 主题里的 CN
         * @param csrPem CSR 文本
         * @return std::string CN；解析失败或没有 CN 时为空
         */
        [[nodiscard]] std::string certificateRequestCommonName(const std::string &csrPem)
        {
            BioHandle csrBio(BIO_new_mem_buf(csrPem.data(), static_cast<int>(csrPem.size())), &BIO_free);
            if (!csrBio)
            {
                return {};
            }
            RequestHandle request(PEM_read_bio_X509_REQ(csrBio.get(), nullptr, nullptr, nullptr), &X509_REQ_free);
            if (!request)
            {
                return {};
            }
            char      buffer[256]{};
            const int length = X509_NAME_get_text_by_NID(X509_REQ_get_subject_name(request.get()), NID_commonName, buffer, sizeof(buffer));
            return length > 0 ? std::string{buffer, static_cast<std::size_t>(length)} : std::string{};
        }

        /**
         * @brief 数一段 PEM 里出现几张证书
         * @param pemText 待检查文本
         * @return int 证书块个数
         */
        [[nodiscard]] int countCertificateBlocks(const std::string &pemText)
        {
            constexpr std::string_view kBeginMarker = "-----BEGIN CERTIFICATE-----";
            int                        count        = 0;
            for (auto position = pemText.find(kBeginMarker); position != std::string::npos; position = pemText.find(kBeginMarker, position + kBeginMarker.size()))
            {
                ++count;
            }
            return count;
        }

        /**
         * @brief 覆盖写一段文本
         * @param path 目标路径
         * @param text 正文
         * @return bool 写入成功
         */
        [[nodiscard]] bool writeTextFile(const std::filesystem::path &path, const std::string &text)
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << text;
            stream.close();
            return static_cast<bool>(stream);
        }

        /**
         * @brief 取安装中断标记的路径
         * @param configuration 身份配置
         * @return std::filesystem::path 标记路径
         */
        [[nodiscard]] std::filesystem::path installMarkerPath(const AgentConfiguration &configuration)
        {
            return configuration.certificateFile.string() + ".install-pending";
        }

        /**
         * @brief 按实现约定拼出安装中断标记的正文
         * @param configuration 身份配置
         * @return std::string 四行路径清单
         */
        [[nodiscard]] std::string installMarkerText(const AgentConfiguration &configuration)
        {
            const auto utf8   = [](const std::filesystem::path &path) { return AsynGyanis::Platform::FileSystem::utf8FromPath(path); };
            const auto backup = [](const std::filesystem::path &path) { return std::filesystem::path{path.string() + ".bak"}; };
            return "certificate=" + utf8(configuration.certificateFile) + "\n" + "certificate_backup=" + utf8(backup(configuration.certificateFile)) + "\n" +
                   "private_key=" + utf8(configuration.privateKeyFile) + "\n" + "private_key_backup=" + utf8(backup(configuration.privateKeyFile)) + "\n";
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
        EXPECT_EQ("edge-01", certificateRequestCommonName(request->csrPem)) << "CSR 主题必须取配置里的节点名";
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

        for (const auto &entry: std::filesystem::directory_iterator(directory.path()))
        {
            EXPECT_EQ(std::string::npos, entry.path().filename().string().find(".private-key-stage.")) << "私钥暂存目录必须在安装结束后清理";
        }

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

        // 一次安装失败不能丢掉待装私钥：上层用同一份 CSR 与证书重试时不需要重新生成密钥
        const auto retried        = Testing::signCertificateRequest(trustedAuthority, request->csrPem, 90L * 24L * 60L * 60L);
        const auto retriedInstall = manager.install(retried.certificatePem);
        ASSERT_TRUE(retriedInstall.has_value()) << retriedInstall.error();
        EXPECT_EQ(retried.fingerprint, retriedInstall->fingerprint);
        EXPECT_NE(initialKey, readText(initial.privateKeyFile));
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
        EXPECT_TRUE(permanentInstallFailure(installed.error())) << "证书与待装私钥不配对属于不可重试错误";
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

    TEST(AgentCertificateManager, InstallsLeafWithIntermediateChain)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      root         = Testing::writeCertificateAuthority(directory, "root");
        const auto                      intermediate = Testing::writeIntermediateCertificateAuthority(directory, "intermediate", root);
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        AgentCertificateManager manager(managerConfiguration(directory, root.certificateFile));
        const auto              request = manager.generateKeyAndCsr();
        ASSERT_TRUE(request.has_value()) << request.error();

        const auto        issued    = Testing::signCertificateRequest(intermediate, request->csrPem, 90L * 24L * 60L * 60L);
        const std::string chain     = issued.certificatePem + readText(intermediate.certificateFile);
        const auto        installed = manager.install(chain);
        ASSERT_TRUE(installed.has_value()) << installed.error();
        EXPECT_EQ(issued.fingerprint, installed->fingerprint);

        const std::string installedChain = readText(managerConfiguration(directory, root.certificateFile).certificateFile);
        EXPECT_EQ(2, countCertificateBlocks(installedChain)) << "叶证书与中间证书都必须落盘";

        const auto loaded = AgentIdentity::load(managerConfiguration(directory, root.certificateFile));
        ASSERT_TRUE(loaded.has_value()) << loaded.error();
        EXPECT_EQ(issued.fingerprint, loaded->fingerprint);
    }

    TEST(AgentCertificateManager, RejectsCertificateWhoseLifetimeDoesNotExceedRenewWindow)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      authority          = Testing::writeCertificateAuthority(directory, "ca");
        const auto                      initial            = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        const std::string               initialCertificate = readText(initial.certificateFile);
        const std::string               initialKey         = readText(initial.privateKeyFile);

        AgentConfiguration configuration         = managerConfiguration(directory, authority.certificateFile);
        configuration.certificateRenewBeforeDays = 7;
        AgentCertificateManager manager(configuration);
        const auto              request = manager.generateKeyAndCsr();
        ASSERT_TRUE(request.has_value()) << request.error();

        // 剩余 5 天会在装好后立刻又进入续期窗口，必须按失败处理而不是反复换证
        const auto issued    = Testing::signCertificateRequest(authority, request->csrPem, 5L * 24L * 60L * 60L);
        const auto installed = manager.install(issued.certificatePem);
        ASSERT_FALSE(installed.has_value());
        EXPECT_NE(std::string::npos, installed.error().find("提前量"));
        EXPECT_NE(std::string::npos, installed.error().find("renew_before_days")) << "错误必须点名要改的配置键";
        EXPECT_TRUE(permanentInstallFailure(installed.error())) << "提前量不小于生命周期属于不可重试的配置错误";
        EXPECT_EQ(initialCertificate, readText(initial.certificateFile));
        EXPECT_EQ(initialKey, readText(initial.privateKeyFile));
    }

    TEST(AgentCertificateManager, AcceptsTwoDayCertificateWithSpecWindow)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      authority = Testing::writeCertificateAuthority(directory, "ca");
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        AgentConfiguration configuration         = managerConfiguration(directory, authority.certificateFile);
        configuration.certificateRenewBeforeDays = 0;
        AgentCertificateManager manager(configuration);
        const auto              request = manager.generateKeyAndCsr();
        ASSERT_TRUE(request.has_value()) << request.error();

        // 2 天证书的规范窗口是 max(25% × 2d, 24h) = 24h，剩余 2 天足以安装
        const auto issued    = Testing::signCertificateRequest(authority, request->csrPem, 2L * 24L * 60L * 60L);
        const auto installed = manager.install(issued.certificatePem);
        ASSERT_TRUE(installed.has_value()) << installed.error();
        EXPECT_EQ(issued.fingerprint, installed->fingerprint);
        EXPECT_FALSE(installed->notBefore.empty()) << "安装结果必须带出生效时间，供下一轮计算窗口";
    }

    TEST(AgentCertificateManager, TrustsSeparateServerAndAgentCaInBundle)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      serverCa = Testing::writeCertificateAuthority(directory, "server");
        const auto                      agentCa  = Testing::writeCertificateAuthority(directory, "agent-ca");
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        // 独立 CA 部署：ca_file 同时装 9443 服务器链根与签发 Agent 证书的 CA
        const auto              bundle = directory.write("bundle.pem", readText(serverCa.certificateFile) + readText(agentCa.certificateFile));
        AgentCertificateManager manager(managerConfiguration(directory, bundle));
        const auto              request = manager.generateKeyAndCsr();
        ASSERT_TRUE(request.has_value()) << request.error();
        const auto issued    = Testing::signCertificateRequest(agentCa, request->csrPem, 90L * 24L * 60L * 60L);
        const auto installed = manager.install(issued.certificatePem);
        ASSERT_TRUE(installed.has_value()) << installed.error();
        EXPECT_EQ(issued.fingerprint, installed->fingerprint);

        // 只放服务器根时续期证书链无法落地，必须按永久安装错误停下
        AgentCertificateManager serverOnlyManager(managerConfiguration(directory, serverCa.certificateFile));
        const auto              nextRequest = serverOnlyManager.generateKeyAndCsr();
        ASSERT_TRUE(nextRequest.has_value()) << nextRequest.error();
        const auto nextIssued = Testing::signCertificateRequest(agentCa, nextRequest->csrPem, 90L * 24L * 60L * 60L);
        const auto rejected   = serverOnlyManager.install(nextIssued.certificatePem);
        ASSERT_FALSE(rejected.has_value());
        EXPECT_NE(std::string::npos, rejected.error().find("无法链接到配置的 CA"));
        EXPECT_TRUE(permanentInstallFailure(rejected.error()));
    }

    TEST(AgentCertificateManager, RejectsResponseWithNonCertificatePemBlock)
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

        // 私钥块混进证书响应：旧解析只挑 CERTIFICATE 块，会让这份材料通过并原样落盘
        const std::string response  = request->privateKeyPem + issued.certificatePem;
        const auto        installed = manager.install(response);
        ASSERT_FALSE(installed.has_value());
        EXPECT_NE(std::string::npos, installed.error().find("非证书"));
        EXPECT_EQ(initialCertificate, readText(initial.certificateFile));
        EXPECT_EQ(initialKey, readText(initial.privateKeyFile));
    }

    TEST(AgentCertificateManager, WritesOnlyParsedCertificateChain)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      authority = Testing::writeCertificateAuthority(directory, "ca");
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));

        AgentCertificateManager manager(managerConfiguration(directory, authority.certificateFile));
        const auto              request = manager.generateKeyAndCsr();
        ASSERT_TRUE(request.has_value()) << request.error();
        const auto issued = Testing::signCertificateRequest(authority, request->csrPem, 90L * 24L * 60L * 60L);

        // 响应里夹带说明文字时仍按证书块解析；写盘只写解析出的链，不回抄响应原文
        const std::string response  = "证书链如下\n" + issued.certificatePem + "以上为全部证书\n";
        const auto        installed = manager.install(response);
        ASSERT_TRUE(installed.has_value()) << installed.error();

        const std::string installedChain = readText(managerConfiguration(directory, authority.certificateFile).certificateFile);
        EXPECT_EQ(std::string::npos, installedChain.find("证书链如下")) << "落盘的只能是解析出的证书块";
        EXPECT_EQ(0U, installedChain.find("-----BEGIN CERTIFICATE-----"));
        EXPECT_EQ(1, countCertificateBlocks(installedChain));
    }

    TEST(AgentCertificateManager, RecoversInterruptedInstallWhenPairDoesNotLoad)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      initial        = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        const auto                      next           = Testing::writeSelfSignedCertificate(directory, 60L * 24L * 60L * 60L, "next");
        const std::string               oldCertificate = readText(initial.certificateFile);
        const std::string               oldKey         = readText(initial.privateKeyFile);
        const auto                      configuration  = managerConfiguration(directory, initial.certificateFile);

        // 模拟两次替换之间崩溃：私钥已换成新的，证书还是旧的，备份是完整旧材料
        ASSERT_TRUE(writeTextFile(configuration.privateKeyFile, readText(next.privateKeyFile)));
        ASSERT_TRUE(writeTextFile(configuration.certificateFile, oldCertificate));
        ASSERT_TRUE(writeTextFile(std::filesystem::path{configuration.certificateFile.string() + ".bak"}, oldCertificate));
        ASSERT_TRUE(writeTextFile(std::filesystem::path{configuration.privateKeyFile.string() + ".bak"}, oldKey));
        const auto marker = installMarkerPath(configuration);
        ASSERT_TRUE(writeTextFile(marker, installMarkerText(configuration)));

        const auto recovered = AgentCertificateManager::recoverInterruptedInstall(configuration);
        ASSERT_TRUE(recovered.has_value()) << recovered.error();
        EXPECT_TRUE(*recovered) << "配对失败时必须回退备份";
        EXPECT_EQ(oldCertificate, readText(configuration.certificateFile));
        EXPECT_EQ(oldKey, readText(configuration.privateKeyFile));
        EXPECT_FALSE(std::filesystem::exists(marker)) << "恢复完成后必须删除安装标记";

        const auto identity = AgentIdentity::load(configuration);
        ASSERT_TRUE(identity.has_value()) << identity.error();
        EXPECT_EQ(initial.fingerprint, identity->fingerprint);
    }

    TEST(AgentCertificateManager, ClearsInterruptedInstallMarkerWhenPairLoads)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      initial            = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        const std::string               initialCertificate = readText(initial.certificateFile);
        const auto                      configuration      = managerConfiguration(directory, initial.certificateFile);
        const auto                      marker             = installMarkerPath(configuration);
        ASSERT_TRUE(writeTextFile(marker, installMarkerText(configuration)));

        const auto recovered = AgentCertificateManager::recoverInterruptedInstall(configuration);
        ASSERT_TRUE(recovered.has_value()) << recovered.error();
        EXPECT_FALSE(*recovered) << "新的一对能装载时不应回退";
        EXPECT_FALSE(std::filesystem::exists(marker));
        EXPECT_EQ(initialCertificate, readText(configuration.certificateFile));
    }

    TEST(AgentCertificateManager, RemovesBackupFilesAfterCommit)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      authority = Testing::writeCertificateAuthority(directory, "ca");
        static_cast<void>(Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent"));
        const auto configuration = managerConfiguration(directory, authority.certificateFile);

        AgentCertificateManager manager(configuration);
        const auto              request = manager.generateKeyAndCsr();
        ASSERT_TRUE(request.has_value()) << request.error();
        const auto issued = Testing::signCertificateRequest(authority, request->csrPem, 90L * 24L * 60L * 60L);
        ASSERT_TRUE(manager.install(issued.certificatePem).has_value());

        const auto certificateBackup = std::filesystem::path{configuration.certificateFile.string() + ".bak"};
        const auto privateKeyBackup  = std::filesystem::path{configuration.privateKeyFile.string() + ".bak"};
        ASSERT_TRUE(std::filesystem::exists(certificateBackup));
        ASSERT_TRUE(std::filesystem::exists(privateKeyBackup));

        manager.commitInstalledIdentity();
        EXPECT_FALSE(std::filesystem::exists(certificateBackup));
        EXPECT_FALSE(std::filesystem::exists(privateKeyBackup));
    }
} // namespace EdgePulse::Agent
