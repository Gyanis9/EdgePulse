/**
 * @file CertificateBinding.h
 * @brief 把 mTLS 连接证书与节点已登记指纹绑定的判定
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <Core/Tls/TlsPeerIdentity.h>

#include <optional>
#include <string>
#include <string_view>

namespace EdgePulse::Device
{
    /**
     * @brief 校验并归一化证书指纹
     * @param fingerprint 原始输入
     * @return 64 位小写十六进制；非法时为空
     */
    [[nodiscard]] std::optional<std::string> normalizeCertificateFingerprint(std::string_view fingerprint);

    /**
     * @brief 判定连接证书能否代表已登记节点
     * @details 非 TLS 链路不参与绑定：开发与测试的明文监听器没有对端身份，这条路径的
     *          身份保证由监听器本身提供。TLS 链路上要求连接证书已通过校验，且叶证书的
     *          SHA-256 与登记值逐字一致，否则调用方应拒绝请求。
     * @param connectionIdentity 连接的对端身份；空表示非 TLS 链路
     * @param registeredFingerprint 节点已登记的指纹
     * @return 不通过时的中文原因；通过为空
     */
    [[nodiscard]] std::optional<std::string> certificateBindingFailure(const AsynGyanis::Core::TlsPeerIdentityPtr &connectionIdentity, std::string_view registeredFingerprint);
} // namespace EdgePulse::Device
