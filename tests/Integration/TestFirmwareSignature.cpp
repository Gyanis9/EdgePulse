#include "Core/Firmware/FirmwareRepository.h"
#include "Core/Firmware/FirmwareSignatureVerifier.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <Base/Coding/Base64.h>
#include <Core/Crypto/Digest.h>

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using PkeyPointer = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

    struct KeyPair
    {
        PkeyPointer privateKey{nullptr, &EVP_PKEY_free}; ///< 签名私钥
        PkeyPointer publicKey{nullptr, &EVP_PKEY_free};  ///< 对应的公钥

        ~KeyPair() = default;

        KeyPair() = default;
        KeyPair(KeyPair &&) noexcept = default;
        KeyPair &operator=(KeyPair &&) noexcept = default;
        KeyPair(const KeyPair &) = delete;
        KeyPair &operator=(const KeyPair &) = delete;
    };

    [[nodiscard]] KeyPair generateEd25519()
    {
        KeyPair       pair;
        EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        EXPECT_NE(nullptr, context);
        EXPECT_EQ(1, EVP_PKEY_keygen_init(context));
        EVP_PKEY *key = nullptr;
        EXPECT_EQ(1, EVP_PKEY_keygen(context, &key));
        EVP_PKEY_CTX_free(context);
        pair.privateKey.reset(key);
        pair.publicKey.reset(EVP_PKEY_dup(pair.privateKey.get()));
        return pair;
    }

    [[nodiscard]] PkeyPointer generateEcKey()
    {
        EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        EXPECT_NE(nullptr, context);
        EXPECT_EQ(1, EVP_PKEY_keygen_init(context));
        EXPECT_EQ(1, EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context, NID_X9_62_prime256v1));
        EVP_PKEY *key = nullptr;
        EXPECT_EQ(1, EVP_PKEY_keygen(context, &key));
        EVP_PKEY_CTX_free(context);
        return PkeyPointer{key, &EVP_PKEY_free};
    }

    void writePublicKey(const std::filesystem::path &path, EVP_PKEY *key)
    {
        BIO *bio = BIO_new_file(path.string().c_str(), "w");
        ASSERT_NE(nullptr, bio);
        EXPECT_EQ(1, PEM_write_bio_PUBKEY(bio, key));
        BIO_free(bio);
    }

    [[nodiscard]] std::filesystem::path makeTemporaryDirectory(const std::string_view name)
    {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto directory = std::filesystem::temp_directory_path() / ("edgepulse-" + std::string{name} + "-" + suffix);
        std::filesystem::create_directories(directory);
        return directory;
    }

    [[nodiscard]] std::array<unsigned char, 32> decodeHexDigest(const std::string_view digestHex)
    {
        std::array<unsigned char, 32> digest{};
        for (std::size_t index = 0; index < digest.size(); ++index)
        {
            digest[index] = static_cast<unsigned char>(std::stoul(std::string{digestHex.substr(index * 2, 2)}, nullptr, 16));
        }
        return digest;
    }

    [[nodiscard]] std::string signDigest(EVP_PKEY *privateKey, const std::string_view digestHex)
    {
        const std::array<unsigned char, 32> digest = decodeHexDigest(digestHex);
        EVP_MD_CTX                         *context = EVP_MD_CTX_new();
        EXPECT_NE(nullptr, context);
        EXPECT_EQ(1, EVP_DigestSignInit(context, nullptr, nullptr, nullptr, privateKey));
        std::size_t signatureLength = 0;
        EXPECT_EQ(1, EVP_DigestSign(context, nullptr, &signatureLength, digest.data(), digest.size()));
        std::string signature(signatureLength, '\0');
        EXPECT_EQ(1, EVP_DigestSign(context, reinterpret_cast<unsigned char *>(signature.data()), &signatureLength, digest.data(), digest.size()));
        EVP_MD_CTX_free(context);
        signature.resize(signatureLength);
        return AsynGyanis::Base::base64Encode(signature);
    }
} // namespace

