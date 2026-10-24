#include "Core/Device/AgentCertificateAuthority.h"

#include "TestSupport/TemporaryDirectory.h"

#include <gtest/gtest.h>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace
{
    using BioHandle  = std::unique_ptr<BIO, decltype(&BIO_free)>;
    using KeyHandle  = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using CertHandle = std::unique_ptr<X509, decltype(&X509_free)>;
    using ReqHandle  = std::unique_ptr<X509_REQ, decltype(&X509_REQ_free)>;
    using ExtHandle  = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>;

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
            throw std::runtime_error(std::string{"生成测试材料失败："} + step);
        }
    }

    /**
     * @brief 把内存 BIO 里的文本取成字符串
     * @param bio 内存 BIO
     * @return std::string 文本内容
     */
    [[nodiscard]] std::string bioText(BIO &bio)
    {
        char      *buffer = nullptr;
        const long length = BIO_get_mem_data(&bio, &buffer);
        if (buffer == nullptr || length <= 0)
        {
            throw std::runtime_error("生成测试材料失败：内存缓冲为空");
        }
        return std::string{buffer, static_cast<std::size_t>(length)};
    }

    /**
     * @brief 把文本写入文件
     * @param path 目标路径
     * @param text 文件内容
     * @throws std::runtime_error 无法创建或写入
     */
    void writeTextFile(const std::filesystem::path &path, const std::string &text)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            throw std::runtime_error("写入测试文件失败：" + path.string());
        }
        stream << text;
        if (!stream)
        {
            throw std::runtime_error("写入测试文件失败：" + path.string());
        }
    }

    /**
     * @brief 读取整个文本文件
     * @param path 文件路径
     * @return std::string 文件内容
     * @throws std::runtime_error 无法打开或读取
     */
    [[nodiscard]] std::string readTextFile(const std::filesystem::path &path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            throw std::runtime_error("读取测试文件失败：" + path.string());
        }
        return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
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
     * @brief 取证书序列号的十六进制文本
     * @param certificate 证书
     * @return std::string 小写十六进制
     */
    [[nodiscard]] std::string serialOf(X509 &certificate)
    {
        BIGNUM *raw = ASN1_INTEGER_to_BN(X509_get0_serialNumber(&certificate), nullptr);
        requireStep(raw != nullptr, "读取序列号");
        const std::unique_ptr<BIGNUM, decltype(&BN_free)> number(raw, &BN_free);
        char                                             *text = BN_bn2hex(number.get());
        requireStep(text != nullptr, "转换序列号");
        std::string result{text};
        OPENSSL_free(text);
        for (char &character: result)
        {
            if (character >= 'A' && character <= 'F')
            {
                character = static_cast<char>(character - 'A' + 'a');
            }
        }
        return result;
    }

    /**
     * @brief 把 ASN.1 时间写成服务端使用的 UTC 文本
     * @param time ASN.1 时间
     * @return std::string 形如 2026-10-25 12:00:00
     */
    [[nodiscard]] std::string utcText(const ASN1_TIME *time)
    {
        tm value{};
        requireStep(ASN1_TIME_to_tm(time, &value) == 1, "解析 ASN.1 时间");
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", value.tm_year + 1900, value.tm_mon + 1, value.tm_mday, value.tm_hour, value.tm_min, value.tm_sec);
        return std::string{buffer};
    }

    /**
     * @brief 把 ASN.1 时间转成 UTC 纪元秒
     * @param time ASN.1 时间
     * @return std::time_t 对应时刻
     */
    [[nodiscard]] std::time_t epochOf(const ASN1_TIME *time)
    {
        tm value{};
        requireStep(ASN1_TIME_to_tm(time, &value) == 1, "解析 ASN.1 时间");
#ifdef _WIN32
        return _mkgmtime(&value);
#else
        return timegm(&value);
#endif
    }

    /**
     * @brief 从 PEM 文件读证书
     * @param path 证书路径
     * @return CertHandle 证书句柄
     */
    [[nodiscard]] CertHandle readCertificate(const std::filesystem::path &path)
    {
        BioHandle bio(BIO_new_file(path.string().c_str(), "rb"), &BIO_free);
        requireStep(bio != nullptr, "打开证书文件");
        CertHandle certificate(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), &X509_free);
        requireStep(certificate != nullptr, "解析证书文件");
        return certificate;
    }

    /**
     * @brief 从 PEM 文本读证书
     * @param pem PEM 文本
     * @return CertHandle 证书句柄
     */
    [[nodiscard]] CertHandle parseCertificate(const std::string &pem)
    {
        BioHandle bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free);
        requireStep(bio != nullptr, "创建证书内存缓冲");
        CertHandle certificate(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), &X509_free);
        requireStep(certificate != nullptr, "解析证书 PEM");
        return certificate;
    }

    /**
     * @brief 取证书主题里的 CN
     * @param certificate 证书
     * @return std::string CN 文本；没有 CN 时为空
     */
    [[nodiscard]] std::string commonNameOf(X509 &certificate)
    {
        char      buffer[256]{};
        const int length = X509_NAME_get_text_by_NID(X509_get_subject_name(&certificate), NID_commonName, buffer, sizeof(buffer));
        if (length <= 0)
        {
            return {};
        }
        return std::string{buffer, static_cast<std::size_t>(length)};
    }

    /**
     * @brief 测试用 CA 的可变参数
     */
    struct TestAuthorityOptions
    {
        bool        isCa{true};                                     ///< basicConstraints 是否为 CA:TRUE
        bool        includeKeyUsage{true};                          ///< 是否写入带 keyCertSign 的 keyUsage
        int         rsaBits{0};                                     ///< 0 表示 EC P-256，正数表示对应位数的 RSA
        long        notBeforeOffsetSeconds{-60};                    ///< 生效时间相对当前时刻的偏移
        long        notAfterOffsetSeconds{3650L * 24L * 60L * 60L}; ///< 到期时间相对当前时刻的偏移
        const char *passphrase{nullptr};                            ///< 非空时私钥按该口令加密写出
    };

    /**
     * @brief 测试用 CA 的文件路径
     */
    struct TestAuthority
    {
        std::filesystem::path certificateFile; ///< CA 证书
        std::filesystem::path keyFile;         ///< CA 私钥
    };

    /**
     * @brief 生成一把椭圆曲线私钥
     * @param group OpenSSL 曲线名
     * @return KeyHandle 私钥句柄
     */
    [[nodiscard]] KeyHandle generateEcKey(const char *group)
    {
        KeyHandle key(EVP_EC_gen(group), &EVP_PKEY_free);
        requireStep(key != nullptr, "生成椭圆曲线私钥");
        return key;
    }

    /**
     * @brief 生成一把 RSA 私钥
     * @param bits 位数
     * @return KeyHandle 私钥句柄
     */
    [[nodiscard]] KeyHandle generateRsaKey(const unsigned int bits)
    {
        KeyHandle key(EVP_RSA_gen(bits), &EVP_PKEY_free);
        requireStep(key != nullptr, "生成 RSA 私钥");
        return key;
    }

    /**
     * @brief 在临时目录里生成一把自签 CA 及其私钥
     * @param directory 临时目录
     * @param prefix 文件名前缀
     * @param options 证书与密钥的可变参数
     * @return TestAuthority 文件路径
     */
    [[nodiscard]] TestAuthority writeTestAuthority(const EdgePulse::TestSupport::TemporaryDirectory &directory, const std::string &prefix, const TestAuthorityOptions &options = {})
    {
        KeyHandle key = options.rsaBits > 0 ? generateRsaKey(static_cast<unsigned int>(options.rsaBits)) : generateEcKey("prime256v1");

        CertHandle certificate(X509_new(), &X509_free);
        requireStep(certificate != nullptr, "创建 CA 证书");
        requireStep(X509_set_version(certificate.get(), 2) == 1, "设置 CA 证书版本");
        requireStep(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1, "设置 CA 序列号");
        requireStep(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), options.notBeforeOffsetSeconds) != nullptr, "设置 CA 起始时间");
        requireStep(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), options.notAfterOffsetSeconds) != nullptr, "设置 CA 到期时间");
        requireStep(X509_set_pubkey(certificate.get(), key.get()) == 1, "写入 CA 公钥");

        X509_NAME        *subject    = X509_get_subject_name(certificate.get());
        const std::string commonName = prefix + " EdgePulse Test CA";
        requireStep(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char *>(commonName.c_str()), -1, -1, 0) == 1, "写入 CA 主题");
        requireStep(X509_set_issuer_name(certificate.get(), subject) == 1, "写入 CA 签发者");

        const char *constraintsText = options.isCa ? "critical,CA:TRUE" : "critical,CA:FALSE";
        ExtHandle   constraints(X509V3_EXT_nconf_nid(nullptr, nullptr, NID_basic_constraints, constraintsText), &X509_EXTENSION_free);
        requireStep(constraints != nullptr, "创建 CA 基本约束");
        requireStep(X509_add_ext(certificate.get(), constraints.get(), -1) == 1, "写入 CA 基本约束");
        if (options.includeKeyUsage)
        {
            ExtHandle usage(X509V3_EXT_nconf_nid(nullptr, nullptr, NID_key_usage, "critical,keyCertSign,cRLSign"), &X509_EXTENSION_free);
            requireStep(usage != nullptr, "创建 CA 密钥用途");
            requireStep(X509_add_ext(certificate.get(), usage.get(), -1) == 1, "写入 CA 密钥用途");
        }
        requireStep(X509_sign(certificate.get(), key.get(), EVP_sha256()) != 0, "签名 CA 证书");

        BioHandle certificateBio(BIO_new(BIO_s_mem()), &BIO_free);
        requireStep(certificateBio != nullptr, "创建 CA 证书缓冲");
        requireStep(PEM_write_bio_X509(certificateBio.get(), certificate.get()) == 1, "输出 CA 证书");
        BioHandle keyBio(BIO_new(BIO_s_mem()), &BIO_free);
        requireStep(keyBio != nullptr, "创建 CA 私钥缓冲");
        unsigned char *passphrase       = options.passphrase == nullptr ? nullptr : reinterpret_cast<unsigned char *>(const_cast<char *>(options.passphrase));
        const int      passphraseLength = options.passphrase == nullptr ? 0 : static_cast<int>(std::strlen(options.passphrase));
        requireStep(
                PEM_write_bio_PrivateKey(keyBio.get(), key.get(), options.passphrase == nullptr ? nullptr : EVP_aes_256_cbc(), passphrase, passphraseLength, nullptr, nullptr) == 1,
                "输出 CA 私钥");

        TestAuthority authority;
        authority.certificateFile = directory.path() / (prefix + ".pem");
        authority.keyFile         = directory.path() / (prefix + "-key.pem");
        writeTextFile(authority.certificateFile, bioText(*certificateBio));
        writeTextFile(authority.keyFile, bioText(*keyBio));
