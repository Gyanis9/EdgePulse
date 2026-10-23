/**
 * @file FirmwareSigningPolicy.h
 * @brief 固件签名门禁：是否强制签名与信任键
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <memory>

namespace EdgePulse::Firmware
{
    class FirmwareSignatureVerifier;

    /**
     * @brief 固件签名策略
     */
    struct FirmwareSigningPolicy
    {
        bool                                       requireSignature{false}; ///< 是否拒绝未签名固件的上传与下发
        std::shared_ptr<FirmwareSignatureVerifier> verifier;                ///< 信任键；为空表示没有可校验的公钥

        /**
         * @brief 是否强制校验签名
         * @return bool 开启时上传与下发都必须带签名
         */
        [[nodiscard]] bool signatureRequired() const noexcept;
    };
} // namespace EdgePulse::Firmware
