#include "AgentIdentity.h"

#include "AgentConfiguration.h"

#include "Base/Log/LogMacros.h"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace EdgePulse::Agent
{
    namespace
    {
        using BioHandle  = std::unique_ptr<BIO, decltype(&BIO_free)>;
        using CertHandle = std::unique_ptr<X509, decltype(&X509_free)>;

        constexpr char kHexDigits[] = "0123456789abcdef";

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
         * @brief 检查私钥文件权限
         * @param path 私钥路径
         * @return 权限不安全时返回中文原因；Windows 只记录警告并放行
         */
        [[nodiscard]] std::optional<std::string> privateKeyPermissionError(const std::filesystem::path &path)
        {
#ifndef _WIN32
            struct stat metadata{};
            if (::stat(path.c_str(), &metadata) != 0)
            {
                return "无法读取私钥文件权限：" + path.string();
            }
            const mode_t forbiddenBits = S_IRWXG | S_IRWXO;
            if ((metadata.st_mode & forbiddenBits) != 0)
            {
                char modeText[8]{};
                std::snprintf(modeText, sizeof(modeText), "%03o", static_cast<unsigned int>(metadata.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)));
                return "私钥文件权限过宽（" + std::string{modeText} + "）：" + path.string() + "，要求 0600 或更严，请执行 chmod 600 后重启";
            }
            return std::nullopt;
#else
            LOG_WARN_FMT("Windows 上不校验私钥文件 ACL，请确认仅 SYSTEM 与 Administrators 可读：{}", path.string());
            return std::nullopt;
#endif
        }

        /**
         * @brief 计算证书 SHA-256 指纹
         * @param certificate 已解析的证书
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
         * @brief 把证书到期时间写成 UTC 文本
         * @param certificate 已解析的证书
         * @return 成功返回形如 2026-10-24T12:00:00Z 的文本；失败返回中文原因
         */
        [[nodiscard]] std::expected<std::string, std::string> expiryOf(X509 &certificate)
        {
            tm expires{};
            if (ASN1_TIME_to_tm(X509_get0_notAfter(&certificate), &expires) != 1)
            {
                return std::unexpected("证书到期时间解析失败：notAfter 不是可识别的时间格式");
            }
            char buffer[32]{};
            std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", expires.tm_year + 1900, expires.tm_mon + 1, expires.tm_mday, expires.tm_hour, expires.tm_min,
                          expires.tm_sec);
            return std::string{buffer};
        }

        /**
         * @brief 把 UTC 日历时间转成 time_t
         * @param value UTC 日历时间
         * @return std::time_t 对应时刻；转换失败返回 -1
         */
        [[nodiscard]] std::time_t toTimeT(tm value)
        {
#ifdef _WIN32
            return _mkgmtime(&value);
#else
            return timegm(&value);
#endif
        }
    } // namespace

    std::expected<AgentIdentity, std::string> AgentIdentity::load(const AgentConfiguration &configuration)
    {
        if (const auto error = readableFileError(configuration.caFile, "CA 文件"))
        {
            return std::unexpected(*error);
        }
        if (const auto error = readableFileError(configuration.certificateFile, "客户端证书"))
        {
            return std::unexpected(*error);
        }
        if (const auto error = readableFileError(configuration.privateKeyFile, "私钥文件"))
        {
            return std::unexpected(*error);
        }
        if (const auto error = privateKeyPermissionError(configuration.privateKeyFile))
        {
            return std::unexpected(*error);
        }

        BioHandle certificateBio(BIO_new_file(configuration.certificateFile.string().c_str(), "rb"), &BIO_free);
        if (!certificateBio)
        {
            return std::unexpected("读取客户端证书失败：" + configuration.certificateFile.string() + "（无法打开文件）");
        }
        CertHandle certificate(PEM_read_bio_X509(certificateBio.get(), nullptr, nullptr, nullptr), &X509_free);
        if (!certificate)
        {
            return std::unexpected("客户端证书解析失败：" + configuration.certificateFile.string() + "（请确认文件是 PEM 编码的 X.509 证书）");
        }

        const auto fingerprint = fingerprintOf(*certificate);
        if (!fingerprint)
        {
            return std::unexpected(fingerprint.error());
        }
        const auto expiresAt = expiryOf(*certificate);
        if (!expiresAt)
        {
            return std::unexpected(expiresAt.error());
        }

        tm expires{};
        if (ASN1_TIME_to_tm(X509_get0_notAfter(certificate.get()), &expires) == 1)
        {
            const std::time_t seconds = toTimeT(expires);
            if (seconds != static_cast<std::time_t>(-1))
            {
                const auto expiryPoint = std::chrono::system_clock::from_time_t(seconds);
                const auto now         = std::chrono::system_clock::now();
                if (expiryPoint <= now)
                {
                    LOG_WARN_FMT("证书已过期（到期时间 {}），服务端 TLS 会拒绝注册；请更换证书后重启", *expiresAt);
                } else if (expiryPoint - now <= std::chrono::days{7})
                {
                    LOG_WARN_FMT("证书将在 {} 到期，剩余不足 7 天，请尽快更换", *expiresAt);
                }
            }
        }

        AgentIdentity identity;
        identity.fingerprint = *fingerprint;
        identity.expiresAt   = *expiresAt;
        return identity;
    }

    std::expected<AgentIdentity, std::string> AgentIdentity::reload(const AgentConfiguration &configuration)
    {
        return load(configuration);
    }
} // namespace EdgePulse::Agent