#ifndef _WIN32
        requireStep(::chmod(authority.keyFile.c_str(), S_IRUSR | S_IWUSR) == 0, "设置 CA 私钥权限");
#endif
        return authority;
    }

    /**
     * @brief CSR 要携带的基本约束
     */
    enum class CsrConstraints
    {
        Absent, ///< 不写 basicConstraints 扩展
        CaTrue, ///< 声明 CA:TRUE
        CaFalse ///< 声明 CA:FALSE
    };

    /**
     * @brief 生成 PKCS#10 CSR
     * @param publicKey CSR 声明的公钥
     * @param commonName CSR 主题里的 CN，服务端应忽略它
     * @param constraints CSR 携带的基本约束
     * @param signingKey 实际签名私钥；空表示用 publicKey 自签
     * @return std::string CSR 的 PEM 文本
     */
    [[nodiscard]] std::string makeCertificateRequest(EVP_PKEY &publicKey, const std::string &commonName, const CsrConstraints constraints, EVP_PKEY *signingKey = nullptr)
    {
        ReqHandle request(X509_REQ_new(), &X509_REQ_free);
        requireStep(request != nullptr, "创建 CSR");
        requireStep(X509_REQ_set_version(request.get(), 0) == 1, "设置 CSR 版本");
        requireStep(X509_REQ_set_pubkey(request.get(), &publicKey) == 1, "写入 CSR 公钥");
        X509_NAME *subject = X509_REQ_get_subject_name(request.get());
        requireStep(subject != nullptr, "创建 CSR 主题");
        requireStep(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char *>(commonName.c_str()), -1, -1, 0) == 1, "写入 CSR 主题");
        if (constraints != CsrConstraints::Absent)
        {
            const char *text = constraints == CsrConstraints::CaTrue ? "critical,CA:TRUE" : "critical,CA:FALSE";
            ExtHandle   extension(X509V3_EXT_nconf_nid(nullptr, nullptr, NID_basic_constraints, text), &X509_EXTENSION_free);
            requireStep(extension != nullptr, "创建 CSR 基本约束");
            STACK_OF(X509_EXTENSION) *extensions = sk_X509_EXTENSION_new_null();
            requireStep(extensions != nullptr, "创建 CSR 扩展列表");
            requireStep(sk_X509_EXTENSION_push(extensions, extension.release()) > 0, "追加 CSR 扩展");
            const int added = X509_REQ_add_extensions(request.get(), extensions);
            sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
            requireStep(added == 1, "写入 CSR 扩展");
        }
        requireStep(X509_REQ_sign(request.get(), signingKey == nullptr ? &publicKey : signingKey, EVP_sha256()) > 0, "签名 CSR");

        BioHandle bio(BIO_new(BIO_s_mem()), &BIO_free);
        requireStep(bio != nullptr, "创建 CSR 缓冲");
        requireStep(PEM_write_bio_X509_REQ(bio.get(), request.get()) == 1, "输出 CSR");
        return bioText(*bio);
    }

    /**
     * @brief 从 PEM 文本读 CSR
     * @param pem PEM 文本
     * @return ReqHandle CSR 句柄
     */
    [[nodiscard]] ReqHandle parseRequest(const std::string &pem)
    {
        BioHandle bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free);
        requireStep(bio != nullptr, "创建 CSR 内存缓冲");
        ReqHandle request(PEM_read_bio_X509_REQ(bio.get(), nullptr, nullptr, nullptr), &X509_REQ_free);
        requireStep(request != nullptr, "解析 CSR PEM");
        return request;
    }

    /**
     * @brief 拼出启用状态的 CA 选项
     * @param authority 测试 CA
     * @param enabled 是否启用
     * @return EdgePulse::Device::AgentCaOptions 选项
     */
    [[nodiscard]] EdgePulse::Device::AgentCaOptions optionsFor(const TestAuthority &authority, const bool enabled)
    {
        EdgePulse::Device::AgentCaOptions options;
        options.enabled         = enabled;
        options.certificateFile = authority.certificateFile;
        options.keyFile         = authority.keyFile;
        options.validityDays    = 30;
        options.overlapHours    = 24;
        return options;
    }

    /**
     * @brief 断言错误文本没有带出 CSR 或私钥材料
     * @param text 错误文本
     */
    void expectNoSensitiveMaterial(const std::string &text)
    {
        EXPECT_EQ(std::string::npos, text.find("PRIVATE KEY")) << text;
        EXPECT_EQ(std::string::npos, text.find("BEGIN CERTIFICATE REQUEST")) << text;
        EXPECT_EQ(std::string::npos, text.find("-----BEGIN")) << text;
    }
} // namespace

