#include "Core/Firmware/FirmwareSignatureVerifier.h"

#include <Base/Coding/Base64.h>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace EdgePulse::Firmware
{
    namespace
    {
        /**
         * @brief 把单个十六进制字符转成数值
         * @param character 字符
         * @return int 0..15；非十六进制返回 -1
         */
        [[nodiscard]] int hexDigit(const char character)
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

        [[nodiscard]] bool isHexDigest(const std::string_view text)
        {
            if (text.size() != 64)
            {
                return false;
            }
            for (const char character: text)
            {
                // 摘要文本来自上传写盘后的 toHex，全小写；这里仍按大小写兼容，便于外部工具直接粘贴
                if (hexDigit(character) < 0)
                {
                    return false;
                }
            }
            return true;
        }

        struct KeyDeleter
        {
            void operator()(EVP_PKEY *key) const noexcept
            {
                EVP_PKEY_free(key);
            }
        };

        using KeyPointer = std::unique_ptr<EVP_PKEY, KeyDeleter>;
    } // namespace

    struct FirmwareSignatureVerifier::Implementation
    {
        std::map<std::string, KeyPointer> keys; ///< key id 到公钥
    };

    FirmwareSignatureVerifier::FirmwareSignatureVerifier() = default;

    FirmwareSignatureVerifier::FirmwareSignatureVerifier(std::unique_ptr<Implementation> implementation) : m_implementation(std::move(implementation))
    {
    }

    FirmwareSignatureVerifier::~FirmwareSignatureVerifier() = default;

    FirmwareSignatureVerifier::FirmwareSignatureVerifier(FirmwareSignatureVerifier &&other) noexcept = default;

    FirmwareSignatureVerifier &FirmwareSignatureVerifier::operator=(FirmwareSignatureVerifier &&other) noexcept = default;

    std::expected<FirmwareSignatureVerifier, std::string> FirmwareSignatureVerifier::loadFromDirectory(const std::filesystem::path &directory)
    {
        if (!std::filesystem::exists(directory))
        {
            // 目录不存在等同「没有配置信任键」；是否允许无签名固件由部署策略决定
            return FirmwareSignatureVerifier{};
        }
        if (!std::filesystem::is_directory(directory))
        {
            return std::unexpected("固件信任键路径不是目录：" + directory.string());
        }

        auto implementation = std::make_unique<Implementation>();
        std::error_code iteratorError;
        for (const auto &entry: std::filesystem::directory_iterator(directory, iteratorError))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".pem")
            {
                continue;
            }
            const std::string keyId = entry.path().stem().string();
            BIO              *bio   = BIO_new_file(entry.path().string().c_str(), "r");
            if (bio == nullptr)
            {
                return std::unexpected("无法打开固件公钥：" + entry.path().string());
            }
            EVP_PKEY *key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
            BIO_free(bio);
            if (key == nullptr)
            {
                return std::unexpected("固件公钥不是合法 PEM：" + entry.path().string());
            }
            KeyPointer guard(key);
            if (EVP_PKEY_get_id(key) != EVP_PKEY_ED25519)
            {
                return std::unexpected("固件公钥必须是 Ed25519：" + entry.path().string());
            }
            if (!implementation->keys.emplace(keyId, std::move(guard)).second)
            {
                return std::unexpected("固件公钥 key id 重复：" + keyId);
            }
        }
        if (iteratorError)
        {
            return std::unexpected("读取固件信任键目录失败：" + directory.string() + "：" + iteratorError.message());
        }
        return FirmwareSignatureVerifier(std::move(implementation));
    }

    std::expected<void, std::string> FirmwareSignatureVerifier::verify(const std::string_view keyId, const std::string_view digestHex,
                                                                      const std::string_view signatureBase64) const
    {
        if (m_implementation == nullptr)
        {
            return std::unexpected("固件验签器未加载任何公钥");
        }
        const auto key = m_implementation->keys.find(std::string{keyId});
        if (key == m_implementation->keys.end())
        {
            return std::unexpected("未知的固件签名 key id：" + std::string{keyId});
        }
        if (!isHexDigest(digestHex))
        {
            return std::unexpected("固件摘要必须是 64 位十六进制");
        }
        const std::optional<std::string> signature = AsynGyanis::Base::base64Decode(signatureBase64);
        // Ed25519 签名固定 32 字节；长度不符时不必进 OpenSSL，先给出可读原因
        if (!signature.has_value() || signature->size() != 64)
        {
            return std::unexpected("固件签名不是合法的 Ed25519 Base64");
        }

        std::array<unsigned char, 32> digest{};
        for (std::size_t index = 0; index < digest.size(); ++index)
        {
            digest[index] = static_cast<unsigned char>(hexDigit(digestHex[index * 2]) * 16 + hexDigit(digestHex[index * 2 + 1]));
        }

        EVP_MD_CTX *context = EVP_MD_CTX_new();
        if (context == nullptr)
        {
            return std::unexpected("创建固件验签上下文失败");
        }
        if (EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key->second.get()) != 1 ||
            EVP_DigestVerify(context, reinterpret_cast<const unsigned char *>(signature->data()), signature->size(), digest.data(), digest.size()) != 1)
        {
            EVP_MD_CTX_free(context);
            return std::unexpected("固件签名与摘要不匹配");
        }
        EVP_MD_CTX_free(context);
        return {};
    }

    std::vector<std::string> FirmwareSignatureVerifier::keyIds() const
    {
        std::vector<std::string> identifiers;
        if (m_implementation == nullptr)
        {
            return identifiers;
        }
        identifiers.reserve(m_implementation->keys.size());
        for (const auto &entry: m_implementation->keys)
        {
            identifiers.push_back(entry.first);
        }
        return identifiers;
    }

    bool FirmwareSignatureVerifier::empty() const noexcept
    {
        return m_implementation == nullptr || m_implementation->keys.empty();
    }
} // namespace EdgePulse::Firmware
