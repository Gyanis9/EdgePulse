#include "Core/Cache/MemoryKeyValueStore.h"

#include <charconv>
#include <chrono>
#include <utility>

namespace EdgePulse::Cache
{
    MemoryKeyValueStore::MemoryKeyValueStore(Clock clock) : m_clock(std::move(clock))
    {
        if (!m_clock)
        {
            m_clock = [] { return std::chrono::steady_clock::now(); };
        }
    }

    AsynGyanis::Core::Task<std::optional<std::string>> MemoryKeyValueStore::get(AsynGyanis::Core::EventLoop &, std::string key)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto                  iterator = m_entries.find(key);
        if (iterator == m_entries.end())
        {
            co_return std::nullopt;
        }
        if (isExpired(iterator->second, now()))
        {
            m_entries.erase(iterator);
            co_return std::nullopt;
        }
        co_return iterator->second.value;
    }

    AsynGyanis::Core::Task<std::optional<std::string>> MemoryKeyValueStore::take(AsynGyanis::Core::EventLoop &, std::string key)
    {
        // 查找、判过期与 erase 必须在同一把锁内完成，否则两个并发消费方可能取到同一个值
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto                  iterator = m_entries.find(key);
        if (iterator == m_entries.end())
        {
            co_return std::nullopt;
        }
        if (isExpired(iterator->second, now()))
        {
            m_entries.erase(iterator);
            co_return std::nullopt;
        }
        // 先移出值再抹掉表项：erase 之后迭代器失效，旧值必须先由本地串接管
        std::string value = std::move(iterator->second.value);
        m_entries.erase(iterator);
        co_return value;
    }

    AsynGyanis::Core::Task<bool> MemoryKeyValueStore::set(AsynGyanis::Core::EventLoop &, std::string key, std::string value, const std::chrono::seconds ttl)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Entry                       entry;
        entry.value     = std::move(value);
        entry.expiresAt = ttl.count() > 0 ? std::optional{now() + ttl} : std::nullopt;
        m_entries.insert_or_assign(std::move(key), std::move(entry));
        co_return true;
    }

    AsynGyanis::Core::Task<bool> MemoryKeyValueStore::erase(AsynGyanis::Core::EventLoop &, std::string key)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        co_return m_entries.erase(key) > 0;
    }

    AsynGyanis::Core::Task<std::int64_t> MemoryKeyValueStore::increment(AsynGyanis::Core::EventLoop &, std::string key, const std::chrono::seconds ttl)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto                  moment   = now();
        const auto                  iterator = m_entries.find(key);
        const bool                  exists   = iterator != m_entries.end() && !isExpired(iterator->second, moment);
        std::int64_t                value    = 0;
        if (exists)
        {
            static_cast<void>(std::from_chars(iterator->second.value.data(), iterator->second.value.data() + iterator->second.value.size(), value));
        }
        value += 1;
        Entry entry;
        entry.value = std::to_string(value);
        // 已存在的计数沿用原窗口的截止时刻；新建时才从现在起算，避免每次自增都把窗口往后推
        entry.expiresAt = exists ? iterator->second.expiresAt : (ttl.count() > 0 ? std::optional{moment + ttl} : std::nullopt);
        m_entries.insert_or_assign(std::move(key), std::move(entry));
        co_return value;
    }

    std::string_view MemoryKeyValueStore::backendName() const noexcept
    {
        return "memory";
    }

    AsynGyanis::Core::Task<bool> MemoryKeyValueStore::probe(AsynGyanis::Core::EventLoop &)
    {
        // 进程内存储没有外部依赖，能调度到本协程即代表可用
        co_return true;
    }

    std::size_t MemoryKeyValueStore::size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.size();
    }

    std::chrono::steady_clock::time_point MemoryKeyValueStore::now() const
    {
        return m_clock();
    }

    bool MemoryKeyValueStore::isExpired(const Entry &entry, const std::chrono::steady_clock::time_point moment) const
    {
        return entry.expiresAt.has_value() && *entry.expiresAt <= moment;
    }
} // namespace EdgePulse::Cache