TEST(AgentCertificateAuthority, IssuesServerOwnedIdentityFromCsr)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");

    auto authority = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(authorityMaterial, true));
    ASSERT_TRUE(authority.has_value()) << authority.error();
    EXPECT_TRUE(authority->enabled());

    const KeyHandle   key = generateEcKey("prime256v1");
    const std::string csr = makeCertificateRequest(*key, "csr-supplied-name", CsrConstraints::Absent);

    const std::time_t issuedAt = std::time(nullptr);
    const auto        issued   = authority->issue(csr, 42, "edge-42");
    ASSERT_TRUE(issued.has_value()) << issued.error();

    EXPECT_EQ(std::string::npos, issued->certificatePem.find("PRIVATE KEY"));
    EXPECT_EQ(0U, issued->certificatePem.find("-----BEGIN CERTIFICATE-----"));

    const CertHandle certificate = parseCertificate(issued->certificatePem);
    const CertHandle ca          = readCertificate(authorityMaterial.certificateFile);
    EXPECT_EQ(1, X509_verify(certificate.get(), X509_get0_pubkey(ca.get())));
    EXPECT_EQ(0, X509_NAME_cmp(X509_get_issuer_name(certificate.get()), X509_get_subject_name(ca.get())));
    EXPECT_EQ("edge-42", commonNameOf(*certificate));

    const std::unique_ptr<GENERAL_NAMES, decltype(&GENERAL_NAMES_free)> names(
            static_cast<GENERAL_NAMES *>(X509_get_ext_d2i(certificate.get(), NID_subject_alt_name, nullptr, nullptr)), &GENERAL_NAMES_free);
    ASSERT_NE(nullptr, names);
    std::vector<std::string> uris;
    for (int index = 0; index < sk_GENERAL_NAME_num(names.get()); ++index)
    {
        const GENERAL_NAME *entry = sk_GENERAL_NAME_value(names.get(), index);
        if (entry->type != GEN_URI)
        {
            continue;
        }
        const ASN1_IA5STRING *uri = entry->d.uniformResourceIdentifier;
        uris.emplace_back(reinterpret_cast<const char *>(ASN1_STRING_get0_data(uri)), static_cast<std::size_t>(ASN1_STRING_length(uri)));
    }
    ASSERT_EQ(1U, uris.size());
    EXPECT_EQ("edgepulse://node/42", uris.front());

    const std::unique_ptr<EXTENDED_KEY_USAGE, decltype(&EXTENDED_KEY_USAGE_free)> extendedUsage(
            static_cast<EXTENDED_KEY_USAGE *>(X509_get_ext_d2i(certificate.get(), NID_ext_key_usage, nullptr, nullptr)), &EXTENDED_KEY_USAGE_free);
    ASSERT_NE(nullptr, extendedUsage);
    ASSERT_EQ(1, sk_ASN1_OBJECT_num(extendedUsage.get()));
    EXPECT_EQ(NID_client_auth, OBJ_obj2nid(sk_ASN1_OBJECT_value(extendedUsage.get(), 0)));

    const std::unique_ptr<BASIC_CONSTRAINTS, decltype(&BASIC_CONSTRAINTS_free)> constraints(
            static_cast<BASIC_CONSTRAINTS *>(X509_get_ext_d2i(certificate.get(), NID_basic_constraints, nullptr, nullptr)), &BASIC_CONSTRAINTS_free);
    ASSERT_NE(nullptr, constraints);
    EXPECT_EQ(0, constraints->ca);

    const std::unique_ptr<ASN1_BIT_STRING, decltype(&ASN1_BIT_STRING_free)> keyUsage(
            static_cast<ASN1_BIT_STRING *>(X509_get_ext_d2i(certificate.get(), NID_key_usage, nullptr, nullptr)), &ASN1_BIT_STRING_free);
    ASSERT_NE(nullptr, keyUsage);
    const auto keyUsageMask = X509_get_key_usage(certificate.get());
    EXPECT_NE(0U, keyUsageMask & KU_DIGITAL_SIGNATURE);
    EXPECT_NE(0U, keyUsageMask & KU_KEY_ENCIPHERMENT);
    EXPECT_EQ(0U, keyUsageMask & KU_KEY_CERT_SIGN);

    EXPECT_EQ(serialOf(*certificate), issued->serial);
    EXPECT_FALSE(issued->serial.empty());
    EXPECT_EQ(std::string::npos, issued->serial.find_first_not_of("0123456789abcdef"));
    EXPECT_EQ(fingerprintOf(*certificate), issued->fingerprint);
    EXPECT_EQ(64U, issued->fingerprint.size());

    EXPECT_EQ(utcText(X509_get0_notBefore(certificate.get())), issued->notBefore);
    EXPECT_EQ(utcText(X509_get0_notAfter(certificate.get())), issued->notAfter);
    EXPECT_EQ(19U, issued->notBefore.size());
    EXPECT_EQ(19U, issued->notAfter.size());
    EXPECT_EQ('-', issued->notBefore[4]);
    EXPECT_EQ('-', issued->notBefore[7]);
    EXPECT_EQ(' ', issued->notBefore[10]);
    EXPECT_EQ(':', issued->notBefore[13]);
    EXPECT_EQ(':', issued->notBefore[16]);
    EXPECT_NE(std::string::npos, issued->subject.find("CN=edge-42"));

    EXPECT_NEAR(static_cast<double>(epochOf(X509_get0_notBefore(certificate.get()))), static_cast<double>(issuedAt - 300), 120.0);
    EXPECT_NEAR(static_cast<double>(epochOf(X509_get0_notAfter(certificate.get()))), static_cast<double>(issuedAt + 30 * 24 * 60 * 60), 120.0);

    const auto second = authority->issue(csr, 42, "edge-42");
    ASSERT_TRUE(second.has_value()) << second.error();
    EXPECT_NE(issued->serial, second->serial);
}

