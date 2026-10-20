#include "Base/Security/Hmac.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace EdgePulse::Common
{
    namespace
    {
        constexpr std::size_t kSha256ByteCount = 32;

        struct MacDeleter
        {
            void operator()(EVP_MAC *mac) const noexcept
            {
                EVP_MAC_free(mac);
            }
        };

        struct MacContextDeleter
        {
            void operator()(EVP_MAC_CTX *context) const noexcept
            {
                EVP_MAC_CTX_free(context);
            }
        };

        [[nodiscard]] char hexDigit(const unsigned int value) noexcept
        {
            return static_cast<char>(value < 10U ? '0' + value : 'a' + (value - 10U));
        }

        [[nodiscard]] char lowerAscii(const char value) noexcept
        {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        }
    } // namespace

    std::string hmacSha256Hex(const std::string_view key, const std::string_view data)
    {
        using MacPointer        = std::unique_ptr<EVP_MAC, MacDeleter>;
        using MacContextPointer = std::unique_ptr<EVP_MAC_CTX, MacContextDeleter>;

        const MacPointer mac{EVP_MAC_fetch(nullptr, "HMAC", nullptr)};
        if (!mac)
        {
            return {};
        }
        const MacContextPointer context{EVP_MAC_CTX_new(mac.get())};
        if (!context)
        {
            return {};
        }

        char       digestName[] = "SHA256";
        OSSL_PARAM parameters[] = {OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digestName, 0), OSSL_PARAM_construct_end()};
        if (EVP_MAC_init(context.get(), reinterpret_cast<const unsigned char *>(key.data()), key.size(), parameters) != 1)
        {
            return {};
        }
        if (EVP_MAC_update(context.get(), reinterpret_cast<const unsigned char *>(data.data()), data.size()) != 1)
        {
            return {};
        }

        std::array<unsigned char, kSha256ByteCount> digest{};
        std::size_t                                 digestLength = 0;
        if (EVP_MAC_final(context.get(), digest.data(), &digestLength, digest.size()) != 1 || digestLength != digest.size())
        {
            return {};
        }

        std::string text;
        text.reserve(digestLength * 2);
        for (const unsigned char byte: digest)
        {
            text.push_back(hexDigit(static_cast<unsigned int>(byte >> 4U)));
            text.push_back(hexDigit(static_cast<unsigned int>(byte & 0x0FU)));
        }
        return text;
    }

    bool constantTimeHexEquals(const std::string_view left, const std::string_view right)
    {
        if (left.size() != right.size())
        {
            return false;
        }
        std::string normalizedLeft{left};
        std::string normalizedRight{right};
        for (char &character: normalizedLeft)
        {
            character = lowerAscii(character);
        }
        for (char &character: normalizedRight)
        {
            character = lowerAscii(character);
        }
        return CRYPTO_memcmp(normalizedLeft.data(), normalizedRight.data(), normalizedLeft.size()) == 0;
    }
} // namespace EdgePulse::Common
