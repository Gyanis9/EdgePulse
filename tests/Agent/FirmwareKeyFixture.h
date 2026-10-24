/**
 * @file FirmwareKeyFixture.h
 * @brief 固件用例使用的运行时 Ed25519 密钥：生成、写公钥、给摘要签名
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace EdgePulse::Testing
{
    /**
     * @brief 一次性的 Ed25519 测试密钥
     * @details 私钥只存在于本对象里，公钥按 key id 写成信任键目录下的 <keyId>.pem；
     *          用例结束时随对象销毁，不落版本库，也不依赖 openssl 命令行。
     */
    class FirmwareKeyFixture
    {
    public:
        ~FirmwareKeyFixture();

        FirmwareKeyFixture(FirmwareKeyFixture &&other) noexcept;
        FirmwareKeyFixture &operator=(FirmwareKeyFixture &&other) noexcept;
        FirmwareKeyFixture(const FirmwareKeyFixture &)            = delete;
        FirmwareKeyFixture &operator=(const FirmwareKeyFixture &) = delete;

        /**
         * @brief 生成密钥对并把公钥写入信任键目录
         * @param trustedKeysDirectory 信任键目录，不存在时创建
         * @param keyId 公钥文件名（不含 .pem），也是验签时使用的 key id
         * @return 成功返回密钥夹具；失败返回中文原因
         */
        [[nodiscard]] static std::expected<FirmwareKeyFixture, std::string> generate(const std::filesystem::path &trustedKeysDirectory, std::string keyId);

        /**
         * @brief 给 SHA-256 摘要签名
         * @param digestHex 64 个十六进制字符的摘要文本，大小写均可
         * @return std::string 标准 Base64 的 Ed25519 签名；输入不合法或签名失败时为空
         */
        [[nodiscard]] std::string signDigest(std::string_view digestHex) const;

        /**
         * @brief 取 key id
         * @return const std::string & 公钥文件名（不含 .pem）
         */
        [[nodiscard]] const std::string &keyId() const noexcept;

    private:
        struct Implementation;

        FirmwareKeyFixture();

        std::unique_ptr<Implementation> m_implementation; ///< 私钥与 key id
    };
} // namespace EdgePulse::Testing
