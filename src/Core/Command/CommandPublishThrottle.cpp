#include "Core/Command/CommandPublishThrottle.h"

#include <utility>

namespace EdgePulse::Command
{
    CommandPublishThrottle::CommandPublishThrottle(std::chrono::seconds interval, NowFunction now) :
        m_interval(interval.count() > 0 ? interval : kDefaultInterval), m_now(std::move(now))
    {
    }

    bool CommandPublishThrottle::shouldPublish(const std::int64_t commandId)
    {
        const auto current = now();
        const auto found   = m_lastPublished.find(commandId);
        if (found != m_lastPublished.end() && current - found->second < m_interval)
        {
            return false;
        }
        m_lastPublished[commandId] = current;
        return true;
    }

    void CommandPublishThrottle::prune(const std::unordered_set<std::int64_t> &dueCommandIds)
    {
        for (auto iterator = m_lastPublished.begin(); iterator != m_lastPublished.end();)
        {
            if (dueCommandIds.contains(iterator->first))
            {
                ++iterator;
            } else
            {
                iterator = m_lastPublished.erase(iterator);
            }
        }
    }

    std::size_t CommandPublishThrottle::trackedCount() const noexcept
    {
        return m_lastPublished.size();
    }

    std::chrono::steady_clock::time_point CommandPublishThrottle::now() const
    {
        return m_now ? m_now() : std::chrono::steady_clock::now();
    }
} // namespace EdgePulse::Command