// 钉住：合法签名通过；未知 key、被篡改的摘要、非 Base64 与非 32 字节签名都被拒绝
TEST(FirmwareSignature, VerifiesDigestAndRejectsTampering)
{
    KeyPair                     pair      = generateEd25519();
    const std::filesystem::path directory = makeTemporaryDirectory("firmware-keys");
    writePublicKey(directory / "test-key.pem", pair.publicKey.get());

    const auto verifier = EdgePulse::Firmware::FirmwareSignatureVerifier::loadFromDirectory(directory);
    ASSERT_TRUE(verifier.has_value()) << verifier.error();
    ASSERT_FALSE(verifier->empty());
    const std::vector<std::string> keyIds = verifier->keyIds();
    ASSERT_EQ(1U, keyIds.size());
    EXPECT_EQ("test-key", keyIds.front());

    const std::string digestHex = AsynGyanis::Core::Digest::sha256Hex("firmware-bytes");
    const std::string signature = signDigest(pair.privateKey.get(), digestHex);
    EXPECT_TRUE(verifier->verify("test-key", digestHex, signature).has_value());
    EXPECT_FALSE(verifier->verify("missing-key", digestHex, signature).has_value());
    EXPECT_FALSE(verifier->verify("test-key", AsynGyanis::Core::Digest::sha256Hex("other"), signature).has_value());
    EXPECT_FALSE(verifier->verify("test-key", digestHex, "not-base64").has_value());
    EXPECT_FALSE(verifier->verify("test-key", "abcd", signature).has_value());

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

// 钉住：信任键目录只收 Ed25519 PEM；非 Ed25519 让整批加载失败，目录不存在则是空验签器
TEST(FirmwareSignature, RejectsNonEd25519KeysAndMissingDirectory)
{
    const std::filesystem::path directory = makeTemporaryDirectory("firmware-keys-invalid");
    const PkeyPointer           ecKey     = generateEcKey();
    ASSERT_NE(nullptr, ecKey.get());
    writePublicKey(directory / "ec-key.pem", ecKey.get());

    const auto rejected = EdgePulse::Firmware::FirmwareSignatureVerifier::loadFromDirectory(directory);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_NE(std::string::npos, rejected.error().find("Ed25519"));

    std::error_code error;
    std::filesystem::remove_all(directory, error);

    const std::filesystem::path missing = std::filesystem::temp_directory_path() / "edgepulse-firmware-keys-does-not-exist";
    std::filesystem::remove_all(missing, error);
    const auto empty = EdgePulse::Firmware::FirmwareSignatureVerifier::loadFromDirectory(missing);
    ASSERT_TRUE(empty.has_value()) << empty.error();
    EXPECT_TRUE(empty->empty());
}

// 钉住：三个签名字段能落库读回，未签名计数只统计 signature 为空的记录
TEST(FirmwareSignature, RepositoryPersistsSignatureFieldsAndCountsUnsigned)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner   runner;
    EdgePulse::Firmware::FirmwareRepository   repository(context.database);

    EdgePulse::Firmware::FirmwareRecord unsignedRecord;
    unsignedRecord.name        = "EdgePulse OS";
    unsignedRecord.version     = "1.0.0";
    unsignedRecord.fileName    = "os.bin";
    unsignedRecord.contentType = "application/octet-stream";
    unsignedRecord.sha256      = std::string(64, 'a');
    unsignedRecord.storagePath = "var/firmware/os.bin";

    EdgePulse::Firmware::FirmwareRecord signedRecord = unsignedRecord;
    signedRecord.version            = "1.0.1";
    signedRecord.fileName           = "os-signed.bin";
    signedRecord.signature          = std::string(86, 'A');
    signedRecord.signingKeyId       = "prod-2026";
    signedRecord.signatureAlgorithm = "ed25519-sha256";

    const std::int64_t unsignedId = runner.run(repository.create(runner.loop(), unsignedRecord));
    const std::int64_t signedId   = runner.run(repository.create(runner.loop(), signedRecord));
    ASSERT_GT(unsignedId, 0);
    ASSERT_GT(signedId, 0);

    const auto stored = runner.run(repository.find(runner.loop(), signedId));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(signedRecord.signature, stored->signature);
    EXPECT_EQ("prod-2026", stored->signingKeyId);
    EXPECT_EQ("ed25519-sha256", stored->signatureAlgorithm);
    EXPECT_EQ(1, runner.run(repository.unsignedCount(runner.loop())));

    const auto allTenants = repository.scoped({0, true});
    EXPECT_EQ(1, runner.run(allTenants->unsignedCount(runner.loop())));
}
