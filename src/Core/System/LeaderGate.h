/**
 * @file LeaderGate.h
 * @brief 周期任务的 leader 门控与状态切换日志
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace EdgePulse::System
{
    /**
     * @brief 每 tick 判定本实例是否该执行单飞工作
     * @details leader 丢失时只在切换的那一拍记一条日志，之后保持静默；重新当选时同样只记一次。
     *          门控本身不加锁，只在所在循环线程上调用。
     */
    class LeaderGate
    {
    public:
        /**
         * @brief 构造门控
         * @param taskName 周期任务名，用于日志
         * @param instanceId 本实例标识，用于日志；可为空
         * @param isLeader leader 判定回调，必须非空
         * @param fence fence 读取回调，可为空；为空时日志不带 fence
         * @throws std::invalid_argument isLeader 为空
         */
        LeaderGate(std::string taskName, std::string instanceId, std::function<bool()> isLeader, std::function<std::int64_t()> fence = {});

        /**
         * @brief 判定本 tick 是否执行工作
         * @return bool 回调返回 true 时才允许执行
         * @note follower 返回 false；leader 与 follower 的切换各记一条日志，不按 tick 重复
         */
        [[nodiscard]] bool allowsWork();

        /**
         * @brief 任务名
         * @return const std::string & 构造时传入的名字
         */
        [[nodiscard]] const std::string &taskName() const noexcept;

    private:
        /**
         * @brief 组装日志里的实例与 fence 描述
         * @return std::string 形如 instance=node-a fence=3
         */
        [[nodiscard]] std::string describeContext() const;

        std::string                   m_taskName;   ///< 任务名
        std::string                   m_instanceId; ///< 实例标识；空时不打印
        std::function<bool()>         m_isLeader;   ///< leader 判定
        std::function<std::int64_t()> m_fence;      ///< fence 读取
        std::optional<bool>           m_lastLeader; ///< 上一拍结论；首拍只记录不打印切换
    };
} // namespace EdgePulse::System
