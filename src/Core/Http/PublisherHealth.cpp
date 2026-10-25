#include "Core/Http/PublisherHealth.h"

namespace EdgePulse::Web
{
    bool PublisherHealth::isHealthy() const noexcept
    {
        return m_healthy.load(std::memory_order_acquire);
    }

    void PublisherHealth::markHealthy() noexcept
    {
        m_healthy.store(true, std::memory_order_release);
    }

    void PublisherHealth::markUnhealthy() noexcept
    {
        m_healthy.store(false, std::memory_order_release);
    }

    bool PublisherHealth::probe(const Probe &probe)
    {
        if (isHealthy())
        {
            return true;
        }
        const bool recovered = static_cast<bool>(probe) && probe();
        if (recovered)
        {
            markHealthy();
        } else
        {
            markUnhealthy();
        }
        return recovered;
    }
} // namespace EdgePulse::Web
