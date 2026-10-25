/**
 * @file LeaderTick.h
 * @brief 单飞周期任务的 tick 执行器：leader 门控与工作体绑定在一起
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <Core/Coroutine/Task.h>

#include <functional>
#include <string>

namespace EdgePulse::System
{
    class LeaderGate;

    /**
     * @brief 每 tick 先过 leader 门控再执行工作
     * @details 周期循环只交出自己的工作体，门控在本类内部执行，调用点无法先跑工作再判 leader。
     *          alwaysWork 用于跟随者也必须刷新的只读工作，例如 fleet 指标读数；它为空的 tick
     *          与原来的「follower 直接 continue」等价。
     * @note 只在周期循环所在线程上调用；本类不加锁
     */
    class LeaderTick
    {
    public:
        using Work = std::function<AsynGyanis::Core::Task<>()>;

        /**
         * @brief 绑定门控与工作体
         * @param gate 每 tick 询问一次的 leader 门控
         * @param leaderWork leader 上执行的工作；不能为空
         * @param alwaysWork 每个 tick 都执行的工作；可为空
         * @throws std::invalid_argument leaderWork 为空
         */
        LeaderTick(LeaderGate &gate, Work leaderWork, Work alwaysWork = {});

        /**
         * @brief 执行一个 tick：follower 跳过 leaderWork，alwaysWork 始终执行
         * @return AsynGyanis::Core::Task<> 本 tick 完成即返回
         * @note 门控在每次调用时都会询问，不缓存上一拍结论
         */
        AsynGyanis::Core::Task<> run();

    private:
        LeaderGate &m_gate;       ///< leader 门控
        Work        m_leaderWork; ///< 只在 leader 上执行
        Work        m_alwaysWork; ///< 每个 tick 都执行；可为空
    };
} // namespace EdgePulse::System
