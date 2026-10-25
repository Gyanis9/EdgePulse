#include "Core/Device/CertificateBinding.h"

#include <openssl/asn1.h>
#include <openssl/x509.h>

#include <cctype>
#include <cstdio>
#include <memory>
#include <string>

namespace EdgePulse::Device
{
    namespace
    {
        constexpr std::size_t kFingerprintLength = 64;
    } // namespace

    std::optional<std::string> normalizeCertificateFingerprint(const std::string_view fingerprint)
    {
        if (fingerprint.size() != kFingerprintLength)
        {
            return std::nullopt;
        }
        std::string normalized;
        normalized.reserve(kFingerprintLength);
        for (const char character: fingerprint)
        {
            const auto value = static_cast<unsigned char>(character);
            if (!std::isxdigit(value))
            {
                return std::nullopt;
            }
            normalized.push_back(static_cast<char>(std::tolower(value)));
        }
        return normalized;
    }

    std::optional<std::string> certificateBindingFailure(const AsynGyanis::Core::TlsPeerIdentityPtr &connectionIdentity, const std::string_view registeredFingerprint)
    {
        if (!connectionIdentity)
        {
            return std::nullopt;
        }
        if (!connectionIdentity->isVerified())
        {
            return "连接证书未通过 TLS 校验";
        }
        const std::optional<std::string> registered = normalizeCertificateFingerprint(registeredFingerprint);
        if (!registered.has_value())
        {
            return "节点没有可用的证书指纹登记";
        }
        if (*registered != connectionIdentity->sha256FingerprintHex())
        {
            return "连接证书指纹与节点登记不一致";
        }
        return std::nullopt;
    }

    std::optional<std::string> certificateNotAfter(const AsynGyanis::Core::TlsPeerIdentityPtr &identity)
    {
        if (!identity || identity->certificateDer().empty())
        {
            // 测试注入的空 DER 没有可解析的证书；生产 mTLS 一定有
            return std::string{};
        }
        const unsigned char *cursor      = identity->certificateDer().data();
        X509                *certificate = d2i_X509(nullptr, &cursor, static_cast<long>(identity->certificateDer().size()));
        if (certificate == nullptr)
        {
            return std::nullopt;
        }
        std::unique_ptr<X509, decltype(&X509_free)> guard(certificate, &X509_free);
        tm                                          expires{};
        if (ASN1_TIME_to_tm(X509_get0_notAfter(certificate), &expires) != 1)
        {
            return std::nullopt;
        }
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", expires.tm_year + 1900, expires.tm_mon + 1, expires.tm_mday, expires.tm_hour, expires.tm_min,
                      expires.tm_sec);
        return std::string{buffer};
    }

    bool certificateMatchesNode(const AsynGyanis::Core::TlsPeerIdentityPtr &connectionIdentity, const NodeRecord &node, const std::string_view now)
    {
        if (!connectionIdentity || !connectionIdentity->isVerified())
        {
            return false;
        }
        if (!node.certRevokedAt.empty())
        {
            return false;
        }
        const std::optional<std::string> observed = normalizeCertificateFingerprint(connectionIdentity->sha256FingerprintHex());
        if (!observed.has_value())
        {
            return false;
        }
        if (const std::optional<std::string> active = normalizeCertificateFingerprint(node.certFingerprint); active.has_value() && *active == *observed)
        {
            return true;
        }
        if (node.previousCertFingerprint.empty() || node.previousCertNotAfter.empty() || now.empty() || node.previousCertNotAfter <= now)
        {
            return false;
        }
        const std::optional<std::string> previous = normalizeCertificateFingerprint(node.previousCertFingerprint);
        return previous.has_value() && *previous == *observed;
    }
} // namespace EdgePulse::Device
