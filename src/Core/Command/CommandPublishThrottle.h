/**
 * @file CommandPublishThrottle.h
 * @brief 计划命令的重复发布节流
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace EdgePulse::Command
{
    /**
     * @brief 限制同一条计划命令在固定间隔内只发布一次
     * @details 计划调度器每 2 秒扫描一次到期命令；没有 Agent 连接时命令会一直留在 pending，
     *          若每轮都重新发布，Redis 与所有实例的订阅线程都会被同一条命令反复唤醒。本类按
     *          命令标识记住上次发布时间，间隔内不再发布。命令离开到期集合（已执行、已取消或
     *          计划时间未到）时由 prune() 清掉记录，标识不会无限增长。
     * @note 只在计划调度器所在的 leader 循环上使用，内部不加锁；领导权切换后新 leader 从空表
     *       开始，最多多发布一次，由 Agent 侧的命令去重收口。
     */
    class CommandPublishThrottle
    {
    public:
        /// 当前时刻来源；为空时使用 steady_clock::now()
        using NowFunction = std::function<std::chrono::steady_clock::time_point()>;

        /// 默认重复发布间隔：30 秒足够覆盖一次跨实例转发与 Agent 重连，且不会让命令长期滞留
        static constexpr std::chrono::seconds kDefaultInterval{30};

        /**
         * @brief 构造节流器
         * @param interval 同一命令的最短发布间隔；非正值退回 30 秒
         * @param now 当前时刻来源，测试注入假时钟
         */
        explicit CommandPublishThrottle(std::chrono::seconds interval = kDefaultInterval, NowFunction now = {});

        /**
         * @brief 判断本次是否应当发布，并记录发布时间
         * @param commandId 命令标识
         * @return bool 首次出现或距上次发布已满间隔时为 true
         */
        [[nodiscard]] bool shouldPublish(std::int64_t commandId);

        /**
         * @brief 清掉不再属于到期集合的命令记录
         * @param dueCommandIds 本轮扫描到的到期命令标识
         */
        void prune(const std::unordered_set<std::int64_t> &dueCommandIds);

        /**
         * @brief 当前记住的命令条数
         * @return std::size_t 条数
         */
        [[nodiscard]] std::size_t trackedCount() const noexcept;

    private:
        /**
         * @brief 取当前时刻
         * @return std::chrono::steady_clock::time_point 注入时钟或系统单调时钟
         */
        [[nodiscard]] std::chrono::steady_clock::time_point now() const;

        std::chrono::seconds                                                    m_interval;      ///< 最短发布间隔
        NowFunction                                                             m_now;           ///< 当前时刻来源
        std::unordered_map<std::int64_t, std::chrono::steady_clock::time_point> m_lastPublished; ///< 命令标识到最后一次发布时刻
    };
} // namespace EdgePulse::Command
