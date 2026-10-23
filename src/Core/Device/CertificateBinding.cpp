#include "Core/Device/CertificateBinding.h"

#include <cctype>
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
} // namespace EdgePulse::Device