TEST(AgentCertificateAuthority, AcceptsRsa3072Csr)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");
    auto                                             authority         = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(authorityMaterial, true));
    ASSERT_TRUE(authority.has_value()) << authority.error();

    const KeyHandle   key    = generateRsaKey(3072U);
    const std::string csr    = makeCertificateRequest(*key, "rsa-agent", CsrConstraints::Absent);
    const auto        issued = authority->issue(csr, 7, "edge-rsa");
    ASSERT_TRUE(issued.has_value()) << issued.error();
    EXPECT_EQ("edge-rsa", commonNameOf(*parseCertificate(issued->certificatePem)));
}

TEST(AgentCertificateAuthority, RejectsCsrWithBrokenSignature)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");
    auto                                             authority         = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(authorityMaterial, true));
    ASSERT_TRUE(authority.has_value()) << authority.error();

    const KeyHandle   publicKey  = generateEcKey("prime256v1");
    const KeyHandle   signingKey = generateEcKey("prime256v1");
    const std::string csr        = makeCertificateRequest(*publicKey, "tampered", CsrConstraints::Absent, signingKey.get());

    const auto issued = authority->issue(csr, 1, "edge-1");
    ASSERT_FALSE(issued.has_value());
    EXPECT_NE(std::string::npos, issued.error().find("签名"));
    expectNoSensitiveMaterial(issued.error());
}

