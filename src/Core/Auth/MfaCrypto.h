/**
 * @file MfaCrypto.h
 * @brief 两步验证秘密的 AES-256-GCM 封装
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace EdgePulse::Auth
{
    /// 两步验证主密钥字节数；配置里是 base64 文本，解码后必须正好 32 字节
    inline constexpr std::size_t kMfaKeyBytes = 32;

    /// GCM 推荐 nonce 长度，单位字节
    inline constexpr std::size_t kMfaNonceBytes = 12;

    /// GCM 认证标签长度，单位字节
    inline constexpr std::size_t kMfaTagBytes = 16;

    /**
     * @brief 已加密的秘密；两段都是 base64 文本，可直接落库
     */
    struct MfaCiphertext
    {
        std::string nonce;      ///< 12 字节 nonce 的 base64
        std::string ciphertext; ///< 密文与 16 字节认证标签拼接后的 base64
    };

    /**
     * @brief 两步验证秘密的对称加密
     * @details 每次加密生成新的 12 字节随机 nonce，认证标签附在密文尾部；解密失败只返回空值，
     *          不区分密钥错误、篡改与格式损坏，避免把失败原因交给调用方或用户。
     */
    namespace MfaCrypto
    {
        /**
         * @brief 用 AES-256-GCM 加密秘密
         * @param plaintext 待加密文本，按字节处理
         * @param key 32 字节原始密钥
         * @return std::optional<MfaCiphertext> 成功返回 base64 的 nonce 与密文；密钥长度不对或加密失败返回空
         */
        [[nodiscard]] std::optional<MfaCiphertext> encrypt(std::string_view plaintext, std::string_view key);

        /**
         * @brief 解密并校验认证标签
         * @param ciphertext base64 的密文与标签
         * @param nonce base64 的 12 字节 nonce
         * @param key 32 字节原始密钥
         * @return std::optional<std::string> 成功返回明文；任何失败都返回空，不抛异常
         */
        [[nodiscard]] std::optional<std::string> decrypt(std::string_view ciphertext, std::string_view nonce, std::string_view key);
    } // namespace MfaCrypto
} // namespace EdgePulse::Auth
