#include "Auth/SessionStore.h"

#include "Common/RandomToken.h"

#include <chrono>
#include <iterator>
#include <mutex>
#include <utility>

namespace EdgePulse::Auth
{
    SessionStore::SessionStore(const int ttlSeconds, Clock clock) : m_ttlSeconds(ttlSeconds), m_clock(std::move(clock))
    {
    }

    std::chrono::steady_clock::time_point SessionStore::now() const
    {
        return m_clock ? m_clock() : std::chrono::steady_clock::now();
    }

    SessionRecord SessionStore::create(SessionRecord record)
    {
        std::lock_guard<std::mutex>                 lock(m_mutex);
        const std::chrono::steady_clock::time_point moment = now();

        // token 用 32 字节私有随机源；碰撞几乎不可能，仍循环到未占用为止
        do
        {
            record.token = Common::randomTokenHex(32);
        } while (m_sessions.contains(record.token));

        Entry entry;
        entry.record    = record;
        entry.expiresAt = moment + std::chrono::seconds(m_ttlSeconds);
        m_sessions.emplace(record.token, std::move(entry));

        // 惰性清理：只在表明显变大时扫一遍，避免每次创建都 O(n)
        if (m_sessions.size() > 1024)
        {
            for (auto iterator = m_sessions.begin(); iterator != m_sessions.end();)
            {
                iterator = iterator->second.expiresAt <= moment ? m_sessions.erase(iterator) : std::next(iterator);
            }
        }
        return record;
    }

    std::optional<SessionRecord> SessionStore::validateAndTouch(const std::string_view token)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto                  iterator = m_sessions.find(std::string{token});
        if (iterator == m_sessions.end())
        {
            return std::nullopt;
        }
        const std::chrono::steady_clock::time_point moment = now();
        if (iterator->second.expiresAt <= moment)
        {
            // 过期项当场删掉，不能让它以“已过期但仍可续”的状态继续存在
            m_sessions.erase(iterator);
            return std::nullopt;
        }
        iterator->second.expiresAt = moment + std::chrono::seconds(m_ttlSeconds);
        return iterator->second.record;
    }

    void SessionStore::remove(const std::string_view token)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions.erase(std::string{token});
    }

    int SessionStore::ttlSeconds() const noexcept
    {
        return m_ttlSeconds;
    }

    std::size_t SessionStore::size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sessions.size();
    }
} // namespace EdgePulse::Auth