TEST(AgentCertificateAuthority, RejectsCaTrueCsr)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");
    auto                                             authority         = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(authorityMaterial, true));
    ASSERT_TRUE(authority.has_value()) << authority.error();

    const KeyHandle   key    = generateEcKey("prime256v1");
    const std::string csr    = makeCertificateRequest(*key, "ca-request", CsrConstraints::CaTrue);
    const auto        issued = authority->issue(csr, 1, "edge-1");
    ASSERT_FALSE(issued.has_value());
    EXPECT_NE(std::string::npos, issued.error().find("CA"));
    expectNoSensitiveMaterial(issued.error());
}

TEST(AgentCertificateAuthority, RejectsNonP256EcCsr)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");
    auto                                             authority         = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(authorityMaterial, true));
    ASSERT_TRUE(authority.has_value()) << authority.error();

    const KeyHandle   key    = generateEcKey("secp384r1");
    const std::string csr    = makeCertificateRequest(*key, "p384-agent", CsrConstraints::Absent);
    const auto        issued = authority->issue(csr, 1, "edge-1");
    ASSERT_FALSE(issued.has_value());
    EXPECT_NE(std::string::npos, issued.error().find("P-256"));
    expectNoSensitiveMaterial(issued.error());
}

TEST(AgentCertificateAuthority, RejectsWeakRsaCsr)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");
    auto                                             authority         = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(authorityMaterial, true));
    ASSERT_TRUE(authority.has_value()) << authority.error();

    const KeyHandle   key    = generateRsaKey(2048U);
    const std::string csr    = makeCertificateRequest(*key, "weak-rsa", CsrConstraints::Absent);
    const auto        issued = authority->issue(csr, 1, "edge-1");
    ASSERT_FALSE(issued.has_value());
    EXPECT_NE(std::string::npos, issued.error().find("3072"));
    expectNoSensitiveMaterial(issued.error());
}

