/**
 * @file AgentCertificateManager.h
 * @brief Agent 证书续期所需的密钥与 CSR 生成、证书安装与回退
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AgentConfiguration.h"
#include "AgentIdentity.h"

#include <chrono>
#include <expected>
#include <string>
#include <string_view>

namespace EdgePulse::Agent
{
    /// 续期窗口下限：生命周期再短也至少提前这么多开始续期
    inline constexpr std::chrono::hours kMinimumRenewalWindow{24};

    /**
     * @brief 按证书生命周期计算续期提前窗口
     * @param lifetime 当前证书的生命周期，未知时传 0
     * @param renewBeforeDaysOverride 配置的覆盖天数；0 表示按规范公式
     * @return std::chrono::seconds 提前窗口，取 max(25% 生命周期, 24h)；
     *         覆盖值只在大于 0 且小于生命周期时生效，否则回退到规范公式
     */
    [[nodiscard]] std::chrono::seconds renewalWindow(std::chrono::seconds lifetime, int renewBeforeDaysOverride);

    /**
     * @brief 判断配置的续期提前量能否用于给定生命周期
     * @param lifetime 证书生命周期，未知时传 0
     * @param renewBeforeDaysOverride 配置的覆盖天数；0 表示自动
     * @return bool 未配置覆盖值、或覆盖值小于生命周期时为 true
     */
    [[nodiscard]] bool renewalWindowOverrideValid(std::chrono::seconds lifetime, int renewBeforeDaysOverride);

    /**
     * @brief 判断一次证书安装失败是否属于不可重试的服务端签发参数错误
     * @param reason install() 返回的中文原因
     * @return bool 证书链、密钥配对、有效期或提前量不匹配时为 true
     * @note 这些原因不会因为重试同一份证书而变化；继续重新签发只会把服务端
     *       active 证书反复挤掉，必须停下来等运维修正配置
     */
    [[nodiscard]] bool permanentInstallFailure(std::string_view reason);

    /**
     * @brief 一次生成的续期材料
     */
    struct CertificateRequestMaterial
    {
        std::string privateKeyPem; ///< 新私钥的 PEM 文本，安装成功后才会落盘
        std::string csrPem;        ///< 与私钥配对的证书签名请求 PEM 文本
    };

    /**
     * @brief 管理续期私钥、CSR 与证书文件的安装
     * @details 生成的私钥只留在内存里，直到服务端签回的证书通过全部校验才与证书一起原子替换
     *          配置里的两个文件。替换前会把旧文件内容留作内存与磁盘备份；替换后若身份加载失败
     *          立即回退。所有方法都在调用线程同步执行，不访问网络。
     * @note 同一实例同一时刻只保留一份待安装材料；重新生成会丢弃上一份。
     */
    class AgentCertificateManager
    {
    public:
        /**
         * @brief 记录证书、私钥与 CA 的路径
         * @param configuration 已通过形态校验的配置
         */
        explicit AgentCertificateManager(AgentConfiguration configuration);

        AgentCertificateManager(const AgentCertificateManager &)            = delete;
        AgentCertificateManager &operator=(const AgentCertificateManager &) = delete;

        /**
         * @brief 生成 EC P-256 私钥与对应的 CSR
         * @return 成功返回 PEM 文本；失败返回中文原因
         * @note 材料同时留在实例里，供随后调用 install() 校验服务端签回的证书；CSR 主题取配置里的节点名
         */
        [[nodiscard]] std::expected<CertificateRequestMaterial, std::string> generateKeyAndCsr();

        /**
         * @brief 校验服务端签回的证书并安装到配置路径
         * @param certificatePem 服务端返回的证书 PEM，可含中间证书
         * @return 成功返回新身份（指纹与到期时间）；失败返回中文原因，磁盘上的旧文件保持不变
         * @details 依次校验：PEM 可解析、notAfter 晚于当前时刻、公钥与待安装私钥配对、
         *          证书链能落到配置的 CA、临时 TLS 上下文能用这对文件装载身份。全部通过后才写
         *          证书与私钥（私钥 0600），并用 AgentIdentity::load 复核最终文件。
         * @note 没有待安装私钥、或写入中途失败时返回错误；写入失败会尽力把旧文件写回
         */
        [[nodiscard]] std::expected<AgentIdentity, std::string> install(std::string_view certificatePem);

        /**
         * @brief 丢弃本次安装留下的回退材料
         * @details HTTP 与 WebSocket 两条通道都已装载新身份后调用：删除磁盘上的 .bak 并清空内存备份。
         *          删除失败只记录 WARN，不影响已经生效的身份。
         */
        void commitInstalledIdentity();

        /**
         * @brief 处理上次进程在两次替换之间中断留下的安装标记
         * @param configuration 已通过形态校验的配置
         * @return 成功时返回是否回退了旧材料；读标记或回退失败时返回中文原因
         * @details 标记由 install() 在两份正式文件被替换之前写下，里面记录证书、私钥与两份 .bak
         *          的路径。启动时若标记存在：配置路径上的证书与私钥能配对装载，说明上次替换已经
         *          完成，只删标记；配对失败说明进程死在两次替换之间，把两份 .bak 写回配置路径
         *          后再删标记。没有标记时直接返回 false。
         * @note 必须在加载身份之前调用；回退失败时保留标记与备份，下次启动继续尝试
         */
        [[nodiscard]] static std::expected<bool, std::string> recoverInterruptedInstall(const AgentConfiguration &configuration);

    private:
        /**
         * @brief 把最近一次 install 之前的文件写回配置路径
         * @return 成功为空；失败返回中文原因
         */
        [[nodiscard]] std::expected<void, std::string> restoreFromMemory();

        AgentConfiguration         m_configuration;          ///< 证书、私钥与 CA 的路径
        CertificateRequestMaterial m_pending;                ///< 待安装的私钥与 CSR，空表示没有
        bool                       m_hasPrevious{false};     ///< 是否有可回退的上一份材料
        std::string                m_previousCertificatePem; ///< 上一份证书原文
        std::string                m_previousPrivateKeyPem;  ///< 上一份私钥原文
    };
} // namespace EdgePulse::Agent
