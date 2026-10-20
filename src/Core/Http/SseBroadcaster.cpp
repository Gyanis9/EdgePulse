#include "Core/Http/SseBroadcaster.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Web
{
    std::uint64_t SseBroadcaster::subscribe()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::uint64_t         id = m_nextId++;
        m_subscribers[id]              = Subscriber{};
        return id;
    }

    void SseBroadcaster::unsubscribe(const std::uint64_t id)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscribers.erase(id);
    }

    void SseBroadcaster::publish(std::string event)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto &[id, subscriber]: m_subscribers)
        {
            // 慢客户端不拖垮其它订阅者：积压到上限后丢最旧的一条
            if (subscriber.pending.size() >= kMaximumPending)
            {
                subscriber.pending.erase(subscriber.pending.begin());
            }
            subscriber.pending.push_back(event);
        }
    }

    std::vector<std::string> SseBroadcaster::drain(const std::uint64_t id, const std::size_t maximum)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto                  iterator = m_subscribers.find(id);
        if (iterator == m_subscribers.end())
        {
            return {};
        }
        const std::size_t        count = std::min(maximum, iterator->second.pending.size());
        std::vector<std::string> events;
        events.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            events.push_back(std::move(iterator->second.pending[index]));
        }
        iterator->second.pending.erase(iterator->second.pending.begin(), iterator->second.pending.begin() + static_cast<std::ptrdiff_t>(count));
        return events;
    }

    std::size_t SseBroadcaster::subscriberCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_subscribers.size();
    }
} // namespace EdgePulse::Web
