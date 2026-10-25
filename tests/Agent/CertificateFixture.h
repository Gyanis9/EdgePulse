/**
 * @file CertificateFixture.h
 * @brief 测试用临时自签证书与私钥的生成入口
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "TestSupport/TemporaryDirectory.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace EdgePulse::Agent::Testing
{
    /**
     * @brief 一次生成的证书材料
     */
    struct CertificateMaterial
    {
        std::filesystem::path certificateFile; ///< 自签叶证书，同时可充当 CA
        std::filesystem::path privateKeyFile;  ///< 与证书配对的私钥
        std::string           fingerprint;     ///< 证书 SHA-256 指纹，小写十六进制
        std::string           expiresAt;       ///< 到期时间，形如 2026-10-24T12:00:00Z
    };

    /**
     * @brief 测试用内部 CA 的证书与私钥
     */
    struct CertificateAuthorityMaterial
    {
        std::filesystem::path certificateFile; ///< CA 证书，可作客户端信任锚
        std::filesystem::path privateKeyFile;  ///< CA 私钥
        std::string           fingerprint;     ///< CA 证书 SHA-256 指纹
    };

    /**
     * @brief 一次按 CSR 签发的叶证书
     */
    struct SignedCertificateMaterial
    {
        std::string certificatePem; ///< 叶证书 PEM
        std::string fingerprint;    ///< 叶证书 SHA-256 指纹
        std::string expiresAt;      ///< 到期时间，形如 2026-10-24T12:00:00Z
    };

    /**
     * @brief 在临时目录里生成自签证书与私钥
     * @param directory 临时目录
     * @param validitySeconds 相对当前时刻的有效期秒数，负数表示已过期
     * @param prefix 文件名前缀
     * @return CertificateMaterial 文件路径、指纹与到期文本
     * @throws std::runtime_error OpenSSL 生成、签名或写文件失败
     * @note POSIX 上私钥写成 0600，避免身份加载按权限拒绝
     */
    CertificateMaterial writeSelfSignedCertificate(const TestSupport::TemporaryDirectory &directory, long validitySeconds, const std::string &prefix);

    /**
     * @brief 在临时目录里生成一张自签 CA（basicConstraints CA:TRUE）
     * @param directory 临时目录
     * @param prefix 文件名前缀
     * @return CertificateAuthorityMaterial 证书、私钥与指纹
     * @throws std::runtime_error OpenSSL 生成、签名或写文件失败
     */
    CertificateAuthorityMaterial writeCertificateAuthority(const TestSupport::TemporaryDirectory &directory, const std::string &prefix);

    /**
     * @brief 在临时目录里生成一张由根 CA 签发的中级 CA
     * @param directory 临时目录
     * @param prefix 文件名前缀
     * @param root 签发中级证书的根 CA
     * @return CertificateAuthorityMaterial 中级证书、私钥与指纹
     * @throws std::runtime_error 根 CA 材料读取或签发失败
     * @note 中级证书带 CA:TRUE 与 keyCertSign，可用于签发叶证书以构造三段链
     */
    CertificateAuthorityMaterial writeIntermediateCertificateAuthority(const TestSupport::TemporaryDirectory &directory, const std::string &prefix,
                                                                       const CertificateAuthorityMaterial &root);

    /**
     * @brief 用测试 CA 按 CSR 签发一张客户端叶证书
     * @param authority 签发用的 CA 材料
     * @param csrPem CSR 的 PEM 文本
     * @param validitySeconds 相对当前时刻的有效期秒数，负数表示已过期
     * @return SignedCertificateMaterial 叶证书 PEM、指纹与到期文本
     * @throws std::runtime_error CSR 解析或自签校验失败、签发失败
     * @note 主题原样取自 CSR，扩展名与服务端签发器一致：CA:FALSE、digitalSignature 与 clientAuth
     */
    SignedCertificateMaterial signCertificateRequest(const CertificateAuthorityMaterial &authority, std::string_view csrPem, long validitySeconds);
} // namespace EdgePulse::Agent::Testing
