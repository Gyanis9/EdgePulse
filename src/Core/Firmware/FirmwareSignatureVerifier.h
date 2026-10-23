/**
 * @file FirmwareSignatureVerifier.h
 * @brief 固件 Ed25519 公钥验签器
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
#include <vector>

namespace EdgePulse::Firmware
{
    /**
     * @brief 按 key id 查公钥并校验固件摘要签名
     * @details 信任键目录里的每个 *.pem 文件都是一把 Ed25519 公钥，文件名去掉扩展名即 key id。
     *          签名的对象是固件文件 SHA-256 的 32 字节原始摘要，而不是整份文件；传输时用标准
     *          Base64 表示。类持有 EVP_PKEY，只能移动，不能拷贝。
     */
    class FirmwareSignatureVerifier
    {
    public:
        ~FirmwareSignatureVerifier();

        FirmwareSignatureVerifier(FirmwareSignatureVerifier &&other) noexcept;
        FirmwareSignatureVerifier &operator=(FirmwareSignatureVerifier &&other) noexcept;
        FirmwareSignatureVerifier(const FirmwareSignatureVerifier &)            = delete;
        FirmwareSignatureVerifier &operator=(const FirmwareSignatureVerifier &) = delete;

        /**
         * @brief 加载目录内全部 PEM 公钥
         * @param directory 信任键目录；目录不存在时返回空验签器
         * @return std::expected<FirmwareSignatureVerifier, std::string> 验签器；失败返回中文原因
         * @note 任何一把公钥无法解析或不是 Ed25519 都拒绝整批加载，避免只信任一半的目录被悄悄启用
         */
        [[nodiscard]] static std::expected<FirmwareSignatureVerifier, std::string> loadFromDirectory(const std::filesystem::path &directory);

        /**
         * @brief 校验摘要签名
         * @param keyId 公钥文件名（不含 .pem）
         * @param digestHex 固件 SHA-256 的小写或大写十六进制文本，必须 64 个字符
         * @param signatureBase64 32 字节 Ed25519 签名的标准 Base64
         * @return std::expected<void, std::string> 成功为空；失败返回中文原因
         */
        [[nodiscard]] std::expected<void, std::string> verify(std::string_view keyId, std::string_view digestHex, std::string_view signatureBase64) const;

        /**
         * @brief 已加载的 key id 列表
         * @return std::vector<std::string> 按字典序排列的 key id
         */
        [[nodiscard]] std::vector<std::string> keyIds() const;

        /**
         * @brief 是否没有加载到任何公钥
         * @return bool 空目录或已移动的验签器返回 true
         */
        [[nodiscard]] bool empty() const noexcept;

    private:
        struct Implementation;

        FirmwareSignatureVerifier();

        explicit FirmwareSignatureVerifier(std::unique_ptr<Implementation> implementation);

        std::unique_ptr<Implementation> m_implementation; ///< 公钥表，键为 key id
    };
} // namespace EdgePulse::Firmware
