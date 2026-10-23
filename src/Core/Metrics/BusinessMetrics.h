/**
 * @file BusinessMetrics.h
 * @brief 进程级业务读数：节点在线数、告警与命令积压等
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <Core/Metrics/ProcessMetricsRegistry.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace EdgePulse::Metrics
{
    /**
     * @brief 业务读数单例
     * @details 读数按进程汇总，不随请求或租户区分。用单例是为了让鉴权、通知等底层组件
     *          直接累加计数器，不必为一条指标再往各层依赖结构里加参数；本类只保存原子量，
     *          不承载任何业务状态，写入方仍是各自的服务。
     */
    class BusinessMetrics
    {
    public:
        /**
         * @brief 取进程内唯一实例
         * @return BusinessMetrics& 实例，活到进程退出
         */
        static BusinessMetrics &instance();

        BusinessMetrics(const BusinessMetrics &)            = delete;
        BusinessMetrics &operator=(const BusinessMetrics &) = delete;

        /**
         * @brief 向进程指标注册表登记全部业务读数
         * @note 重复调用只登记一次；注册表在抓取时回调读取本对象的原子量
         */
        void registerMetrics();

        /// 刷新在线与离线节点数
        void setNodeCounts(std::int64_t online, std::int64_t offline);

        /// 刷新未恢复告警数
        void setAlarmsOpen(std::int64_t open);

        /// 刷新待完成命令数
        void setCommandsPending(std::int64_t pending);

        /// 刷新七天内到期的 Agent 证书数
        void setAgentCertificatesExpiring(std::int64_t count);

        /// 刷新未签名固件数
        void setUnsignedFirmware(std::int64_t count);

        /// 累加一次在线转离线
        void incrementOfflineTransitions();

        /// 累加一次 Agent 身份校验拒绝
        void incrementIdentityRejections();

        /// 累加一次 Webhook 地址安全拒绝
        void incrementWebhookSsrfRejections();

    private:
        BusinessMetrics() = default;

        std::once_flag                                     m_registration;             ///< 保证指标只登记一次
        std::vector<AsynGyanis::Core::ProcessMetricHandle> m_handles;                  ///< 登记把手，随实例存活
        std::atomic<std::uint64_t>                         m_online{0};                ///< 在线节点数
        std::atomic<std::uint64_t>                         m_offline{0};               ///< 离线节点数
        std::atomic<std::uint64_t>                         m_alarmsOpen{0};            ///< 未恢复告警数
        std::atomic<std::uint64_t>                         m_commandsPending{0};       ///< 待完成命令数
        std::atomic<std::uint64_t>                         m_certificatesExpiring{0};  ///< 七天内到期证书数
        std::atomic<std::uint64_t>                         m_unsignedFirmware{0};      ///< 未签名固件数
        std::atomic<std::uint64_t>                         m_offlineTransitions{0};    ///< 离线转换累计
        std::atomic<std::uint64_t>                         m_identityRejections{0};    ///< 身份校验拒绝累计
        std::atomic<std::uint64_t>                         m_webhookSsrfRejections{0}; ///< Webhook 地址拒绝累计
    };
} // namespace EdgePulse::Metrics