TEST(AgentCertificateAuthority, RejectsEmptyAndGarbageCsr)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");
    auto                                             authority         = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(authorityMaterial, true));
    ASSERT_TRUE(authority.has_value()) << authority.error();

    for (const std::string &csr: {std::string{}, std::string{"this is not a certificate request\n"}, std::string{"-----BEGIN CERTIFICATE REQUEST-----\nbroken\n"}})
    {
        const auto issued = authority->issue(csr, 1, "edge-1");
        ASSERT_FALSE(issued.has_value());
        EXPECT_NE(std::string::npos, issued.error().find("CSR"));
        expectNoSensitiveMaterial(issued.error());
    }
}

TEST(AgentCertificateAuthority, RejectsDisabledAuthority)
{
    EdgePulse::Device::AgentCaOptions options;
    options.enabled         = false;
    options.certificateFile = "missing-ca.pem";
    options.keyFile         = "missing-ca-key.pem";
    options.validityDays    = 30;
    options.overlapHours    = 24;

    auto authority = EdgePulse::Device::AgentCertificateAuthority::load(options);
    ASSERT_TRUE(authority.has_value()) << authority.error();
    EXPECT_FALSE(authority->enabled());

    const KeyHandle   key    = generateEcKey("prime256v1");
    const std::string csr    = makeCertificateRequest(*key, "disabled", CsrConstraints::Absent);
    const auto        issued = authority->issue(csr, 1, "edge-1");
    ASSERT_FALSE(issued.has_value());
    EXPECT_NE(std::string::npos, issued.error().find("未启用"));
    expectNoSensitiveMaterial(issued.error());
}

TEST(AgentCertificateAuthority, RejectsMismatchedCertificateAndKey)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              first  = writeTestAuthority(directory, "first");
    const TestAuthority                              second = writeTestAuthority(directory, "second");

    EdgePulse::Device::AgentCaOptions options = optionsFor(first, true);
    options.keyFile                           = second.keyFile;

    auto authority = EdgePulse::Device::AgentCertificateAuthority::load(options);
    ASSERT_FALSE(authority.has_value());
    EXPECT_NE(std::string::npos, authority.error().find("不匹配"));
    expectNoSensitiveMaterial(authority.error());
}

TEST(AgentCertificateAuthority, RejectsMissingCaFiles)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    EdgePulse::Device::AgentCaOptions                options;
    options.enabled         = true;
    options.certificateFile = directory.path() / "absent-ca.pem";
    options.keyFile         = directory.path() / "absent-ca-key.pem";
    options.validityDays    = 30;
    options.overlapHours    = 24;

    auto authority = EdgePulse::Device::AgentCertificateAuthority::load(options);
    ASSERT_FALSE(authority.has_value());
    EXPECT_NE(std::string::npos, authority.error().find("不存在"));
    expectNoSensitiveMaterial(authority.error());
}

TEST(AgentCertificateAuthority, RejectsOverlyPermissivePrivateKeyOnPosix)
{
#ifdef _WIN32
    GTEST_SKIP() << "Windows 不校验 CA 私钥 ACL";
#else
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");
    ASSERT_EQ(0, ::chmod(authorityMaterial.keyFile.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH));

    auto authority = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(authorityMaterial, true));
    ASSERT_FALSE(authority.has_value());
    EXPECT_NE(std::string::npos, authority.error().find("权限"));
    expectNoSensitiveMaterial(authority.error());
#endif
}

TEST(AgentCertificateAuthority, RejectsInvalidNodeIdentity)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");
    auto                                             authority         = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(authorityMaterial, true));
    ASSERT_TRUE(authority.has_value()) << authority.error();

    const KeyHandle   key = generateEcKey("prime256v1");
    const std::string csr = makeCertificateRequest(*key, "edge", CsrConstraints::Absent);

    const auto invalidId = authority->issue(csr, 0, "edge-1");
    ASSERT_FALSE(invalidId.has_value());
    EXPECT_NE(std::string::npos, invalidId.error().find("节点标识"));

    const auto negativeId = authority->issue(csr, -1, "edge-1");
    ASSERT_FALSE(negativeId.has_value());
    EXPECT_NE(std::string::npos, negativeId.error().find("节点标识"));

    const auto emptyName = authority->issue(csr, 1, "");
    ASSERT_FALSE(emptyName.has_value());
    EXPECT_NE(std::string::npos, emptyName.error().find("节点名称"));

    const auto tooLongName = authority->issue(csr, 1, std::string(129, 'a'));
    ASSERT_FALSE(tooLongName.has_value());
    EXPECT_NE(std::string::npos, tooLongName.error().find("128"));
}

