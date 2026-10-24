#include "Core/Device/AgentCertificateAuthority.h"

#include "Base/Log/LogMacros.h"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace EdgePulse::Device
{
    namespace
    {
        using BioHandle    = std::unique_ptr<BIO, decltype(&BIO_free)>;
        using KeyHandle    = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
        using CertHandle   = std::unique_ptr<X509, decltype(&X509_free)>;
        using ReqHandle    = std::unique_ptr<X509_REQ, decltype(&X509_REQ_free)>;
        using ExtHandle    = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>;
        using NameHandle   = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>;
        using BignumHandle = std::unique_ptr<BIGNUM, decltype(&BN_free)>;

        constexpr char        kHexDigits[]              = "0123456789abcdef";
        constexpr std::size_t kMaximumNodeNameLength    = 128;
        constexpr std::size_t kMaximumSubjectLength     = 255;
        constexpr long        kNotBeforeBackdateSeconds = 300L;
        constexpr std::size_t kSerialBytes              = 16;

        /**
         * @brief 扩展栈的释放器
         */
        struct ExtensionStackDeleter
        {
            void operator()(STACK_OF(X509_EXTENSION) * extensions) const noexcept
            {
                sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
            }
        };

        using ExtensionStack = std::unique_ptr<STACK_OF(X509_EXTENSION), ExtensionStackDeleter>;

        /**
         * @brief 返回失败原因前清空 OpenSSL 错误队列
         * @param message 中文原因
         * @return std::unexpected<std::string> 可直接作为 load 的失败结果
         */
        [[nodiscard]] std::unexpected<std::string> failAndClear(std::string message)
        {
            ERR_clear_error();
            return std::unexpected(std::move(message));
        }

        /**
         * @brief 拒绝为加密私钥索取口令
         * @return int 恒为 -1；PEM_read_bio_PrivateKey 收到负值即停止，不读终端也不挂起
         */
        int denyPassphrase(char * /*buffer*/, const int /*size*/, const int /*writing*/, void * /*userdata*/) noexcept
        {
            return -1;
        }

        /**
         * @brief 检查文件存在、是普通文件且可打开
         * @param path 文件路径
         * @param label 写进错误文本的名字
         * @return 检查通过时为空；失败返回中文原因
         */
        [[nodiscard]] std::optional<std::string> readableFileError(const std::filesystem::path &path, const std::string_view label)
        {
            std::error_code error;
            if (!std::filesystem::exists(path, error) || error)
            {
                return std::string{label} + "不存在：" + path.string();
            }
            if (!std::filesystem::is_regular_file(path, error) || error)
            {
                return std::string{label} + "不是普通文件：" + path.string();
            }
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
            {
                return std::string{label} + "无法读取：" + path.string();
            }
            return std::nullopt;
        }

        /**
         * @brief 检查 CA 私钥文件权限
         * @param path 私钥路径
         * @return 权限不安全时返回中文原因；Windows 只记录警告并放行
         */
        [[nodiscard]] std::optional<std::string> privateKeyPermissionError(const std::filesystem::path &path)
        {
#ifndef _WIN32
            struct stat metadata{};
            if (::stat(path.c_str(), &metadata) != 0)
            {
                return "无法读取 CA 私钥文件权限：" + path.string();
            }
            const mode_t forbiddenBits = S_IRWXG | S_IRWXO;
            if ((metadata.st_mode & forbiddenBits) != 0)
            {
                char modeText[8]{};
                std::snprintf(modeText, sizeof(modeText), "%03o", static_cast<unsigned int>(metadata.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)));
                return "CA 私钥文件权限过宽（" + std::string{modeText} + "）：" + path.string() + "，要求 0600 或更严，请执行 chmod 600 后重启";
            }
            return std::nullopt;
#else
            LOG_WARN_FMT("Windows 上不校验 CA 私钥文件 ACL，请确认仅 SYSTEM 与 Administrators 可读：{}", path.string());
            return std::nullopt;
#endif
        }

        /**
         * @brief 计算证书 SHA-256 指纹
         * @param certificate 已签发证书
         * @return 成功返回 64 位小写十六进制；失败返回中文原因
         */
        [[nodiscard]] std::expected<std::string, std::string> fingerprintOf(X509 &certificate)
        {
            unsigned char digest[EVP_MAX_MD_SIZE]{};
            unsigned int  length = 0;
            if (X509_digest(&certificate, EVP_sha256(), digest, &length) != 1)
            {
                return std::unexpected("计算证书指纹失败：OpenSSL 没有返回 SHA-256 摘要");
            }
            if (length != 32U)
            {
                return std::unexpected("计算证书指纹失败：摘要长度不是 32 字节");
            }
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
         * @brief 校验 CA 证书自身的约束与有效期
         * @param certificate 待检查的 CA 证书
         * @param path 证书路径，写进错误文本
         * @return 可用于签发时为空；否则返回中文原因
         */
        [[nodiscard]] std::optional<std::string> caConstraintError(X509 &certificate, const std::filesystem::path &path)
        {
            const std::unique_ptr<BASIC_CONSTRAINTS, decltype(&BASIC_CONSTRAINTS_free)> constraints(
                    static_cast<BASIC_CONSTRAINTS *>(X509_get_ext_d2i(&certificate, NID_basic_constraints, nullptr, nullptr)), &BASIC_CONSTRAINTS_free);
            if (!constraints)
            {
                return "CA 证书缺少 basicConstraints 扩展，不能用于签发 Agent 证书：" + path.string();
            }
            if (constraints->ca == 0)
            {
                return "CA 证书的 basicConstraints 不是 CA:TRUE，不能用于签发 Agent 证书：" + path.string();
            }
            const std::uint32_t keyUsage = X509_get_key_usage(&certificate);
            if (keyUsage == UINT32_MAX)
            {
                return "CA 证书缺少 keyUsage 扩展，必须包含 keyCertSign：" + path.string();
            }
            if ((keyUsage & KU_KEY_CERT_SIGN) == 0)
            {
                return "CA 证书的 keyUsage 不含 keyCertSign，不能用于签发 Agent 证书：" + path.string();
            }
            const ASN1_TIME *notBefore = X509_get0_notBefore(&certificate);
            const ASN1_TIME *notAfter  = X509_get0_notAfter(&certificate);
            tm               parsed{};
            if (notBefore == nullptr || notAfter == nullptr || ASN1_TIME_to_tm(notBefore, &parsed) != 1 || ASN1_TIME_to_tm(notAfter, &parsed) != 1)
            {
                return "CA 证书的时间无法解析，不能用于签发 Agent 证书：" + path.string();
            }
            if (X509_cmp_current_time(notBefore) > 0)
            {
                return "CA 证书尚未生效：" + path.string();
            }
            if (X509_cmp_current_time(notAfter) <= 0)
            {
                return "CA 证书已过期：" + path.string();
            }
            return std::nullopt;
        }

        /**
         * @brief 校验 CA 私钥的算法与强度
         * @param privateKey CA 私钥
         * @return 强度可接受时为空；否则返回中文原因
         */
        [[nodiscard]] std::optional<std::string> caKeyStrengthError(EVP_PKEY &privateKey)
        {
            const int keyType = EVP_PKEY_base_id(&privateKey);
            if (keyType == EVP_PKEY_EC)
            {
                std::array<char, 64> groupName{};
                std::size_t          groupLength = 0;
                if (EVP_PKEY_get_group_name(&privateKey, groupName.data(), groupName.size(), &groupLength) != 1)
                {
                    return "CA 私钥缺少曲线信息，无法确认强度";
                }
                const std::string_view group{groupName.data(), groupLength};
                // 三种曲线各有 OpenSSL 常见的别名写法，按曲线判定而不是按密钥位数
                const bool allowed =
                        group == "prime256v1" || group == "P-256" || group == "secp256r1" || group == "secp384r1" || group == "P-384" || group == "secp521r1" || group == "P-521";
                if (!allowed)
                {
                    return "CA 私钥的椭圆曲线不受支持：只接受 P-256、P-384 或 P-521";
                }
                return std::nullopt;
            }
            if (keyType == EVP_PKEY_RSA || keyType == EVP_PKEY_RSA_PSS)
            {
                const int bits = EVP_PKEY_get_bits(&privateKey);
                if (bits < 3072)
                {
                    return "CA 私钥的 RSA 密钥只有 " + std::to_string(bits) + " 位，要求至少 3072 位";
                }
                return std::nullopt;
            }
            return "CA 私钥类型不受支持：只接受 P-256、P-384、P-521 椭圆曲线或至少 3072 位的 RSA";
        }

        /**
         * @brief 确认签发 CA 会被 9443 监听器信任
         * @param certificate 签发 CA 证书
         * @param trustFile 9443 监听器加载的客户端信任库，可包含多张证书
         * @return 签发 CA 的指纹出现在信任库中时为空；否则返回中文原因
         */
        [[nodiscard]] std::optional<std::string> listenerTrustError(X509 &certificate, const std::filesystem::path &trustFile)
        {
            std::error_code error;
            if (!std::filesystem::exists(trustFile, error) || error)
            {
                return "9443 监听器的客户端信任库不存在：" + trustFile.string() + "；启用 Agent CA 时必须让 cert_dir/ca.pem 信任签发 CA";
            }
            if (!std::filesystem::is_regular_file(trustFile, error) || error)
            {
                return "9443 监听器的客户端信任库不是普通文件：" + trustFile.string();
            }
            const auto issuerFingerprint = fingerprintOf(certificate);
            if (!issuerFingerprint)
            {
                return issuerFingerprint.error();
            }
            BioHandle bio(BIO_new_file(trustFile.string().c_str(), "rb"), &BIO_free);
            if (!bio)
            {
                return "读取 9443 监听器客户端信任库失败：" + trustFile.string() + "（无法打开文件）";
            }
            ERR_clear_error();
            std::size_t parsedCertificates = 0;
            while (true)
            {
                CertHandle candidate(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), &X509_free);
                if (!candidate)
                {
                    break;
                }
                ++parsedCertificates;
                const auto candidateFingerprint = fingerprintOf(*candidate);
                if (!candidateFingerprint)
                {
                    return candidateFingerprint.error();
                }
                if (*candidateFingerprint == *issuerFingerprint)
                {
                    ERR_clear_error();
                    return std::nullopt;
                }
            }
            ERR_clear_error();
            if (parsedCertificates == 0)
            {
                return "9443 监听器的客户端信任库没有可解析的 PEM 证书：" + trustFile.string();
            }
            return "Agent CA 证书不在 9443 监听器的客户端信任库中：签发 CA 指纹 " + *issuerFingerprint + "，信任库 " + trustFile.string() +
                   "；请让 edgepulse.agent_ca.certificate_file 与 cert_dir/ca.pem 使用同一 CA，或把该 CA 证书加入信任包"
                   "（ca.pem 可以拼接多张证书）";
        }

        /**
         * @brief 把 BIGNUM 写成小写十六进制
         * @param number 待转换数值
         * @return 成功返回十六进制文本；失败返回中文原因
         */
        [[nodiscard]] std::expected<std::string, std::string> hexOf(BIGNUM &number)
        {
            char *raw = BN_bn2hex(&number);
            if (raw == nullptr)
            {
                return std::unexpected("序列号转十六进制失败：OpenSSL 没有返回文本");
            }
            std::string text{raw};
            OPENSSL_free(raw);
            for (char &character: text)
            {
                if (character >= 'A' && character <= 'F')
                {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            return text;
        }

        /**
         * @brief 把 ASN.1 时间写成服务端使用的 UTC 文本
         * @param time ASN.1 时间
         * @return 成功返回形如 2026-10-25 12:00:00 的文本；失败返回中文原因
         */
        [[nodiscard]] std::expected<std::string, std::string> formatAsn1Time(const ASN1_TIME *time)
        {
            tm value{};
            if (ASN1_TIME_to_tm(time, &value) != 1)
            {
                return std::unexpected("证书时间解析失败：ASN.1 时间不是可识别格式");
            }
            char buffer[32]{};
            std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", value.tm_year + 1900, value.tm_mon + 1, value.tm_mday, value.tm_hour, value.tm_min,
                          value.tm_sec);
            return std::string{buffer};
        }

        /**
         * @brief 拒绝声明 CA:TRUE 的 CSR
         * @param request 已通过自签校验的请求
         * @return 请求声明自己是 CA 或扩展无法解析时返回中文原因；其余为空
         */
        [[nodiscard]] std::optional<std::string> caRequestError(X509_REQ &request)
        {
            ExtensionStack extensions(X509_REQ_get_extensions(&request));
            if (!extensions)
            {
                return std::nullopt;
            }
            for (int index = 0; index < sk_X509_EXTENSION_num(extensions.get()); ++index)
            {
                X509_EXTENSION *extension = sk_X509_EXTENSION_value(extensions.get(), index);
                if (OBJ_obj2nid(X509_EXTENSION_get_object(extension)) != NID_basic_constraints)
                {
                    continue;
                }
                const std::unique_ptr<BASIC_CONSTRAINTS, decltype(&BASIC_CONSTRAINTS_free)> constraints(static_cast<BASIC_CONSTRAINTS *>(X509V3_EXT_d2i(extension)),
                                                                                                        &BASIC_CONSTRAINTS_free);
                if (!constraints)
                {
                    return "CSR 的基本约束扩展无法解析，拒绝签发";
                }
                if (constraints->ca != 0)
                {
                    return "CSR 请求 CA:TRUE，拒绝签发终端证书";
                }
            }
            return std::nullopt;
        }

        /**
         * @brief 校验 CSR 公钥算法与强度
         * @param publicKey CSR 携带的公钥
         * @return 允许签发时为空；失败返回中文原因
         */
        [[nodiscard]] std::optional<std::string> publicKeyError(EVP_PKEY &publicKey)
        {
            const int keyType = EVP_PKEY_base_id(&publicKey);
            if (keyType == EVP_PKEY_EC)
            {
                std::array<char, 64> groupName{};
                std::size_t          groupLength = 0;
                if (EVP_PKEY_get_group_name(&publicKey, groupName.data(), groupName.size(), &groupLength) != 1)
                {
                    return "CSR 公钥缺少曲线信息，只接受 P-256";
                }
                const std::string_view group{groupName.data(), groupLength};
                // OpenSSL 3 把 P-256 报成 prime256v1，部分实现报 P-256 或 secp256r1，三种写法指同一条曲线
                if (group != "prime256v1" && group != "P-256" && group != "secp256r1")
                {
                    return "CSR 公钥曲线不受支持，只接受 P-256";
                }
                return std::nullopt;
            }
            if (keyType == EVP_PKEY_RSA || keyType == EVP_PKEY_RSA_PSS)
            {
                const int bits = EVP_PKEY_get_bits(&publicKey);
                if (bits < 3072)
                {
                    return "CSR 的 RSA 公钥只有 " + std::to_string(bits) + " 位，要求至少 3072 位";
                }
                return std::nullopt;
            }
            return "CSR 公钥类型不受支持，只接受 P-256 椭圆曲线或至少 3072 位的 RSA";
        }

        /**
         * @brief 往证书追加一个文本扩展
         * @param certificate 目标证书
         * @param context 扩展上下文
         * @param nid 扩展 NID
         * @param value 扩展文本
         * @return 成功为空；失败返回中文原因
         */
        [[nodiscard]] std::optional<std::string> addExtension(X509 &certificate, X509V3_CTX &context, const int nid, const std::string &value)
        {
            ExtHandle extension(X509V3_EXT_nconf_nid(nullptr, &context, nid, value.c_str()), &X509_EXTENSION_free);
            if (!extension)
            {
                return "构造证书扩展失败：" + std::string{OBJ_nid2sn(nid)};
            }
            if (X509_add_ext(&certificate, extension.get(), -1) != 1)
            {
                return "写入证书扩展失败：" + std::string{OBJ_nid2sn(nid)};
            }
            return std::nullopt;
        }
    } // namespace

    struct AgentCertificateAuthority::Implementation
    {
        AgentCaOptions options;                             ///< 签发参数
        CertHandle     certificate{nullptr, &X509_free};    ///< CA 证书
        KeyHandle      privateKey{nullptr, &EVP_PKEY_free}; ///< CA 私钥
    };

    AgentCertificateAuthority::AgentCertificateAuthority() = default;

    AgentCertificateAuthority::AgentCertificateAuthority(std::unique_ptr<Implementation> implementation) : m_implementation(std::move(implementation))
    {
    }

    AgentCertificateAuthority::~AgentCertificateAuthority() = default;

    AgentCertificateAuthority::AgentCertificateAuthority(AgentCertificateAuthority &&other) noexcept = default;

    AgentCertificateAuthority &AgentCertificateAuthority::operator=(AgentCertificateAuthority &&other) noexcept = default;

    std::expected<AgentCertificateAuthority, std::string> AgentCertificateAuthority::load(const AgentCaOptions &options)
    {
        ERR_clear_error();
        if (options.validityDays < 1 || options.validityDays > 3650)
        {
            return std::unexpected("Agent CA 的证书有效期必须在 1 到 3650 天之间");
        }
        if (options.overlapHours < 1 || options.overlapHours > 168)
        {
            return std::unexpected("Agent CA 的证书重叠窗口必须在 1 到 168 小时之间");
        }

        auto implementation     = std::make_unique<Implementation>();
        implementation->options = options;
        if (!options.enabled)
        {
            return AgentCertificateAuthority{std::move(implementation)};
        }
        if (options.certificateFile.empty() || options.keyFile.empty())
        {
            return std::unexpected("启用 Agent CA 时必须配置 CA 证书与私钥路径");
        }
        if (const auto error = readableFileError(options.certificateFile, "CA 证书文件"))
        {
            return std::unexpected(*error);
        }
        if (const auto error = readableFileError(options.keyFile, "CA 私钥文件"))
        {
            return std::unexpected(*error);
        }
        // 路径不同但指向同一文件同样会先读证书再读私钥，这里按文件身份而不是字面路径拒绝
        std::error_code samePathError;
        if (std::filesystem::equivalent(options.certificateFile, options.keyFile, samePathError) && !samePathError)
        {
            return failAndClear("CA 证书与私钥不能使用同一个文件：" + options.certificateFile.string() + "；请把私钥放到仅服务账号可读的单独路径");
        }
        if (const auto error = privateKeyPermissionError(options.keyFile))
        {
            return std::unexpected(*error);
        }

        BioHandle certificateBio(BIO_new_file(options.certificateFile.string().c_str(), "rb"), &BIO_free);
        if (!certificateBio)
        {
            return failAndClear("读取 CA 证书失败：" + options.certificateFile.string() + "（无法打开文件）");
        }
        CertHandle certificate(PEM_read_bio_X509(certificateBio.get(), nullptr, nullptr, nullptr), &X509_free);
        if (!certificate)
        {
            return failAndClear("CA 证书解析失败：" + options.certificateFile.string() + "（请确认文件是 PEM 编码的 X.509 证书）");
        }

        BioHandle privateKeyBio(BIO_new_file(options.keyFile.string().c_str(), "rb"), &BIO_free);
        if (!privateKeyBio)
        {
            return failAndClear("读取 CA 私钥失败：" + options.keyFile.string() + "（无法打开文件）");
        }
        // 口令回调一律拒绝：加密私钥在启动期直接失败，不能读终端或挂起在等待输入
        KeyHandle privateKey(PEM_read_bio_PrivateKey(privateKeyBio.get(), nullptr, denyPassphrase, nullptr), &EVP_PKEY_free);
        if (!privateKey)
        {
            return failAndClear("CA 私钥解析失败：" + options.keyFile.string() + "（请确认文件是未加密的 PEM 私钥；不接受交互式口令输入）");
        }
        // 配对失败时 OpenSSL 会把原因堆在错误队列里，这里只给固定中文说明，避免把密钥细节带进日志或响应
        if (X509_check_private_key(certificate.get(), privateKey.get()) != 1)
        {
            return failAndClear("CA 证书与私钥不匹配：" + options.certificateFile.string() + " 与 " + options.keyFile.string());
        }
        if (const auto error = caConstraintError(*certificate, options.certificateFile))
        {
            return failAndClear(*error);
        }
        if (const auto error = caKeyStrengthError(*privateKey))
        {
            return failAndClear(*error);
        }
        if (!options.trustedCertificateFile.empty())
        {
            if (const auto error = listenerTrustError(*certificate, options.trustedCertificateFile))
            {
                return failAndClear(*error);
            }
        }

        ERR_clear_error();
        implementation->certificate = std::move(certificate);
        implementation->privateKey  = std::move(privateKey);
        return AgentCertificateAuthority{std::move(implementation)};
    }

    std::expected<IssuedAgentCertificate, std::string> AgentCertificateAuthority::issue(const std::string &csrPem, const std::int64_t nodeId, const std::string &nodeName)
    {
        if (!m_implementation || !m_implementation->options.enabled)
        {
            return std::unexpected("Agent 内部 CA 未启用，无法签发证书");
        }
        if (nodeId <= 0)
        {
            return std::unexpected("节点标识必须是正整数");
        }
        if (nodeName.empty())
        {
            return std::unexpected("节点名称不能为空");
        }
        if (nodeName.size() > kMaximumNodeNameLength || nodeName.find('\0') != std::string::npos)
        {
            return std::unexpected("节点名称不能超过 128 字节，且不能包含空字符");
        }
        // 落库的是审计用紧凑主题而不是 RFC 2253：后者会把每个非 ASCII 字节转义成四个字符，128 字节节点名可超过 255
        const std::string auditSubject = "CN=" + nodeName;
        if (auditSubject.size() > kMaximumSubjectLength)
        {
            return std::unexpected("节点名称拼成审计主题后超过 255 字节，无法写入证书历史：" + std::to_string(auditSubject.size()) + " 字节");
        }
        if (csrPem.empty())
        {
            return std::unexpected("CSR 为空：请提交 PEM 编码的 PKCS#10 请求");
        }

        BioHandle csrBio(BIO_new_mem_buf(csrPem.data(), static_cast<int>(csrPem.size())), &BIO_free);
        if (!csrBio)
        {
            return std::unexpected("CSR 解析失败：无法创建内存缓冲");
        }
        ReqHandle request(PEM_read_bio_X509_REQ(csrBio.get(), nullptr, nullptr, nullptr), &X509_REQ_free);
        if (!request)
        {
            return std::unexpected("CSR 解析失败：请提交 PEM 编码的 PKCS#10 请求");
        }
        EVP_PKEY *publicKey = X509_REQ_get0_pubkey(request.get());
        if (publicKey == nullptr)
        {
            return std::unexpected("CSR 解析失败：请求没有携带公钥");
        }
        if (X509_REQ_verify(request.get(), publicKey) != 1)
        {
            return std::unexpected("CSR 自签名校验失败：请求可能被篡改，或公钥与签名不匹配");
        }
        if (const auto error = publicKeyError(*publicKey))
        {
            return std::unexpected(*error);
        }
        if (const auto error = caRequestError(*request))
        {
            return std::unexpected(*error);
        }

        std::array<unsigned char, kSerialBytes> serialBytes{};
        bool                                    nonZero = false;
        while (!nonZero)
        {
            if (RAND_bytes(serialBytes.data(), static_cast<int>(serialBytes.size())) != 1)
            {
                return std::unexpected("生成证书序列号失败：随机数源不可用");
            }
            nonZero = std::any_of(serialBytes.begin(), serialBytes.end(), [](const unsigned char value) { return value != 0; });
        }
        BignumHandle serialNumber(BN_bin2bn(serialBytes.data(), static_cast<int>(serialBytes.size()), nullptr), &BN_free);
        if (!serialNumber)
        {
            return std::unexpected("生成证书序列号失败：无法构造大整数");
        }

        CertHandle certificate(X509_new(), &X509_free);
        if (!certificate)
        {
            return std::unexpected("创建证书失败：OpenSSL 没有返回证书对象");
        }
        if (X509_set_version(certificate.get(), 2) != 1)
        {
            return std::unexpected("创建证书失败：无法设置 X.509 v3 版本");
        }
        if (BN_to_ASN1_INTEGER(serialNumber.get(), X509_get_serialNumber(certificate.get())) == nullptr)
        {
            return std::unexpected("创建证书失败：无法写入随机序列号");
        }
        if (X509_set_pubkey(certificate.get(), publicKey) != 1)
        {
            return std::unexpected("创建证书失败：无法写入 CSR 公钥");
        }
        if (X509_set_issuer_name(certificate.get(), X509_get_subject_name(m_implementation->certificate.get())) != 1)
        {
            return std::unexpected("创建证书失败：无法写入签发者主题");
        }

        NameHandle subject(X509_NAME_new(), &X509_NAME_free);
        if (!subject)
        {
            return std::unexpected("创建证书失败：无法创建主题");
        }
        if (X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char *>(nodeName.c_str()), -1, -1, 0) != 1)
        {
            return std::unexpected("创建证书失败：无法写入节点名称");
        }
        if (X509_set_subject_name(certificate.get(), subject.get()) != 1)
        {
            return std::unexpected("创建证书失败：无法设置证书主题");
        }

        if (X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -kNotBeforeBackdateSeconds) == nullptr)
        {
            return std::unexpected("创建证书失败：无法设置生效时间");
        }
        if (X509_gmtime_adj(X509_getm_notAfter(certificate.get()), static_cast<long>(m_implementation->options.validityDays) * 24L * 60L * 60L) == nullptr)
        {
            return std::unexpected("创建证书失败：无法设置到期时间");
        }

        X509V3_CTX context;
        X509V3_set_ctx_nodb(&context);
        X509V3_set_ctx(&context, m_implementation->certificate.get(), certificate.get(), nullptr, nullptr, 0);
        if (const auto error = addExtension(*certificate, context, NID_basic_constraints, "critical,CA:FALSE"))
        {
            return std::unexpected(*error);
        }
        if (const auto error = addExtension(*certificate, context, NID_key_usage, "critical,digitalSignature,keyEncipherment"))
        {
            return std::unexpected(*error);
        }
        if (const auto error = addExtension(*certificate, context, NID_ext_key_usage, "clientAuth"))
        {
            return std::unexpected(*error);
        }
        if (const auto error = addExtension(*certificate, context, NID_subject_alt_name, "URI:edgepulse://node/" + std::to_string(nodeId)))
        {
            return std::unexpected(*error);
        }

        if (X509_sign(certificate.get(), m_implementation->privateKey.get(), EVP_sha256()) == 0)
        {
            return std::unexpected("证书签名失败：CA 私钥无法完成 SHA-256 签名");
        }

        BioHandle pemBio(BIO_new(BIO_s_mem()), &BIO_free);
        if (!pemBio)
        {
            return std::unexpected("输出证书失败：无法创建内存缓冲");
        }
        if (PEM_write_bio_X509(pemBio.get(), certificate.get()) != 1)
        {
            return std::unexpected("输出证书失败：无法写出 PEM 文本");
        }
        char      *pemBuffer = nullptr;
        const long pemLength = BIO_get_mem_data(pemBio.get(), &pemBuffer);
        if (pemBuffer == nullptr || pemLength <= 0)
        {
            return std::unexpected("输出证书失败：PEM 文本为空");
        }

        const auto fingerprint = fingerprintOf(*certificate);
        if (!fingerprint)
        {
            return std::unexpected(fingerprint.error());
        }
        const auto serial = hexOf(*serialNumber);
        if (!serial)
        {
            return std::unexpected(serial.error());
        }
        const auto notBefore = formatAsn1Time(X509_get0_notBefore(certificate.get()));
        if (!notBefore)
        {
            return std::unexpected(notBefore.error());
        }
        const auto notAfter = formatAsn1Time(X509_get0_notAfter(certificate.get()));
        if (!notAfter)
        {
            return std::unexpected(notAfter.error());
        }

        IssuedAgentCertificate issued;
        issued.certificatePem = std::string{pemBuffer, static_cast<std::size_t>(pemLength)};
        issued.fingerprint    = *fingerprint;
        issued.serial         = *serial;
        issued.subject        = auditSubject;
        issued.notBefore      = *notBefore;
        issued.notAfter       = *notAfter;
        return issued;
    }

    bool AgentCertificateAuthority::enabled() const noexcept
    {
        return m_implementation && m_implementation->options.enabled;
    }
} // namespace EdgePulse::Device
