#include "Base/Coding/Base64.h"
#include "Core/Auth/MfaCrypto.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace
{
    constexpr const char *kPlaintext = "JBSWY3DPEHPK3PXP";

    [[nodiscard]] std::string testKey()
    {
        std::string key;
        key.reserve(32);
        for (int index = 0; index < 32; ++index)
        {
            key.push_back(static_cast<char>(index + 1));
        }
        return key;
    }
} // namespace

// 钉住：32 字节密钥加密后可原样解出，nonce 为 12 字节、密文尾部带 16 字节 tag
TEST(MfaCrypto, EncryptsAndDecryptsWithAes256Gcm)
{
    const std::string key       = testKey();
    const auto        encrypted = EdgePulse::Auth::MfaCrypto::encrypt(kPlaintext, key);
    ASSERT_TRUE(encrypted.has_value());

    const auto nonceBytes = AsynGyanis::Base::base64Decode(encrypted->nonce);
    ASSERT_TRUE(nonceBytes.has_value());
    EXPECT_EQ(12U, nonceBytes->size());

    const auto ciphertextBytes = AsynGyanis::Base::base64Decode(encrypted->ciphertext);
    ASSERT_TRUE(ciphertextBytes.has_value());
    EXPECT_EQ(std::string{kPlaintext}.size() + 16U, ciphertextBytes->size());

    const auto decrypted = EdgePulse::Auth::MfaCrypto::decrypt(encrypted->ciphertext, encrypted->nonce, key);
    ASSERT_TRUE(decrypted.has_value());
    EXPECT_EQ(kPlaintext, *decrypted);
}

// 钉住：同一明文两次加密的 nonce 与密文都不同
TEST(MfaCrypto, UsesFreshNoncePerEncryption)
{
    const std::string key    = testKey();
    const auto        first  = EdgePulse::Auth::MfaCrypto::encrypt(kPlaintext, key);
    const auto        second = EdgePulse::Auth::MfaCrypto::encrypt(kPlaintext, key);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(first->nonce, second->nonce);
    EXPECT_NE(first->ciphertext, second->ciphertext);
}

// 钉住：换错密钥解不开，且只返回空值不抛异常
TEST(MfaCrypto, RejectsWrongKey)
{
    const std::string key      = testKey();
    std::string       wrongKey = key;
    wrongKey[0]                = static_cast<char>(wrongKey[0] ^ 0x5A);
    const auto encrypted       = EdgePulse::Auth::MfaCrypto::encrypt(kPlaintext, key);
    ASSERT_TRUE(encrypted.has_value());

    EXPECT_FALSE(EdgePulse::Auth::MfaCrypto::decrypt(encrypted->ciphertext, encrypted->nonce, wrongKey).has_value());
    EXPECT_FALSE(EdgePulse::Auth::MfaCrypto::decrypt(encrypted->ciphertext, encrypted->nonce, key.substr(0, 31)).has_value());
}

// 钉住：密文或 nonce 任一字节被改动都过不了 GCM 校验
TEST(MfaCrypto, RejectsTamperedCiphertextOrNonce)
{
    const std::string key       = testKey();
    const auto        encrypted = EdgePulse::Auth::MfaCrypto::encrypt(kPlaintext, key);
    ASSERT_TRUE(encrypted.has_value());

    std::string ciphertextBytes = *AsynGyanis::Base::base64Decode(encrypted->ciphertext);
    ciphertextBytes[0]          = static_cast<char>(ciphertextBytes[0] ^ 0x01);
    EXPECT_FALSE(EdgePulse::Auth::MfaCrypto::decrypt(AsynGyanis::Base::base64Encode(ciphertextBytes), encrypted->nonce, key).has_value());

    std::string nonceBytes = *AsynGyanis::Base::base64Decode(encrypted->nonce);
    nonceBytes[0]          = static_cast<char>(nonceBytes[0] ^ 0x01);
    EXPECT_FALSE(EdgePulse::Auth::MfaCrypto::decrypt(encrypted->ciphertext, AsynGyanis::Base::base64Encode(nonceBytes), key).has_value());
}

// 钉住：密钥长度不对、base64 损坏或 nonce 长度不对都只返回空值
TEST(MfaCrypto, RejectsMalformedInputs)
{
    const std::string key = testKey();
    EXPECT_FALSE(EdgePulse::Auth::MfaCrypto::encrypt(kPlaintext, key.substr(0, 16)).has_value());
    EXPECT_FALSE(EdgePulse::Auth::MfaCrypto::decrypt("not-base64", "not-base64", key).has_value());
    EXPECT_FALSE(EdgePulse::Auth::MfaCrypto::decrypt(AsynGyanis::Base::base64Encode("short"), AsynGyanis::Base::base64Encode("short"), key).has_value());
}