TEST(AgentCertificateAuthority, KeepsIssuedCertificatePublicKeyAndConfiguredValidity)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");

    EdgePulse::Device::AgentCaOptions options = optionsFor(authorityMaterial, true);
    options.validityDays                      = 7;
    auto authority                            = EdgePulse::Device::AgentCertificateAuthority::load(options);
    ASSERT_TRUE(authority.has_value()) << authority.error();

    const KeyHandle   key = generateEcKey("prime256v1");
    const std::string csr = makeCertificateRequest(*key, "public-key-check", CsrConstraints::Absent);

    const std::time_t issuedAt = std::time(nullptr);
    const auto        issued   = authority->issue(csr, 11, "edge-11");
    ASSERT_TRUE(issued.has_value()) << issued.error();

    const CertHandle certificate = parseCertificate(issued->certificatePem);
    const ReqHandle  request     = parseRequest(csr);
    EXPECT_EQ(1, EVP_PKEY_eq(X509_get0_pubkey(certificate.get()), X509_REQ_get0_pubkey(request.get())));

    const int serialLength = ASN1_STRING_length(X509_get0_serialNumber(certificate.get()));
    EXPECT_GT(serialLength, 0);
    EXPECT_LE(serialLength, 16);
    EXPECT_GE(issued->serial.size(), 2U);
    EXPECT_LE(issued->serial.size(), 32U);

    EXPECT_NEAR(static_cast<double>(epochOf(X509_get0_notAfter(certificate.get()))), static_cast<double>(issuedAt + 7 * 24 * 60 * 60), 120.0);
}

TEST(AgentCertificateAuthority, AcceptsCaFalseCsr)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");
    auto                                             authority         = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(authorityMaterial, true));
    ASSERT_TRUE(authority.has_value()) << authority.error();

    const KeyHandle   key    = generateEcKey("prime256v1");
    const std::string csr    = makeCertificateRequest(*key, "explicit-leaf", CsrConstraints::CaFalse);
    const auto        issued = authority->issue(csr, 12, "edge-12");
    ASSERT_TRUE(issued.has_value()) << issued.error();
    const CertHandle certificate = parseCertificate(issued->certificatePem);

    const std::unique_ptr<BASIC_CONSTRAINTS, decltype(&BASIC_CONSTRAINTS_free)> constraints(
            static_cast<BASIC_CONSTRAINTS *>(X509_get_ext_d2i(certificate.get(), NID_basic_constraints, nullptr, nullptr)), &BASIC_CONSTRAINTS_free);
    ASSERT_NE(nullptr, constraints);
    EXPECT_EQ(0, constraints->ca);
}

TEST(AgentCertificateAuthority, UsesCompactAuditSubjectForLongNonAsciiNodeName)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");
    auto                                             authority         = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(authorityMaterial, true));
    ASSERT_TRUE(authority.has_value()) << authority.error();

    std::string nodeName;
    for (int index = 0; index < 21; ++index)
    {
        nodeName += "边缘";
    }
    nodeName += "01";
    ASSERT_EQ(128U, nodeName.size());

    const KeyHandle   key    = generateEcKey("prime256v1");
    const std::string csr    = makeCertificateRequest(*key, "ignored-csr-name", CsrConstraints::Absent);
    const auto        issued = authority->issue(csr, 13, nodeName);
    ASSERT_TRUE(issued.has_value()) << issued.error();

    EXPECT_EQ("CN=" + nodeName, issued->subject);
    EXPECT_LE(issued->subject.size(), 255U);
    EXPECT_EQ(std::string::npos, issued->subject.find('\\'));
    EXPECT_EQ(nodeName, commonNameOf(*parseCertificate(issued->certificatePem)));
}

TEST(AgentCertificateAuthority, RejectsNonCaAuthorityCertificate)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    TestAuthorityOptions                             options;
    options.isCa                          = false;
    const TestAuthority authorityMaterial = writeTestAuthority(directory, "leaf", options);

    auto authority = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(authorityMaterial, true));
    ASSERT_FALSE(authority.has_value());
    EXPECT_NE(std::string::npos, authority.error().find("CA:TRUE"));
    expectNoSensitiveMaterial(authority.error());
}

TEST(AgentCertificateAuthority, RejectsAuthorityWithoutKeyCertSign)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    TestAuthorityOptions                             options;
    options.includeKeyUsage               = false;
    const TestAuthority authorityMaterial = writeTestAuthority(directory, "no-usage", options);

    auto authority = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(authorityMaterial, true));
    ASSERT_FALSE(authority.has_value());
    EXPECT_NE(std::string::npos, authority.error().find("keyCertSign"));
    expectNoSensitiveMaterial(authority.error());
}

