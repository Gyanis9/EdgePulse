#include "FirmwareKeyFixture.h"

#include <Base/Coding/Base64.h>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <array>
#include <cstddef>
#include <system_error>
#include <utility>

namespace EdgePulse::Testing
{
    namespace
    {
        /**
         * @brief 取一个十六进制字符的数值
         * @param character 待解析字符
         * @return int 0 到 15；不是十六进制字符时返回 -1
         */
        [[nodiscard]] int hexValue(const char character)
        {
            if (character >= '0' && character <= '9')
            {
                return character - '0';
            }
            if (character >= 'a' && character <= 'f')
            {
                return character - 'a' + 10;
            }
            if (character >= 'A' && character <= 'F')
            {
                return character - 'A' + 10;
            }
            return -1;
        }

        /**
         * @brief 把 64 个十六进制字符解析成 32 字节摘要
         * @param digestHex 摘要文本
         * @param digest 成功时写入摘要字节
         * @return true 表示文本合法
         */
        [[nodiscard]] bool decodeDigest(const std::string_view digestHex, std::array<unsigned char, 32> &digest)
        {
            if (digestHex.size() != 64)
            {
                return false;
            }
            for (std::size_t index = 0; index < digest.size(); ++index)
            {
                const int high = hexValue(digestHex[index * 2]);
                const int low  = hexValue(digestHex[index * 2 + 1]);
                if (high < 0 || low < 0)
                {
                    return false;
                }
                digest[index] = static_cast<unsigned char>(high << 4 | low);
            }
            return true;
        }
    } // namespace

    struct FirmwareKeyFixture::Implementation
    {
        EVP_PKEY   *privateKey{nullptr}; ///< 签名私钥
        std::string keyId;               ///< 公钥文件名（不含 .pem）

        ~Implementation()
        {
            if (privateKey != nullptr)
            {
                EVP_PKEY_free(privateKey);
            }
        }
    };

    FirmwareKeyFixture::FirmwareKeyFixture() = default;

    FirmwareKeyFixture::~FirmwareKeyFixture() = default;

    FirmwareKeyFixture::FirmwareKeyFixture(FirmwareKeyFixture &&other) noexcept = default;

    FirmwareKeyFixture &FirmwareKeyFixture::operator=(FirmwareKeyFixture &&other) noexcept = default;

    std::expected<FirmwareKeyFixture, std::string> FirmwareKeyFixture::generate(const std::filesystem::path &trustedKeysDirectory, std::string keyId)
    {
        if (trustedKeysDirectory.empty())
        {
            return std::unexpected("信任键目录为空");
        }
        if (keyId.empty())
        {
            return std::unexpected("key id 为空");
        }

        std::error_code filesystemError;
        std::filesystem::create_directories(trustedKeysDirectory, filesystemError);
        if (filesystemError)
        {
            return std::unexpected("无法创建信任键目录：" + filesystemError.message());
        }

        EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        if (context == nullptr)
        {
            return std::unexpected("无法创建 Ed25519 密钥上下文");
        }
        EVP_PKEY  *key       = nullptr;
        const bool generated = EVP_PKEY_keygen_init(context) == 1 && EVP_PKEY_keygen(context, &key) == 1;
        EVP_PKEY_CTX_free(context);
        if (!generated || key == nullptr)
        {
            if (key != nullptr)
            {
                EVP_PKEY_free(key);
            }
            return std::unexpected("无法生成 Ed25519 密钥对");
        }

        const std::filesystem::path publicKeyPath = trustedKeysDirectory / (keyId + ".pem");
        BIO                        *bio           = BIO_new_file(publicKeyPath.string().c_str(), "w");
        if (bio == nullptr)
        {
            EVP_PKEY_free(key);
            return std::unexpected("无法写入公钥文件 " + publicKeyPath.string());
        }
        const bool written = PEM_write_bio_PUBKEY(bio, key) == 1;
        BIO_free(bio);
        if (!written)
        {
            EVP_PKEY_free(key);
            return std::unexpected("无法编码 Ed25519 公钥到 " + publicKeyPath.string());
        }

        FirmwareKeyFixture fixture;
        fixture.m_implementation             = std::make_unique<Implementation>();
        fixture.m_implementation->privateKey = key;
        fixture.m_implementation->keyId      = std::move(keyId);
        return fixture;
    }

    std::string FirmwareKeyFixture::signDigest(const std::string_view digestHex) const
    {
        if (m_implementation == nullptr || m_implementation->privateKey == nullptr)
        {
            return {};
        }
        std::array<unsigned char, 32> digest{};
        if (!decodeDigest(digestHex, digest))
        {
            return {};
        }

        EVP_MD_CTX *context = EVP_MD_CTX_new();
        if (context == nullptr)
        {
            return {};
        }
        std::string signature;
        if (EVP_DigestSignInit(context, nullptr, nullptr, nullptr, m_implementation->privateKey) == 1)
        {
            std::size_t length = 0;
            if (EVP_DigestSign(context, nullptr, &length, digest.data(), digest.size()) == 1)
            {
                signature.assign(length, '\0');
                if (EVP_DigestSign(context, reinterpret_cast<unsigned char *>(signature.data()), &length, digest.data(), digest.size()) == 1)
                {
                    signature.resize(length);
                } else
                {
                    signature.clear();
                }
            }
        }
        EVP_MD_CTX_free(context);
        if (signature.empty())
        {
            return {};
        }
        return AsynGyanis::Base::base64Encode(signature);
    }

    const std::string &FirmwareKeyFixture::keyId() const noexcept
    {
        static const std::string empty;
        return m_implementation == nullptr ? empty : m_implementation->keyId;
    }
} // namespace EdgePulse::Testing
