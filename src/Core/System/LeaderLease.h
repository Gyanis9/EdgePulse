/**
 * @file LeaderLease.h
 * @brief 数据库支撑的控制面单飞租约
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <Core/Coroutine/Task.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Common
{
    class Database;
}

namespace EdgePulse::System
{
    /**
     * @brief 用 sys_lease 的一行表达控制面 leader：条件 UPDATE 成功才是 leader
     * @details 获取与续约都只发一条带条件的 UPDATE，时间参数在数据库工作线程上按执行时刻生成，
     *          由数据库的行锁给出跨实例的互斥；获取命中后在同一连接里回读，只有库里记录的
     *          到期时间仍在未来才承认 leader。续约同时要求 holder、fence 与未过期，延迟到达的
     *          续约不能把已过期的行再延长。
     *          fence 每次获取成功递增，续约按 holder 与 fence 做 CAS，旧 leader 恢复后
     *          无法继续延长或写回租约。获取条件不接受 holder=自己，两个实例配了同一个
     *          instanceId 时后来者不会与先来者共享同一行，必须等 TTL 过期；代价是进程
     *          崩溃后重启最长要等一个 TTL 才能重新成为 leader。
     *          每次成功获取或续约都按完成时刻刷新本地单调截止，isLeader 在截止过后立即变 false，
     *          即使下一次续约还没跑，被暂停或拖慢的循环也不会继续执行只允许 leader 做的工作。
     *          续约遇到数据库异常时，只要本地截止未到就保留最后已知的 leadership 并重试；
     *          获取异常与续约 CAS 失败仍立即降为 follower。
     * @note 所有实例的系统时钟必须由 NTP 对齐：expires_at 的写入与比较都取本机 UTC 时间，
     *       时钟偏差超过 TTL 会让仍存活的 holder 被其他实例误判为过期，形成双 leader。
     */
    class LeaderLease
    {
    public:
        /**
         * @brief 构造一个尚未启动的租约
         * @param database 进程级数据库
         * @param instanceId 本实例标识，必须非空、不含空白且不超过 128 字符；同时存活的实例必须唯一
         * @param name 租约行名，默认 control-plane
         * @param ttl 每次获取或续约写入的有效期
         * @param renewInterval 续约周期；跟随者同样按该周期重试获取
         * @throws std::invalid_argument instanceId 或时间参数非法
         */
        LeaderLease(std::shared_ptr<Common::Database> database, std::string instanceId, std::string name = "control-plane", std::chrono::seconds ttl = std::chrono::seconds{15},
                    std::chrono::milliseconds renewInterval = std::chrono::milliseconds{5000});

        /**
         * @brief 停止租约并等内部循环退出后销毁帧
         * @note 析构会调用 stop()；事件循环正在运行时最多等两秒让循环协程自行收口，
         *       避免在循环线程仍持有计时器等待器时销毁协程帧；循环已经停止时不再等待，
         *       此时不会再有定时器回调访问该帧
         */
        ~LeaderLease();

        LeaderLease(const LeaderLease &)            = delete;
        LeaderLease &operator=(const LeaderLease &) = delete;
        LeaderLease(LeaderLease &&)                 = delete;
        LeaderLease &operator=(LeaderLease &&)      = delete;

        /**
         * @brief 在事件循环上先尝试获取一次，再启动周期续约循环
         * @param loop 事件循环
         * @return true 首次获取成功；false 首次未获取到、数据库报错或循环调度被拒
         * @note 只能在事件循环线程上 co_await；重复调用返回当前 leader 状态，不会启动第二个循环。
         *       首次未获取或数据库报错时循环仍会按续约周期重试；本地调度被拒时收回半启动状态
         *       并返回 false，已写入的 holder 会在 TTL 后自然失效。
         *       事件循环必须比本对象活得久：析构会查询它是否仍在运行
         */
        AsynGyanis::Core::Task<bool> start(AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 请求停止并立即清空自己的 holder
         * @note 幂等；清空条件带 holder=instanceId 与当前 fence，同 instanceId 的后来者已经
         *       接手时 fence 不同，不会被先来者的 stop 清掉。内部循环最多在一个轮询片
         *       （250 毫秒）内观察到停止请求并退出。
         */
        void stop();

        /**
         * @brief 最近一次观察到的 leader 状态
         * @return true 最近一次获取或续约成功、未收到停止请求且本地单调截止尚未到
         * @note 截止由成功获取或续约的完成时刻加 TTL 得到，不依赖数据库时钟；
         *       截止过后即使续约循环还没醒也返回 false
         */
        [[nodiscard]] bool isLeader() const noexcept;

        /**
         * @brief 最近一次成功获取时数据库返回的 fence
         * @return std::int64_t fence；失去租约后保留最后成功值，不重置
         */
        [[nodiscard]] std::int64_t fence() const noexcept;

        /**
         * @brief 本实例标识
         * @return const std::string& 构造时传入的 instanceId
         */
        [[nodiscard]] const std::string &instanceId() const noexcept;

    private:
        /**
         * @brief 周期续约；非 leader 时改为重试获取
         * @param loop 事件循环
         * @return AsynGyanis::Core::Task<> 循环协程，活到 LeaderLease 析构
         */
        AsynGyanis::Core::Task<> runLoop(AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 发起一次条件获取
         * @param loop 事件循环
         * @return true 本次 UPDATE 命中、库内到期时间仍在未来且读回 fence
         */
        AsynGyanis::Core::Task<bool> acquireOnce(AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 按 holder 与 fence 做一次条件续约
         * @param loop 事件循环
         * @return true 本条实例仍是 holder 且续约后的到期时间仍在未来
         */
        AsynGyanis::Core::Task<bool> renewOnce(AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 用连接池连接清空自己的 holder
         * @param loop 事件循环
         * @param fence 清空条件里要求的 fence；停止与循环退出时传当前观察到的值
         * @return AsynGyanis::Core::Task<> 清理完成即返回
         */
        AsynGyanis::Core::Task<> releaseHolder(AsynGyanis::Core::EventLoop &loop, std::int64_t fence);

        /**
         * @brief 记录一次成功的获取或续约并刷新本地截止
         * @param fence 数据库返回的 fence
         * @param remainingTtl 按库内到期时间算出的剩余有效期
         */
        void markLeader(std::int64_t fence, std::chrono::milliseconds remainingTtl) noexcept;

        /**
         * @brief 标记当前不再是 leader
         */
        void markFollower() noexcept;

        std::shared_ptr<Common::Database>          m_database;          ///< 进程级数据库
        std::string                                m_instanceId;        ///< 本实例标识
        std::string                                m_name;              ///< 租约行名
        std::chrono::seconds                       m_ttl;               ///< 租约有效期
        std::chrono::milliseconds                  m_renewInterval;     ///< 续约与重试周期
        std::atomic<bool>                          m_leader{false};     ///< 最近一次观察到的 leader 状态
        std::atomic<std::int64_t>                  m_leaderDeadline{0}; ///< 本地单调截止（steady_clock 纳秒），与 m_leader 配对
        std::atomic<std::int64_t>                  m_fence{0};          ///< 最近一次成功获取的 fence
        std::atomic<bool>                          m_stopped{false};    ///< 是否已请求停止
        std::atomic<bool>                          m_started{false};    ///< 是否已启动过循环
        std::atomic<bool>                          m_loopExited{true};  ///< 周期循环是否已退出
        std::atomic<AsynGyanis::Core::EventLoop *> m_loop{nullptr};     ///< 启动时绑定的事件循环，仅用于析构判断是否仍在运行
        std::mutex                                 m_exitMutex;         ///< 退出通知的互斥量
        std::condition_variable                    m_exitCondition;     ///< 周期循环退出时唤醒析构
        std::optional<AsynGyanis::Core::Task<>>    m_loopTask;          ///< 周期循环帧，活到析构
    };
} // namespace EdgePulse::System
