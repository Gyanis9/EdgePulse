#include "Core/Auth/Jwt.h"

#include "Base/Config/ConfigValue.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::Auth
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        struct PkeyDeleter
        {
            void operator()(EVP_PKEY *key) const noexcept
            {
                EVP_PKEY_free(key);
            }
        };
        struct PkeyContextDeleter
        {
            void operator()(EVP_PKEY_CTX *context) const noexcept
            {
                EVP_PKEY_CTX_free(context);
            }
        };
        struct ParamBuildDeleter
        {
            void operator()(OSSL_PARAM_BLD *builder) const noexcept
            {
                OSSL_PARAM_BLD_free(builder);
            }
        };
        struct ParamDeleter
        {
            void operator()(OSSL_PARAM *parameters) const noexcept
            {
                OSSL_PARAM_free(parameters);
            }
        };
        struct MdContextDeleter
        {
            void operator()(EVP_MD_CTX *context) const noexcept
            {
                EVP_MD_CTX_free(context);
            }
        };
        struct BignumDeleter
        {
            void operator()(BIGNUM *number) const noexcept
            {
                BN_free(number);
            }
        };

        using PkeyPointer        = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
        using PkeyContextPointer = std::unique_ptr<EVP_PKEY_CTX, PkeyContextDeleter>;
        using ParamBuildPointer  = std::unique_ptr<OSSL_PARAM_BLD, ParamBuildDeleter>;
        using ParamPointer       = std::unique_ptr<OSSL_PARAM, ParamDeleter>;
        using MdContextPointer   = std::unique_ptr<EVP_MD_CTX, MdContextDeleter>;
        using BignumPointer      = std::unique_ptr<BIGNUM, BignumDeleter>;

        [[nodiscard]] std::string base64UrlEncode(const unsigned char *data, const std::size_t size)
        {
            std::string encoded(4 * ((size + 2) / 3), '\0');
            const int   written = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()), data, static_cast<int>(size));
            encoded.resize(static_cast<std::size_t>(written));
            for (char &character: encoded)
            {
                if (character == '+')
                {
                    character = '-';
                } else if (character == '/')
                {
                    character = '_';
                }
            }
            while (!encoded.empty() && encoded.back() == '=')
            {
                encoded.pop_back();
            }
            return encoded;
        }

        [[nodiscard]] std::vector<std::string_view> splitJwt(const std::string_view token)
        {
            std::vector<std::string_view> parts;
            std::size_t                   begin = 0;
            while (begin <= token.size())
            {
                const std::size_t dot = token.find('.', begin);
                if (dot == std::string_view::npos)
                {
                    parts.push_back(token.substr(begin));
                    break;
                }
                parts.push_back(token.substr(begin, dot - begin));
                begin = dot + 1;
            }
            return parts;
        }

        [[nodiscard]] std::string textOf(const ConfigValue &value, const std::string_view key)
        {
            const auto iterator = value.find(std::string{key});
            return iterator != value.end() && iterator->is_string() ? iterator->get<std::string>() : std::string{};
        }

        [[nodiscard]] PkeyPointer rsaKeyFromJwk(const JwtRsaKey &jwk)
        {
            const auto modulus  = decodeBase64Url(jwk.modulus);
            const auto exponent = decodeBase64Url(jwk.exponent);
            if (!modulus.has_value() || !exponent.has_value())
            {
                return {};
            }
            const BignumPointer modulusNumber{BN_bin2bn(reinterpret_cast<const unsigned char *>(modulus->data()), static_cast<int>(modulus->size()), nullptr)};
            const BignumPointer exponentNumber{BN_bin2bn(reinterpret_cast<const unsigned char *>(exponent->data()), static_cast<int>(exponent->size()), nullptr)};
            if (!modulusNumber || !exponentNumber)
            {
                return {};
            }
            const ParamBuildPointer builder{OSSL_PARAM_BLD_new()};
            if (!builder || OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_N, modulusNumber.get()) != 1 ||
                OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_E, exponentNumber.get()) != 1)
            {
                return {};
            }
            const ParamPointer       parameters{OSSL_PARAM_BLD_to_param(builder.get())};
            const PkeyContextPointer context{EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)};
            if (!parameters || !context || EVP_PKEY_fromdata_init(context.get()) != 1)
            {
                return {};
            }
            EVP_PKEY *key = nullptr;
            if (EVP_PKEY_fromdata(context.get(), &key, EVP_PKEY_PUBLIC_KEY, parameters.get()) != 1 || key == nullptr)
            {
                return {};
            }
            return PkeyPointer{key};
        }
    } // namespace

    std::optional<std::string> decodeBase64Url(const std::string_view text)
    {
        std::string padded;
        padded.reserve(text.size() + 3);
        for (const char character: text)
        {
            if (character == '-')
            {
                padded.push_back('+');
            } else if (character == '_')
            {
                padded.push_back('/');
            } else if ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '+' ||
                       character == '/')
            {
                padded.push_back(character);
            } else
            {
                return std::nullopt;
            }
        }
        const std::size_t remainder = padded.size() % 4;
        if (remainder == 1)
        {
            return std::nullopt;
        }
        if (remainder != 0)
        {
            padded.append(4 - remainder, '=');
        }
        std::string decoded(padded.size() / 4 * 3, '\0');
        const int   written =
                EVP_DecodeBlock(reinterpret_cast<unsigned char *>(decoded.data()), reinterpret_cast<const unsigned char *>(padded.data()), static_cast<int>(padded.size()));
        if (written < 0)
        {
            return std::nullopt;
        }
        std::size_t padding = 0;
        if (!padded.empty() && padded.back() == '=')
        {
            ++padding;
        }
        if (padded.size() >= 2 && padded[padded.size() - 2] == '=')
        {
            ++padding;
        }
        decoded.resize(static_cast<std::size_t>(written) - padding);
        return decoded;
    }

    std::string pkceS256Challenge(const std::string_view verifier)
    {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int                               length = 0;
        if (EVP_Digest(verifier.data(), verifier.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1)
        {
            return {};
        }
        return base64UrlEncode(digest.data(), length);
    }

    JwtVerification verifyRs256Jwt(const std::string_view token, const std::vector<JwtRsaKey> &keys, const std::string_view expectedIssuer, const std::string_view expectedAudience,
                                   const std::string_view expectedNonce, const std::int64_t nowSeconds)
    {
        JwtVerification result;
        const auto      parts = splitJwt(token);
        if (parts.size() != 3)
        {
            result.error = "ID Token 不是三段式 JWT";
            return result;
        }
        const auto headerBytes  = decodeBase64Url(parts[0]);
        const auto payloadBytes = decodeBase64Url(parts[1]);
        const auto signature    = decodeBase64Url(parts[2]);
        if (!headerBytes.has_value() || !payloadBytes.has_value() || !signature.has_value())
        {
            result.error = "ID Token 段不是合法 base64url";
            return result;
        }
        const auto header  = AsynGyanis::Base::parseConfigValue(*headerBytes);
        const auto payload = AsynGyanis::Base::parseConfigValue(*payloadBytes);
        if (!header.has_value() || !header->is_object() || !payload.has_value() || !payload->is_object())
        {
            result.error = "ID Token 头部或载荷不是 JSON 对象";
            return result;
        }
        if (textOf(*header, "alg") != "RS256")
        {
            result.error = "ID Token 只接受 RS256 签名";
            return result;
        }
        const std::string keyId = textOf(*header, "kid");
        const JwtRsaKey  *key   = nullptr;
        for (const JwtRsaKey &candidate: keys)
        {
            if (!keyId.empty() && candidate.keyId == keyId)
            {
                key = &candidate;
                break;
            }
        }
        if (key == nullptr)
        {
            result.error = "JWKS 中找不到 ID Token 的 kid";
            return result;
        }
        const PkeyPointer publicKey = rsaKeyFromJwk(*key);
        if (!publicKey)
        {
            result.error = "无法从 JWKS 构造 RSA 公钥";
            return result;
        }
        const std::string_view signingInput = token.substr(0, token.rfind('.'));
        const MdContextPointer digest{EVP_MD_CTX_new()};
        if (!digest || EVP_DigestVerifyInit(digest.get(), nullptr, EVP_sha256(), nullptr, publicKey.get()) != 1 ||
            EVP_DigestVerifyUpdate(digest.get(), signingInput.data(), signingInput.size()) != 1 ||
            EVP_DigestVerifyFinal(digest.get(), reinterpret_cast<const unsigned char *>(signature->data()), signature->size()) != 1)
        {
            result.error = "ID Token 签名校验失败";
            return result;
        }

        result.claims.issuer            = textOf(*payload, "iss");
        result.claims.subject           = textOf(*payload, "sub");
        result.claims.nonce             = textOf(*payload, "nonce");
        result.claims.email             = textOf(*payload, "email");
        result.claims.preferredUsername = textOf(*payload, "preferred_username");
        result.claims.displayName       = textOf(*payload, "name");
        const auto expiration           = payload->find("exp");
        if (expiration != payload->end() && expiration->is_number_integer())
        {
            result.claims.expiresAt = expiration->get<std::int64_t>();
        }
        const auto audience = payload->find("aud");
        if (audience != payload->end())
        {
            if (audience->is_string())
            {
                result.claims.audience.push_back(audience->get<std::string>());
            } else if (audience->is_array())
            {
                for (const ConfigValue &entry: *audience)
                {
                    if (entry.is_string())
                    {
                        result.claims.audience.push_back(entry.get<std::string>());
                    }
                }
            }
        }
        if (result.claims.issuer != expectedIssuer)
        {
            result.error = "ID Token 的 iss 与配置不一致";
            return result;
        }
        bool audienceMatched = false;
        for (const std::string &entry: result.claims.audience)
        {
            if (entry == expectedAudience)
            {
                audienceMatched = true;
                break;
            }
        }
        if (!audienceMatched)
        {
            result.error = "ID Token 的 aud 不包含本客户端";
            return result;
        }
        if (result.claims.expiresAt <= nowSeconds)
        {
            result.error = "ID Token 已过期";
            return result;
        }
        if (!expectedNonce.empty() && result.claims.nonce != expectedNonce)
        {
            result.error = "ID Token 的 nonce 不匹配";
            return result;
        }
        if (result.claims.subject.empty())
        {
            result.error = "ID Token 缺少 sub";
            return result;
        }
        result.ok = true;
        return result;
    }
} // namespace EdgePulse::Auth
