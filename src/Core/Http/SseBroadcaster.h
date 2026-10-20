/**
 * @file SseBroadcaster.h
 * @brief SSE 订阅者队列与广播
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace EdgePulse::Web
{
    /**
     * @brief 线程安全的 SSE 广播器
     * @details 发布只是入队；每个连接在自己的循环上按节拍取队列，
     *          因此跨线程发布不会碰对端连接对象。
     */
    class SseBroadcaster
    {
    public:
        /**
         * @brief 登记一个订阅者
         * @param tenantId 订阅者归属租户
         * @param platformAdmin 是否跨租户接收
         * @return std::uint64_t 订阅标识
         */
        std::uint64_t subscribe(std::int64_t tenantId = 0, bool platformAdmin = false);

        /**
         * @brief 注销订阅者
         * @param id 订阅标识
         */
        void unsubscribe(std::uint64_t id);

        /**
         * @brief 广播一条事件
         * @param event 已格式化的事件文本，调用方负责 SSE 字段格式
         * @param tenantId 事件归属租户；只投递给同租户或跨租户订阅者
         */
        void publish(std::string event, std::int64_t tenantId);

        /**
         * @brief 取走订阅者当前积压的事件
         * @param id 订阅标识
         * @param maximum 本次最多取走条数
         * @return std::vector<std::string> 事件
         */
        [[nodiscard]] std::vector<std::string> drain(std::uint64_t id, std::size_t maximum);

        /**
         * @brief 当前订阅者数量
         * @return std::size_t 数量
         */
        [[nodiscard]] std::size_t subscriberCount() const;

    private:
        struct Subscriber
        {
            std::int64_t             tenantId{0};          ///< 订阅者租户
            bool                     platformAdmin{false}; ///< 是否跨租户接收
            std::vector<std::string> pending;              ///< 待发事件
        };

        mutable std::mutex                            m_mutex;               ///< 保护订阅表
        std::unordered_map<std::uint64_t, Subscriber> m_subscribers;         ///< 订阅者
        std::uint64_t                                 m_nextId{1};           ///< 下一个标识
        static constexpr std::size_t                  kMaximumPending = 256; ///< 单订阅者积压上限
    };
} // namespace EdgePulse::Web