TEST(AgentCertificateAuthority, RejectsAuthorityOutsideValidityPeriod)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;

    TestAuthorityOptions expired;
    expired.notBeforeOffsetSeconds       = -7200L;
    expired.notAfterOffsetSeconds        = -3600L;
    const TestAuthority expiredMaterial  = writeTestAuthority(directory, "expired", expired);
    auto                expiredAuthority = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(expiredMaterial, true));
    ASSERT_FALSE(expiredAuthority.has_value());
    EXPECT_NE(std::string::npos, expiredAuthority.error().find("过期"));
    expectNoSensitiveMaterial(expiredAuthority.error());

    TestAuthorityOptions future;
    future.notBeforeOffsetSeconds       = 3600L;
    future.notAfterOffsetSeconds        = 7200L;
    const TestAuthority futureMaterial  = writeTestAuthority(directory, "future", future);
    auto                futureAuthority = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(futureMaterial, true));
    ASSERT_FALSE(futureAuthority.has_value());
    EXPECT_NE(std::string::npos, futureAuthority.error().find("尚未生效"));
    expectNoSensitiveMaterial(futureAuthority.error());
}

TEST(AgentCertificateAuthority, RejectsWeakAuthorityKey)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    TestAuthorityOptions                             options;
    options.rsaBits                       = 2048;
    const TestAuthority authorityMaterial = writeTestAuthority(directory, "weak-ca", options);

    auto authority = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(authorityMaterial, true));
    ASSERT_FALSE(authority.has_value());
    EXPECT_NE(std::string::npos, authority.error().find("3072"));
    expectNoSensitiveMaterial(authority.error());
}

TEST(AgentCertificateAuthority, RejectsSameCertificateAndKeyPath)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");

    EdgePulse::Device::AgentCaOptions options = optionsFor(authorityMaterial, true);
    options.keyFile                           = options.certificateFile;

    auto authority = EdgePulse::Device::AgentCertificateAuthority::load(options);
    ASSERT_FALSE(authority.has_value());
    EXPECT_NE(std::string::npos, authority.error().find("同一个文件"));
    expectNoSensitiveMaterial(authority.error());
}

TEST(AgentCertificateAuthority, AcceptsAuthorityTrustedByListenerBundle)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");
    const TestAuthority                              otherMaterial     = writeTestAuthority(directory, "other");

    const std::filesystem::path bundle = directory.path() / "trust-bundle.pem";
    // 签发 CA 放在第二张，确认信任库是按整包匹配而不是只看首张证书
    writeTextFile(bundle, readTextFile(otherMaterial.certificateFile) + readTextFile(authorityMaterial.certificateFile));

    EdgePulse::Device::AgentCaOptions options = optionsFor(authorityMaterial, true);
    options.trustedCertificateFile            = bundle;
    auto authority                            = EdgePulse::Device::AgentCertificateAuthority::load(options);
    ASSERT_TRUE(authority.has_value()) << authority.error();
    EXPECT_TRUE(authority->enabled());
}

TEST(AgentCertificateAuthority, RejectsAuthorityMissingFromListenerTrust)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");
    const TestAuthority                              otherMaterial     = writeTestAuthority(directory, "other");

    EdgePulse::Device::AgentCaOptions options = optionsFor(authorityMaterial, true);
    options.trustedCertificateFile            = otherMaterial.certificateFile;
    auto authority                            = EdgePulse::Device::AgentCertificateAuthority::load(options);
    ASSERT_FALSE(authority.has_value());
    EXPECT_NE(std::string::npos, authority.error().find("信任"));
    expectNoSensitiveMaterial(authority.error());
}

TEST(AgentCertificateAuthority, RejectsMissingListenerTrustFile)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    const TestAuthority                              authorityMaterial = writeTestAuthority(directory, "ca");

    EdgePulse::Device::AgentCaOptions options = optionsFor(authorityMaterial, true);
    options.trustedCertificateFile            = directory.path() / "absent-trust.pem";
    auto authority                            = EdgePulse::Device::AgentCertificateAuthority::load(options);
    ASSERT_FALSE(authority.has_value());
    EXPECT_NE(std::string::npos, authority.error().find("信任库"));
    expectNoSensitiveMaterial(authority.error());
}

TEST(AgentCertificateAuthority, RejectsEncryptedPrivateKey)
{
    const EdgePulse::TestSupport::TemporaryDirectory directory;
    TestAuthorityOptions                             options;
    options.passphrase                    = "test-passphrase";
    const TestAuthority authorityMaterial = writeTestAuthority(directory, "encrypted", options);

    auto authority = EdgePulse::Device::AgentCertificateAuthority::load(optionsFor(authorityMaterial, true));
    ASSERT_FALSE(authority.has_value());
    EXPECT_NE(std::string::npos, authority.error().find("未加密"));
    expectNoSensitiveMaterial(authority.error());
}
