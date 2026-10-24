/**
 * @file AgentCertificateAuthority.h
 * @brief 控制面内部 CA：校验 Agent 的 CSR 并签发终端证书
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace EdgePulse::Device
{
    /**
     * @brief Agent 内部 CA 的启动参数
     */
    struct AgentCaOptions
    {
        bool                  enabled{false};                      ///< 是否启用自动签发
        std::filesystem::path certificateFile{"var/certs/ca.pem"}; ///< CA 证书链 PEM
        std::filesystem::path keyFile{"var/certs/ca-key.pem"};     ///< CA 私钥 PEM，POSIX 上要求 0600 或更严
        std::filesystem::path trustedCertificateFile;              ///< 9443 监听器使用的客户端信任库；非空时签发 CA 必须出现在其中，可拼接多张证书
        int                   validityDays{30};                    ///< 签发证书的有效期，按天计
        int                   overlapHours{24};                    ///< 新旧证书重叠窗口，按小时计
    };

    /**
     * @brief 一次签发的结果
     */
    struct IssuedAgentCertificate
    {
        std::string certificatePem; ///< 新证书的 PEM 文本
        std::string fingerprint;    ///< 证书 SHA-256 指纹，64 位小写十六进制
        std::string serial;         ///< 序列号，小写十六进制
        std::string subject;        ///< 审计用主题，CN=<节点名> 的 UTF-8 原文
        std::string notBefore;      ///< 生效时间，UTC，形如 2026-10-25 12:00:00
        std::string notAfter;       ///< 到期时间，UTC，形如 2026-10-25 12:00:00
    };

    /**
     * @brief 用控制面私钥为 Agent 的 CSR 签发终端证书
     * @details CSR 只提供公钥与自签证明，主题、有效期与全部扩展都由服务端重建：
     *          主题固定为 CN=节点名，SAN 为 URI:edgepulse://node/<节点标识>，
     *          用途固定为 clientAuth，基本约束固定为 CA:FALSE。这样 Agent 无法通过
     *          CSR 自带字段把自己声明成 CA 或别的身份。类持有 CA 证书与私钥，只能移动。
     */
    class AgentCertificateAuthority
    {
    public:
        ~AgentCertificateAuthority();

        AgentCertificateAuthority(AgentCertificateAuthority &&other) noexcept;
        AgentCertificateAuthority &operator=(AgentCertificateAuthority &&other) noexcept;
        AgentCertificateAuthority(const AgentCertificateAuthority &)            = delete;
        AgentCertificateAuthority &operator=(const AgentCertificateAuthority &) = delete;

        /**
         * @brief 加载 CA 证书与私钥并校验配对
         * @param options 启动参数；未启用时只返回禁用状态，不读取任何文件
         * @return 成功返回签发器；文件缺失、无法解析、私钥权限过宽、证书不是可用 CA、证书与私钥不配对或
         *         签发 CA 不在监听器信任库内时返回中文原因
         */
        [[nodiscard]] static std::expected<AgentCertificateAuthority, std::string> load(const AgentCaOptions &options);

        /**
         * @brief 校验 CSR 并签发终端证书
         * @param csrPem CSR 的 PEM 文本
         * @param nodeId 节点标识，必须为正整数，用于拼 SAN URI
         * @param nodeName 节点名，用作证书主题 CN
         * @return 成功返回证书 PEM、指纹、序列号、主题与起止时间；失败返回中文原因，不回显 CSR 或私钥内容
         */
        [[nodiscard]] std::expected<IssuedAgentCertificate, std::string> issue(const std::string &csrPem, std::int64_t nodeId, const std::string &nodeName);

        /**
         * @brief 是否已启用签发
         * @return bool 禁用或已移动的签发器返回 false
         */
        [[nodiscard]] bool enabled() const noexcept;

    private:
        struct Implementation;

        AgentCertificateAuthority();

        explicit AgentCertificateAuthority(std::unique_ptr<Implementation> implementation);

        std::unique_ptr<Implementation> m_implementation; ///< CA 证书、私钥与签发参数
    };
} // namespace EdgePulse::Device
