#include "CertificateFixture.h"

#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace EdgePulse::Agent::Testing
{
    namespace
    {
        using BioHandle       = std::unique_ptr<BIO, decltype(&BIO_free)>;
        using KeyHandle       = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
        using CertHandle      = std::unique_ptr<X509, decltype(&X509_free)>;
        using ExtensionHandle = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>;

        constexpr char kHexDigits[] = "0123456789abcdef";

        /**
         * @brief 生成失败时抛出带步骤名的异常
         * @param succeeded OpenSSL 调用的成功判据
         * @param step 失败步骤
         * @throws std::runtime_error succeeded 为 false 时抛出
         */
        void requireStep(const bool succeeded, const char *step)
        {
            if (!succeeded)
            {
                throw std::runtime_error(std::string{"生成测试证书失败："} + step);
            }
        }

        /**
         * @brief 把内存 BIO 里的 PEM 文本取成字符串
         * @param bio 内存 BIO
         * @return std::string PEM 文本
         */
        [[nodiscard]] std::string pemText(BIO &bio)
        {
            char      *buffer = nullptr;
            const long length = BIO_get_mem_data(&bio, &buffer);
            if (buffer == nullptr || length <= 0)
            {
                throw std::runtime_error("生成测试证书失败：PEM 文本为空");
            }
            return std::string{buffer, static_cast<std::size_t>(length)};
        }

        /**
         * @brief 计算证书 SHA-256 指纹
         * @param certificate 证书
         * @return std::string 64 位小写十六进制
         */
        [[nodiscard]] std::string fingerprintOf(X509 &certificate)
        {
            unsigned char digest[EVP_MAX_MD_SIZE]{};
            unsigned int  length = 0;
            requireStep(X509_digest(&certificate, EVP_sha256(), digest, &length) == 1, "计算指纹");
            requireStep(length == 32U, "指纹长度不是 32 字节");
            std::string text;
            text.reserve(64U);
            for (unsigned int index = 0; index < length; ++index)
            {
                text.push_back(kHexDigits[digest[index] >> 4U]);
                text.push_back(kHexDigits[digest[index] & 0x0FU]);
            }
            return text;
        }

        /**
         * @brief 把证书到期时间写成与实现一致的 UTC 文本
         * @param certificate 证书
         * @return std::string 形如 2026-10-24T12:00:00Z
         */
        [[nodiscard]] std::string expiryOf(X509 &certificate)
        {
            tm expires{};
            requireStep(ASN1_TIME_to_tm(X509_get0_notAfter(&certificate), &expires) == 1, "解析到期时间");
            char buffer[32]{};
            std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", expires.tm_year + 1900, expires.tm_mon + 1, expires.tm_mday, expires.tm_hour, expires.tm_min,
                          expires.tm_sec);
            return std::string{buffer};
        }
    } // namespace

    CertificateMaterial writeSelfSignedCertificate(const TestSupport::TemporaryDirectory &directory, const long validitySeconds, const std::string &prefix)
    {
        KeyHandle key(EVP_RSA_gen(2048U), &EVP_PKEY_free);
        requireStep(key != nullptr, "生成 RSA 私钥");

        CertHandle certificate(X509_new(), &X509_free);
        requireStep(certificate != nullptr, "创建证书对象");
        X509_set_version(certificate.get(), 2);
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1);
        // 已过期的证书也让 notBefore 早于 notAfter，避免生成顺序颠倒的证书
        const long notBeforeOffset = validitySeconds < 0 ? validitySeconds - 60 : -60;
        X509_gmtime_adj(X509_getm_notBefore(certificate.get()), notBeforeOffset);
        X509_gmtime_adj(X509_getm_notAfter(certificate.get()), validitySeconds);
        requireStep(X509_set_pubkey(certificate.get(), key.get()) == 1, "写入公钥");

        X509_NAME        *subject    = X509_get_subject_name(certificate.get());
        const std::string commonName = prefix + "-edgepulse-agent-test";
        requireStep(X509_NAME_add_entry_by_txt(subject, SN_commonName, MBSTRING_ASC, reinterpret_cast<const unsigned char *>(commonName.c_str()), -1, -1, 0) == 1, "写入证书主题");
        requireStep(X509_set_issuer_name(certificate.get(), subject) == 1, "写入签发者");

        ExtensionHandle basicConstraints(X509V3_EXT_nconf_nid(nullptr, nullptr, NID_basic_constraints, "critical,CA:TRUE"), &X509_EXTENSION_free);
        requireStep(basicConstraints != nullptr, "创建基本约束扩展");
        requireStep(X509_add_ext(certificate.get(), basicConstraints.get(), -1) == 1, "写入基本约束扩展");
        requireStep(X509_sign(certificate.get(), key.get(), EVP_sha256()) != 0, "签名证书");

        BioHandle certificateBio(BIO_new(BIO_s_mem()), &BIO_free);
        requireStep(certificateBio != nullptr, "创建证书内存缓冲");
        requireStep(PEM_write_bio_X509(certificateBio.get(), certificate.get()) == 1, "输出证书 PEM");
        const std::string certificateText = pemText(*certificateBio);

        BioHandle keyBio(BIO_new(BIO_s_mem()), &BIO_free);
        requireStep(keyBio != nullptr, "创建私钥内存缓冲");
        requireStep(PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1, "输出私钥 PEM");
        const std::string keyText = pemText(*keyBio);

        CertificateMaterial material;
        material.certificateFile = directory.write(prefix + ".pem", certificateText);
        material.privateKeyFile  = directory.write(prefix + "-key.pem", keyText);
        material.fingerprint     = fingerprintOf(*certificate);
        material.expiresAt       = expiryOf(*certificate);

#ifndef _WIN32
        requireStep(::chmod(material.privateKeyFile.c_str(), S_IRUSR | S_IWUSR) == 0, "设置私钥权限");
#endif
        return material;
    }
} // namespace EdgePulse::Agent::Testing
