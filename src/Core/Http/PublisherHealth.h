/**
 * @file PublisherHealth.h
 * @brief 实时事件发布连接的恢复状态机
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <atomic>
#include <functional>

namespace EdgePulse::Web
{
    /**
     * @brief 发布连接的健康标记与按需探针
     * @details 发布失败时标为不健康；周期检查只在已判定不健康时调用一次注入的恢复探针，
     *          恢复后回到只读原子的快路径。探针本身负责重连与 PING，本类不加锁。
     * @note 可在发布线程与订阅线程同时使用，状态只有一个原子量
     */
    class PublisherHealth
    {
    public:
        using Probe = std::function<bool()>;

        /**
         * @brief 最近一次已知健康状态
         * @return bool true 表示发布连接可用
         */
        [[nodiscard]] bool isHealthy() const noexcept;

        /**
         * @brief 把状态标为健康
         */
        void markHealthy() noexcept;

        /**
         * @brief 把状态标为不健康，下一次周期检查会尝试恢复
         */
        void markUnhealthy() noexcept;

        /**
         * @brief 按需执行一次有界恢复探针
         * @param probe 重连并 PING 的探针；返回 true 表示恢复
         * @param force true 时即使当前健康也执行一次，用于发布失败后的立即恢复
         * @return bool 探针之后的状态
         * @note 已健康且未强制时不会调用 probe；空 probe 视为恢复失败
         */
        bool probe(const Probe &probe, bool force = false);

    private:
        std::atomic<bool> m_healthy{false}; ///< 发布连接最近一次已知状态
    };
} // namespace EdgePulse::Web
