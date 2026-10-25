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

#include <expected>
#include <string>
#include <string_view>

namespace EdgePulse::Agent
{
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
         * @brief 把最近一次 install 之前的证书与私钥写回配置路径
         * @return 成功为空；没有可恢复的上一份或写入失败时返回中文原因
         * @note 供上层在 HTTP 通道装载新身份失败时回退；恢复成功后清空备份，重复调用返回错误
         */
        [[nodiscard]] std::expected<void, std::string> restorePrevious();

        /**
         * @brief 丢弃尚未安装的私钥与 CSR
         */
        void discardPending();

        /**
         * @brief 取当前配置副本
         * @return const AgentConfiguration & 构造时记录的配置
         */
        [[nodiscard]] const AgentConfiguration &configuration() const noexcept
        {
            return m_configuration;
        }

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
