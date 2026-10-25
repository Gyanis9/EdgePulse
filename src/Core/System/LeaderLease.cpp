#include "Core/System/LeaderLease.h"

#include "Base/Database/Database.h"
#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Log/LogMacros.h"
#include "Base/Runtime/TimeFormat.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseValue.h"

#include "Core/Coroutine/Scheduler.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/System/LeaseDeadline.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::System
{
    namespace
    {
        constexpr std::size_t kMaximumInstanceIdLength = 128; ///< 与 holder VARCHAR(128) 对齐
        constexpr std::size_t kMaximumLeaseNameLength  = 64;  ///< 与 name VARCHAR(64) 对齐

        /// 一次条件 UPDATE 完成空行与过期接手两种情形；不接受 holder=自己，
        /// 否则两个实例配了同一个 instanceId 时会同时把自己当成可续期的 holder
        constexpr const char *kAcquireSql = "UPDATE sys_lease SET holder = ?, fence = fence + 1, expires_at = ?, updated_at = ? "
                                            "WHERE name = ? AND (holder = '' OR expires_at IS NULL OR expires_at < ?)";
        /// 回读时再确认库内到期时间仍在未来，慢查询不能凭一次 UPDATE 命中就自认 leader
        constexpr const char *kReadBackSql = "SELECT fence FROM sys_lease WHERE name = ? AND holder = ? AND expires_at > ?";
        /// 续约必须带 holder、fence 与未过期：延迟到达的续约不能延长已经过期的行
        constexpr const char *kRenewSql = "UPDATE sys_lease SET expires_at = ?, updated_at = ? WHERE name = ? AND holder = ? AND fence = ? AND expires_at > ?";
        /// 停止只清自己 fence 的行，同 instanceId 的后来者已经接手时不会被误清
        constexpr const char *kClearSql = "UPDATE sys_lease SET holder = '', expires_at = NULL, updated_at = ? WHERE name = ? AND holder = ? AND fence = ?";

        /**
         * @brief 一次成功的获取或续约在数据库线程上算出的结果
         */
        struct LeaseGrant
        {
            std::int64_t fence{0};               ///< 行内 fence
            std::int64_t deadlineNanoseconds{0}; ///< 按授权时刻锚定的本地单调截止（steady_clock 纳秒）
        };

        /**
         * @brief 取 steady_clock 当前刻度（纳秒）
         * @return std::int64_t 单调时钟纳秒数
         */
        [[nodiscard]] std::int64_t steadyNanoseconds() noexcept
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        /**
         * @brief 按本地绝对截止算出剩余毫秒，供日志使用
         * @param deadlineNanoseconds 绝对单调截止（steady_clock 纳秒）
         * @return std::int64_t 剩余毫秒；截止已过返回 0
         */
        [[nodiscard]] std::int64_t remainingMilliseconds(const std::int64_t deadlineNanoseconds) noexcept
        {
            const auto remaining = std::chrono::nanoseconds{deadlineNanoseconds - steadyNanoseconds()};
            if (remaining <= std::chrono::nanoseconds::zero())
            {
                return 0;
            }
            return std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
        }

        /**
         * @brief 判断文本里是否含空白字符
         * @param text 待检查文本
         * @return true 至少一个字符是空白
         */
        [[nodiscard]] bool containsWhitespace(const std::string_view text)
        {
            return std::any_of(text.begin(), text.end(), [](const unsigned char character) { return std::isspace(character) != 0; });
        }
    } // namespace

    LeaderLease::LeaderLease(std::shared_ptr<Common::Database> database, std::string instanceId, std::string name, const std::chrono::seconds ttl,
                             const std::chrono::milliseconds renewInterval) :
        m_database(std::move(database)), m_instanceId(std::move(instanceId)), m_name(std::move(name)), m_ttl(ttl), m_renewInterval(renewInterval)
    {
        if (!m_database)
        {
            throw std::invalid_argument("控制面租约缺少数据库");
        }
        if (m_instanceId.empty() || m_instanceId.size() > kMaximumInstanceIdLength)
        {
            throw std::invalid_argument("实例标识长度必须在 1 到 128 之间");
        }
        if (containsWhitespace(m_instanceId))
        {
            throw std::invalid_argument("实例标识不能包含空白字符");
        }
        if (m_name.empty() || m_name.size() > kMaximumLeaseNameLength)
        {
            throw std::invalid_argument("租约名长度必须在 1 到 64 之间");
        }
        if (m_ttl <= std::chrono::seconds::zero())
        {
            throw std::invalid_argument("控制面租约的 TTL 必须是正数");
        }
        if (m_renewInterval <= std::chrono::milliseconds::zero())
        {
            throw std::invalid_argument("控制面租约的续约周期必须是正数");
        }
    }

    LeaderLease::~LeaderLease()
    {
        stop();
        // 循环仍在跑时先等它退出，避免从别的线程销毁仍挂在计时器队列里的协程帧；
        // 循环已经停止时不会再有人回调该帧，直接销毁，避免给每个正常停机都加满等待时长
        const AsynGyanis::Core::EventLoop *const loop        = m_loop.load(std::memory_order_acquire);
        const bool                               loopRunning = loop != nullptr && loop->isRunning();
        if (loopRunning && !m_loopExited.load(std::memory_order_acquire))
        {
            std::unique_lock lock(m_exitMutex);
            m_exitCondition.wait_for(lock, std::chrono::seconds{2}, [this] { return m_loopExited.load(std::memory_order_acquire); });
        }
        m_loopTask.reset();
    }

    AsynGyanis::Core::Task<bool> LeaderLease::start(AsynGyanis::Core::EventLoop &loop)
    {
        if (m_started.exchange(true, std::memory_order_acq_rel))
        {
            co_return isLeader();
        }
        m_loop.store(&loop, std::memory_order_release);
        if (m_stopped.load(std::memory_order_acquire))
        {
            co_return false;
        }

        const bool acquired = co_await acquireOnce(loop);
        // 获取期间收到停止请求：按当前 fence 释放刚拿到的行，不再启动循环。
        // acquireOnce 已在停止分支里用回读到的 fence 清过一次，这里兜住它返回成功后才置停止位的交错
        if (m_stopped.load(std::memory_order_acquire))
        {
            co_await releaseHolder(loop, m_fence.load(std::memory_order_acquire));
            markFollower();
            co_return false;
        }

        m_loopExited.store(false, std::memory_order_release);
        m_loopTask.emplace(runLoop(loop));
        try
        {
            loop.scheduler().schedule(m_loopTask->handle());
        } catch (const std::exception &failure)
        {
            // 调度被拒时收回半启动状态，不留下一个已置 leader 却不会再续约的租约
            m_loopTask.reset();
            {
                const std::lock_guard exitLock(m_exitMutex);
                m_loopExited.store(true, std::memory_order_release);
            }
            m_exitCondition.notify_all();
            markFollower();
            LOG_WARN_FMT("启动控制面租约循环失败：instance={} error={}", m_instanceId, failure.what());
            co_return false;
        }
        co_return acquired;
    }

    void LeaderLease::stop()
    {
        if (m_stopped.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        markFollower();
        if (!m_started.load(std::memory_order_acquire))
        {
            return;
        }

        // 用独立直连立即清空自己的 holder，让故障转移不用等循环下一次醒来；
        // 条件里带自身 fence，同 instanceId 的后来者已经接手时不会被这次停止清掉
        try
        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = Common::Database::openDirect(m_database->options());
            const std::array<AsynGyanis::Database::DatabaseValue, 4>        parameters{Common::nowDateTimeUtc(), m_name, m_instanceId, m_fence.load(std::memory_order_acquire)};
            const std::unique_ptr<AsynGyanis::Database::DatabaseResult>     result = connection->execute(kClearSql, parameters);
            if (!result)
            {
                LOG_WARN_FMT("停止时清空控制面租约失败：instance={} error={}", m_instanceId, connection->lastError());
            }
            connection->disconnect();
        } catch (const std::exception &failure)
        {
            LOG_WARN_FMT("停止时清空控制面租约失败：instance={} error={}", m_instanceId, failure.what());
        }
    }

    bool LeaderLease::isLeader() const noexcept
    {
        if (!m_leader.load(std::memory_order_acquire))
        {
            return false;
        }
        // markLeader 先写截止再放开 m_leader，看到 true 时这里读到的一定是配对值
        const std::int64_t deadline = m_leaderDeadline.load(std::memory_order_relaxed);
        return steadyNanoseconds() < deadline;
    }

    std::int64_t LeaderLease::fence() const noexcept
    {
        return m_fence.load(std::memory_order_relaxed);
    }

    const std::string &LeaderLease::instanceId() const noexcept
    {
        return m_instanceId;
    }

    AsynGyanis::Core::Task<> LeaderLease::runLoop(AsynGyanis::Core::EventLoop &loop)
    {
        AsynGyanis::Core::Timer timer(loop);
        while (!m_stopped.load(std::memory_order_acquire))
        {
            try
            {
                // 用带本地截止的判定选路径：截止已过就立即改走重试获取，不拿过期行续约
                if (isLeader())
                {
                    static_cast<void>(co_await renewOnce(loop));
                } else
                {
                    static_cast<void>(co_await acquireOnce(loop));
                }
            } catch (const std::exception &failure)
            {
                // 兜底只处理没有在单次操作里收口的异常；本地截止仍有效时不得凭一次异常降级
                if (!isLeader())
                {
                    markFollower();
                }
                LOG_WARN_FMT("控制面租约循环失败：instance={} error={}", m_instanceId, failure.what());
            }

            // 把一次续约周期切成可观察停止请求的小片，避免 stop 要等满 5 秒
            const auto deadline = std::chrono::steady_clock::now() + m_renewInterval;
            while (!m_stopped.load(std::memory_order_acquire))
            {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                {
                    break;
                }
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
                if (remaining <= std::chrono::milliseconds::zero())
                {
                    break;
                }
                co_await timer.waitFor(std::min(remaining, std::chrono::milliseconds{250}));
            }
        }

        // stop 已经清过；这里兜住直连清理失败或获取与停止交错的情形
        co_await releaseHolder(loop, m_fence.load(std::memory_order_acquire));
        markFollower();
        {
            // 与析构里的判据共用同一把锁：先落标志再通知，不会漏掉已经进入等待的析构
            const std::lock_guard exitLock(m_exitMutex);
            m_loopExited.store(true, std::memory_order_release);
        }
        m_exitCondition.notify_all();
        co_return;
    }

    AsynGyanis::Core::Task<bool> LeaderLease::acquireOnce(AsynGyanis::Core::EventLoop &loop)
    {
        const std::string instanceId = m_instanceId;
        const std::string name       = m_name;
        const auto        ttl        = m_ttl;
        try
        {
            const std::optional<LeaseGrant> grant = co_await m_database->withConnection<std::optional<LeaseGrant>>(
                    loop,
                    [instanceId, name, ttl](AsynGyanis::Database::DatabaseConnection &connection) -> std::optional<LeaseGrant>
                    {
                        // 时间在数据库线程上按执行时刻生成，排队等待连接或线程调度不会把到期时间写成过去
                        const auto                                               acquiredAt = std::chrono::system_clock::now();
                        const std::string                                        nowText    = Common::formatDateTimeUtc(acquiredAt);
                        const std::string                                        expiresAt  = Common::formatDateTimeUtc(acquiredAt + ttl);
                        const std::array<AsynGyanis::Database::DatabaseValue, 5> parameters{instanceId, expiresAt, nowText, name, nowText};
                        const auto                                               update = Sql::executeOrThrow(connection, kAcquireSql, parameters);
                        if (update->affectedRowCount() <= 0)
                        {
                            return std::nullopt;
                        }

                        // 回读要求库内到期时间仍在未来，只有 UPDATE 命中不足以承认 leader
                        const std::string                                        verifyAt = Common::formatDateTimeUtc(std::chrono::system_clock::now());
                        const std::array<AsynGyanis::Database::DatabaseValue, 3> selectParameters{name, instanceId, verifyAt};
                        const auto                                               row = Sql::executeOrThrow(connection, kReadBackSql, selectParameters);
                        if (!row->next())
                        {
                            return std::nullopt;
                        }
                        // 截止锚定在取到授权的同一段工作线程执行里：先取单调刻度再取墙上时间，
                        // 换算只会更早；协程此后多久才恢复都不再参与计算
                        const std::int64_t workerSteady = steadyNanoseconds();
                        const auto         workerWall   = std::chrono::system_clock::now();
                        const auto         deadline     = leaseDeadlineNanoseconds(workerSteady, workerWall, acquiredAt + ttl, ttl);
                        if (deadline <= workerSteady)
                        {
                            return std::nullopt;
                        }
                        return LeaseGrant{Sql::integer(*row, "fence"), deadline};
                    });
            if (!grant.has_value())
            {
                markFollower();
                co_return false;
            }
            if (m_stopped.load(std::memory_order_acquire))
            {
                // 停止请求可能早于 m_fence 更新，必须用本次回读到的 fence 清刚拿到的行
                co_await releaseHolder(loop, grant->fence);
                markFollower();
                co_return false;
            }
            markLeader(grant->fence, grant->deadlineNanoseconds);
            // 停止请求与这次获取交错时，stop 的清理可能早于 m_fence 更新；这里再按取得的 fence 收口
            if (m_stopped.load(std::memory_order_acquire))
            {
                co_await releaseHolder(loop, grant->fence);
                markFollower();
                co_return false;
            }
            LOG_INFO_FMT("获得控制面租约：instance={} fence={} 剩余有效期={}ms", m_instanceId, grant->fence, remainingMilliseconds(grant->deadlineNanoseconds));
            // 事件循环可能停摆到截止之后才恢复：此时授权行已过期，不能报告获取成功
            co_return isLeader();
        } catch (const std::exception &failure)
        {
            markFollower();
            LOG_WARN_FMT("获取控制面租约失败：instance={} error={}", m_instanceId, failure.what());
            co_return false;
        }
    }

    AsynGyanis::Core::Task<bool> LeaderLease::renewOnce(AsynGyanis::Core::EventLoop &loop)
    {
        const std::string  instanceId   = m_instanceId;
        const std::string  name         = m_name;
        const auto         ttl          = m_ttl;
        const std::int64_t currentFence = m_fence.load(std::memory_order_acquire);
        try
        {
            const std::optional<LeaseGrant> grant = co_await m_database->withConnection<std::optional<LeaseGrant>>(
                    loop,
                    [instanceId, name, ttl, currentFence](AsynGyanis::Database::DatabaseConnection &connection) -> std::optional<LeaseGrant>
                    {
                        // 时间同样在执行时刻生成；AND expires_at > ? 让延迟到达的续约无法延长已过期的行
                        const auto                                               renewedAt = std::chrono::system_clock::now();
                        const std::string                                        nowText   = Common::formatDateTimeUtc(renewedAt);
                        const std::string                                        expiresAt = Common::formatDateTimeUtc(renewedAt + ttl);
                        const std::array<AsynGyanis::Database::DatabaseValue, 6> parameters{expiresAt, nowText, name, instanceId, currentFence, nowText};
                        const auto                                               update = Sql::executeOrThrow(connection, kRenewSql, parameters);
                        if (update->affectedRowCount() <= 0)
                        {
                            return std::nullopt;
                        }
                        // 与获取同一条口径：续约结果落库后立刻在工作线程上锚定本地截止
                        const std::int64_t workerSteady = steadyNanoseconds();
                        const auto         workerWall   = std::chrono::system_clock::now();
                        const auto         deadline     = leaseDeadlineNanoseconds(workerSteady, workerWall, renewedAt + ttl, ttl);
                        if (deadline <= workerSteady)
                        {
                            return std::nullopt;
                        }
                        return LeaseGrant{currentFence, deadline};
                    });
            if (!grant.has_value())
            {
                if (m_leader.exchange(false, std::memory_order_acq_rel))
                {
                    LOG_WARN_FMT("控制面租约已丢失：instance={} fence={}", m_instanceId, currentFence);
                }
                co_return false;
            }
            if (m_stopped.load(std::memory_order_acquire))
            {
                markFollower();
                co_return false;
            }
            markLeader(grant->fence, grant->deadlineNanoseconds);
            // 停止请求可能落在前一次检查之后，markLeader 之后再确认一次，避免 stop 后又被抬成 leader
            if (m_stopped.load(std::memory_order_acquire))
            {
                markFollower();
                co_return false;
            }
            // 恢复晚于本地截止时续约结果已经作废，如实报告当前状态
            co_return isLeader();
        } catch (const std::exception &failure)
        {
            // 数据库短暂不可用不等于租约丢失：本地截止未到就保留最后已知的 leadership，下一轮重试
            if (isLeader())
            {
                LOG_WARN_FMT("续约控制面租约异常，本地截止前保持 leader：instance={} fence={} error={}", m_instanceId, currentFence, failure.what());
                co_return false;
            }
            markFollower();
            LOG_WARN_FMT("续约控制面租约失败：instance={} fence={} error={}", m_instanceId, currentFence, failure.what());
            co_return false;
        }
    }

    AsynGyanis::Core::Task<> LeaderLease::releaseHolder(AsynGyanis::Core::EventLoop &loop, const std::int64_t fence)
    {
        const std::string instanceId = m_instanceId;
        const std::string name       = m_name;
        const std::string now        = Common::nowDateTimeUtc();
        try
        {
            static_cast<void>(co_await m_database->withConnection<bool>(loop,
                                                                        [instanceId, name, now, fence](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                                                                        {
                                                                            const std::array<AsynGyanis::Database::DatabaseValue, 4> parameters{now, name, instanceId, fence};
                                                                            const auto update = Sql::executeOrThrow(connection, kClearSql, parameters);
                                                                            return update->affectedRowCount() > 0;
                                                                        }));
        } catch (const std::exception &failure)
        {
            LOG_WARN_FMT("清空控制面租约失败：instance={} fence={} error={}", m_instanceId, fence, failure.what());
        }
        co_return;
    }

    void LeaderLease::markLeader(const std::int64_t fence, const std::int64_t deadlineNanoseconds) noexcept
    {
        // 截止已由数据库工作线程锚定在取到授权的那一刻，这里只原样发布，
        // 不再叠加事件循环恢复时刻；先写 fence 与截止再放开 leader，读侧看到 true 时两者一定就位
        m_fence.store(fence, std::memory_order_relaxed);
        m_leaderDeadline.store(deadlineNanoseconds, std::memory_order_relaxed);
        m_leader.store(true, std::memory_order_release);
    }

    void LeaderLease::markFollower() noexcept
    {
        m_leader.store(false, std::memory_order_release);
    }
} // namespace EdgePulse::System
