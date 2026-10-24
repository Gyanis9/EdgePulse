#include "Core/Auth/MfaCrypto.h"

#include "Base/Coding/Base64.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace EdgePulse::Auth::MfaCrypto
{
    namespace
    {
        using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

        /**
         * @brief 把长度收窄到 OpenSSL 接口接受的 int
         * @param length 字节数
         * @return bool 是否在 int 范围内
         */
        [[nodiscard]] bool fitsOpenSslLength(const std::size_t length) noexcept
        {
            return length <= static_cast<std::size_t>(std::numeric_limits<int>::max());
        }
    } // namespace

    std::optional<MfaCiphertext> encrypt(const std::string_view plaintext, const std::string_view key)
    {
        if (key.size() != kMfaKeyBytes || !fitsOpenSslLength(plaintext.size()))
        {
            return std::nullopt;
        }

        std::array<unsigned char, kMfaNonceBytes> nonce{};
        if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1)
        {
            return std::nullopt;
        }

        CipherContext context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
        if (!context)
        {
            return std::nullopt;
        }
        if (EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
            EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kMfaNonceBytes), nullptr) != 1 ||
            EVP_EncryptInit_ex(context.get(), nullptr, nullptr, reinterpret_cast<const unsigned char *>(key.data()), nonce.data()) != 1)
        {
            return std::nullopt;
        }

        // 先按明文长度加标签预留，收尾后再缩到实际产出，避免二次分配
        std::string ciphertextBytes(plaintext.size() + kMfaTagBytes, '\0');
        int         written = 0;
        if (!plaintext.empty() && EVP_EncryptUpdate(context.get(), reinterpret_cast<unsigned char *>(ciphertextBytes.data()), &written,
                                                    reinterpret_cast<const unsigned char *>(plaintext.data()), static_cast<int>(plaintext.size())) != 1)
        {
            return std::nullopt;
        }
        int finalWritten = 0;
        if (EVP_EncryptFinal_ex(context.get(), reinterpret_cast<unsigned char *>(ciphertextBytes.data()) + written, &finalWritten) != 1)
        {
            return std::nullopt;
        }
        const std::size_t bodyBytes = static_cast<std::size_t>(written) + static_cast<std::size_t>(finalWritten);
        // 标签紧跟在密文尾部，存储与解析都只认这一种形状
        if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kMfaTagBytes), reinterpret_cast<unsigned char *>(ciphertextBytes.data()) + bodyBytes) != 1)
        {
            return std::nullopt;
        }
        ciphertextBytes.resize(bodyBytes + kMfaTagBytes);

        const std::string_view nonceBytes(reinterpret_cast<const char *>(nonce.data()), nonce.size());
        return MfaCiphertext{AsynGyanis::Base::base64Encode(nonceBytes), AsynGyanis::Base::base64Encode(ciphertextBytes)};
    }

    std::optional<std::string> decrypt(const std::string_view ciphertext, const std::string_view nonce, const std::string_view key)
    {
        if (key.size() != kMfaKeyBytes)
        {
            return std::nullopt;
        }
        const std::optional<std::string> decodedNonce = AsynGyanis::Base::base64Decode(nonce);
        const std::optional<std::string> decodedBody  = AsynGyanis::Base::base64Decode(ciphertext);
        if (!decodedNonce.has_value() || decodedNonce->size() != kMfaNonceBytes || !decodedBody.has_value() || decodedBody->size() < kMfaTagBytes)
        {
            return std::nullopt;
        }

        const std::size_t bodyBytes = decodedBody->size() - kMfaTagBytes;
        if (!fitsOpenSslLength(bodyBytes))
        {
            return std::nullopt;
        }
        CipherContext context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
        if (!context)
        {
            return std::nullopt;
        }
        if (EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
            EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kMfaNonceBytes), nullptr) != 1 ||
            EVP_DecryptInit_ex(context.get(), nullptr, nullptr, reinterpret_cast<const unsigned char *>(key.data()),
                               reinterpret_cast<const unsigned char *>(decodedNonce->data())) != 1)
        {
            return std::nullopt;
        }

        std::string plaintext(bodyBytes, '\0');
        int         written = 0;
        if (bodyBytes > 0 && EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char *>(plaintext.data()), &written,
                                               reinterpret_cast<const unsigned char *>(decodedBody->data()), static_cast<int>(bodyBytes)) != 1)
        {
            return std::nullopt;
        }
        // OpenSSL 的 set_tag 只读这段标签，但接口形参不是 const，转换只在这一处发生
        if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kMfaTagBytes),
                                const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(decodedBody->data() + bodyBytes))) != 1)
        {
            return std::nullopt;
        }
        int finalWritten = 0;
        if (EVP_DecryptFinal_ex(context.get(), reinterpret_cast<unsigned char *>(plaintext.data()) + written, &finalWritten) <= 0)
        {
            return std::nullopt;
        }
        plaintext.resize(static_cast<std::size_t>(written) + static_cast<std::size_t>(finalWritten));
        return plaintext;
    }
} // namespace EdgePulse::Auth::MfaCrypto
