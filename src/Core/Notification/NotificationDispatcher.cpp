#include "Core/Notification/NotificationDispatcher.h"

#include "Base/Log/LogMacros.h"
#include "Core/Notification/NotificationService.h"
#include "Core/Telemetry/TelemetryModels.h"

#include <Core/EventLoop/EventLoop.h>

#include <exception>
#include <utility>

namespace EdgePulse::Notification
{
    NotificationDispatcher::NotificationDispatcher(AsynGyanis::Core::EventLoop &loop, std::shared_ptr<NotificationService> service) : m_loop(loop), m_service(std::move(service))
    {
    }

    void NotificationDispatcher::enqueue(const Alarm::AlarmRecord &alarm)
    {
        if (m_service == nullptr)
        {
            return;
        }
        m_pending.push_back(alarm);
        if (m_running)
        {
            return;
        }
        m_running = true;
        m_worker  = drain();
        m_loop.scheduler().schedule(m_worker->handle());
    }

    AsynGyanis::Core::Task<> NotificationDispatcher::drain()
    {
        while (!m_pending.empty())
        {
            const Alarm::AlarmRecord alarm = std::move(m_pending.front());
            m_pending.pop_front();
            try
            {
                co_await m_service->dispatch(m_loop, alarm);
            } catch (const std::exception &failure)
            {
                LOG_WARN_FMT("告警通知投递异常：alarm={} error={}", alarm.id, failure.what());
            }
        }
        m_running = false;
        co_return;
    }
} // namespace EdgePulse::Notification
