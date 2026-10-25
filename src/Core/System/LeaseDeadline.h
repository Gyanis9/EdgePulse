/**
 * @file LeaseDeadline.h
 * @brief 把数据库里的租约到期时刻换算成本进程的绝对单调截止
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <chrono>
#include <cstdint>

namespace EdgePulse::System
{
    /**
     * @brief 按数据库工作线程取到授权的同一时刻，算出本地绝对单调截止
     * @details 截止等于「取到授权时的单调刻度」加「库内到期时刻减去同一时刻的墙上时间」。
     *          两项都在数据库工作线程上取得，协程多久之后才在事件循环上恢复都不再参与换算，
     *          事件循环停摆因此不会把本地截止推到库内到期之后。
     *          库内到期只保存到秒，先把到期时刻向下取整到秒再相减，换算出的截止只会更早。
     *          结果再夹在 TTL 之内，防止墙上时钟异常把 leadership 拉长到一个 TTL 以上。
     * @param workerSteadyNanoseconds 数据库工作线程取到的单调刻度（steady_clock 纳秒）
     * @param workerWallNow 同一段工作线程执行里取到的墙上时间，必须与上一项紧邻采样
     * @param rowExpiresAt 库内到期时刻，按写入时的墙上时间表示
     * @param ttl 本次获取或续约写入的有效期
     * @return std::int64_t 绝对单调截止（steady_clock 纳秒）；行已过期时等于取到授权的刻度
     * @note 判据是「当前单调刻度小于截止」；返回值等于传入刻度时该判据为 false
     */
    [[nodiscard]] inline std::int64_t leaseDeadlineNanoseconds(const std::int64_t workerSteadyNanoseconds, const std::chrono::system_clock::time_point workerWallNow,
                                                               const std::chrono::system_clock::time_point rowExpiresAt, const std::chrono::seconds ttl) noexcept
    {
        const auto rowExpiry = std::chrono::floor<std::chrono::seconds>(rowExpiresAt);
        if (rowExpiry <= workerWallNow)
        {
            return workerSteadyNanoseconds;
        }
        auto       remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(rowExpiry - workerWallNow);
        const auto maximum   = std::chrono::duration_cast<std::chrono::nanoseconds>(ttl);
        if (remaining > maximum)
        {
            remaining = maximum;
        }
        return workerSteadyNanoseconds + remaining.count();
    }
} // namespace EdgePulse::System
