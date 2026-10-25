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
     * @note 可在发布线程与订阅线程同时使用，状态只有一个原子量。调用 probe() 的线程必须在
     *       发布连接锁内执行，让状态写回与连接恢复同属一个临界区：发布线程只在自己的锁内
     *       因真实 PUBLISH 失败标不健康，恢复成功写回的 true 不会被发布路径的旧判断覆盖
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
         * @return bool 探针之后的状态
         * @note 已健康时不会调用 probe；空 probe 视为恢复失败。调用方必须持有发布连接锁，
         *       让状态写回与连接恢复同属一个临界区
         */
        bool probe(const Probe &probe);

    private:
        std::atomic<bool> m_healthy{false}; ///< 发布连接最近一次已知状态
    };
} // namespace EdgePulse::Web
