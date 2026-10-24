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
     * @brief 在临时目录里生成自签证书与私钥
     * @param directory 临时目录
     * @param validitySeconds 相对当前时刻的有效期秒数，负数表示已过期
     * @param prefix 文件名前缀
     * @return CertificateMaterial 文件路径、指纹与到期文本
     * @throws std::runtime_error OpenSSL 生成、签名或写文件失败
     * @note POSIX 上私钥写成 0600，避免身份加载按权限拒绝
     */
    CertificateMaterial writeSelfSignedCertificate(const TestSupport::TemporaryDirectory &directory, long validitySeconds, const std::string &prefix);
} // namespace EdgePulse::Agent::Testing
