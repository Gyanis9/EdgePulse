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

        /// 一次条件 UPDATE 同时完成空行、自己续期与过期接手三种情形
        constexpr const char *kAcquireSql = "UPDATE sys_lease SET holder = ?, fence = fence + 1, expires_at = ?, updated_at = ? "
                                            "WHERE name = ? AND (holder = '' OR holder = ? OR expires_at IS NULL OR expires_at < ?)";
        /// 续约必须带 holder 与 fence，旧 leader 恢复后无法覆盖新 holder
        constexpr const char *kRenewSql = "UPDATE sys_lease SET expires_at = ?, updated_at = ? WHERE name = ? AND holder = ? AND fence = ?";
        /// 停止只清自己的 holder，不碰已经接手的新实例
        constexpr const char *kClearSql = "UPDATE sys_lease SET holder = '', expires_at = NULL, updated_at = ? WHERE name = ? AND holder = ?";

        /**
         * @brief 取当前时刻加 TTL 的 SQL DATETIME 文本
         * @param ttl 租约有效期
         * @return std::string 形如 2026-10-25 12:00:15
         */
        [[nodiscard]] std::string expiryText(const std::chrono::seconds ttl)
        {
            return Common::formatDateTimeUtc(std::chrono::system_clock::now() + ttl);
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
            co_return m_leader.load(std::memory_order_acquire);
        }
        m_loop.store(&loop, std::memory_order_release);
        if (m_stopped.load(std::memory_order_acquire))
        {
            co_return false;
        }

        const bool acquired = co_await acquireOnce(loop);
        // 获取期间收到停止请求：释放刚拿到的行，不再启动循环
        if (m_stopped.load(std::memory_order_acquire))
        {
            co_await releaseHolder(loop);
            co_return false;
        }

        m_loopExited.store(false, std::memory_order_release);
        m_loopTask.emplace(runLoop(loop));
        loop.scheduler().scheduleRemote(m_loopTask->handle());
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

        // 用独立直连立即清空自己的 holder，让故障转移不用等循环下一次醒来
        try
        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = Common::Database::openDirect(m_database->options());
            const std::array<AsynGyanis::Database::DatabaseValue, 3>        parameters{Common::nowDateTimeUtc(), m_name, m_instanceId};
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
        return m_leader.load(std::memory_order_acquire);
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
                if (m_leader.load(std::memory_order_acquire))
                {
                    static_cast<void>(co_await renewOnce(loop));
                } else
                {
                    static_cast<void>(co_await acquireOnce(loop));
                }
            } catch (const std::exception &failure)
            {
                markFollower();
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
        co_await releaseHolder(loop);
        markFollower();
        m_loopExited.store(true, std::memory_order_release);
        m_exitCondition.notify_all();
        co_return;
    }

    AsynGyanis::Core::Task<bool> LeaderLease::acquireOnce(AsynGyanis::Core::EventLoop &loop)
    {
        const std::string now        = Common::nowDateTimeUtc();
        const std::string expiresAt  = expiryText(m_ttl);
        const std::string instanceId = m_instanceId;
        const std::string name       = m_name;
        try
        {
            const std::optional<std::int64_t> acquiredFence = co_await m_database->withConnection<std::optional<std::int64_t>>(
                    loop,
                    [instanceId, name, now, expiresAt](AsynGyanis::Database::DatabaseConnection &connection) -> std::optional<std::int64_t>
                    {
                        const std::array<AsynGyanis::Database::DatabaseValue, 6> parameters{instanceId, expiresAt, now, name, instanceId, now};
                        const auto                                               update = Sql::executeOrThrow(connection, kAcquireSql, parameters);
                        if (update->affectedRowCount() <= 0)
                        {
                            return std::nullopt;
                        }
                        const std::array<AsynGyanis::Database::DatabaseValue, 2> selectParameters{name, instanceId};
                        const auto row = Sql::executeOrThrow(connection, "SELECT fence FROM sys_lease WHERE name = ? AND holder = ?", selectParameters);
                        if (!row->next())
                        {
                            return std::nullopt;
                        }
                        return Sql::integer(*row, "fence");
                    });
            if (!acquiredFence.has_value())
            {
                markFollower();
                co_return false;
            }
            if (m_stopped.load(std::memory_order_acquire))
            {
                co_await releaseHolder(loop);
                co_return false;
            }
            markLeader(*acquiredFence);
            LOG_INFO_FMT("获得控制面租约：instance={} fence={} expires={}", m_instanceId, *acquiredFence, expiresAt);
            co_return true;
        } catch (const std::exception &failure)
        {
            markFollower();
            LOG_WARN_FMT("获取控制面租约失败：instance={} error={}", m_instanceId, failure.what());
            co_return false;
        }
    }

    AsynGyanis::Core::Task<bool> LeaderLease::renewOnce(AsynGyanis::Core::EventLoop &loop)
    {
        const std::string  now          = Common::nowDateTimeUtc();
        const std::string  expiresAt    = expiryText(m_ttl);
        const std::string  instanceId   = m_instanceId;
        const std::string  name         = m_name;
        const std::int64_t currentFence = m_fence.load(std::memory_order_acquire);
        try
        {
            const bool renewed = co_await m_database->withConnection<bool>(
                    loop,
                    [instanceId, name, now, expiresAt, currentFence](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                    {
                        const std::array<AsynGyanis::Database::DatabaseValue, 5> parameters{expiresAt, now, name, instanceId, currentFence};
                        const auto                                               update = Sql::executeOrThrow(connection, kRenewSql, parameters);
                        return update->affectedRowCount() > 0;
                    });
            if (!renewed)
            {
                if (m_leader.exchange(false, std::memory_order_acq_rel))
                {
                    LOG_WARN_FMT("控制面租约已丢失：instance={} fence={}", m_instanceId, currentFence);
                }
                co_return false;
            }
            co_return !m_stopped.load(std::memory_order_acquire);
        } catch (const std::exception &failure)
        {
            markFollower();
            LOG_WARN_FMT("续约控制面租约失败：instance={} fence={} error={}", m_instanceId, currentFence, failure.what());
            co_return false;
        }
    }

    AsynGyanis::Core::Task<> LeaderLease::releaseHolder(AsynGyanis::Core::EventLoop &loop)
    {
        const std::string instanceId = m_instanceId;
        const std::string name       = m_name;
        const std::string now        = Common::nowDateTimeUtc();
        try
        {
            static_cast<void>(co_await m_database->withConnection<bool>(loop,
                                                                        [instanceId, name, now](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                                                                        {
                                                                            const std::array<AsynGyanis::Database::DatabaseValue, 3> parameters{now, name, instanceId};
                                                                            const auto update = Sql::executeOrThrow(connection, kClearSql, parameters);
                                                                            return update->affectedRowCount() > 0;
                                                                        }));
        } catch (const std::exception &failure)
        {
            LOG_WARN_FMT("清空控制面租约失败：instance={} error={}", m_instanceId, failure.what());
        }
        co_return;
    }

    void LeaderLease::markLeader(const std::int64_t fence) noexcept
    {
        // 先写 fence 再放开 leader，读侧看到 true 时 fence 一定已经就位
        m_fence.store(fence, std::memory_order_relaxed);
        m_leader.store(true, std::memory_order_release);
    }

    void LeaderLease::markFollower() noexcept
    {
        m_leader.store(false, std::memory_order_release);
    }
} // namespace EdgePulse::System
