/**
 * @file Jwt.h
 * @brief OIDC ID Token 的 RS256 校验与载荷读取
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace EdgePulse::Auth
{
    /**
     * @brief JWKS 里的一把 RSA 公钥
     */
    struct JwtRsaKey
    {
        std::string keyId;    ///< kid
        std::string modulus;  ///< base64url 的 n
        std::string exponent; ///< base64url 的 e
    };

    /**
     * @brief ID Token 中本平台需要的声明
     */
    struct JwtClaims
    {
        std::string              issuer;            ///< iss
        std::string              subject;           ///< sub
        std::vector<std::string> audience;          ///< aud，单值折成一项
        std::int64_t             expiresAt{0};      ///< exp，Unix 秒
        std::string              nonce;             ///< nonce
        std::string              email;             ///< email
        std::string              preferredUsername; ///< preferred_username
        std::string              displayName;       ///< name
    };

    /**
     * @brief 校验结果
     */
    struct JwtVerification
    {
        bool        ok{false}; ///< 签名与声明都通过
        std::string error;     ///< 中文失败原因
        JwtClaims   claims;    ///< ok 为真时有效
    };

    /**
     * @brief 校验 RS256 签名的 ID Token
     * @param token 紧凑序列化的 JWT
     * @param keys JWKS 公钥
     * @param expectedIssuer 期望的 iss
     * @param expectedAudience 期望出现在 aud 里的 client_id
     * @param expectedNonce 期望的 nonce；空表示不校验
     * @param nowSeconds 当前 Unix 秒
     * @return JwtVerification 结果
     * @note 只接受 alg=RS256：本平台不引入第二条签名算法链
     */
    [[nodiscard]] JwtVerification verifyRs256Jwt(std::string_view token, const std::vector<JwtRsaKey> &keys, std::string_view expectedIssuer, std::string_view expectedAudience,
                                                 std::string_view expectedNonce, std::int64_t nowSeconds);

    /**
     * @brief base64url 解码
     * @param text 无填充的 base64url
     * @return std::optional<std::string> 解码字节；非法字符或长度错误时为空
     */
    [[nodiscard]] std::optional<std::string> decodeBase64Url(std::string_view text);

    /**
     * @brief 计算 PKCE 的 S256 challenge
     * @param verifier 原始 verifier
     * @return std::string 无填充的 base64url(SHA-256(verifier))
     */
    [[nodiscard]] std::string pkceS256Challenge(std::string_view verifier);
} // namespace EdgePulse::Auth
