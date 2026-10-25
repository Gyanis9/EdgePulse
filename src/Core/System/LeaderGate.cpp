#include "Core/System/LeaderGate.h"

#include "Base/Log/LogMacros.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace EdgePulse::System
{
    LeaderGate::LeaderGate(std::string taskName, std::string instanceId, std::function<bool()> isLeader, std::function<std::int64_t()> fence) :
        m_taskName(std::move(taskName)), m_instanceId(std::move(instanceId)), m_isLeader(std::move(isLeader)), m_fence(std::move(fence))
    {
        if (!m_isLeader)
        {
            throw std::invalid_argument("周期任务的 leader 判定回调不能为空");
        }
    }

    bool LeaderGate::allowsWork()
    {
        const bool leader = m_isLeader();
        if (!m_lastLeader.has_value())
        {
            // 首拍不打印切换：启动时的站位由租约自身的获取日志说明
            m_lastLeader = leader;
            return leader;
        }
        if (*m_lastLeader == leader)
        {
            return leader;
        }
        m_lastLeader = leader;
        if (leader)
        {
            LOG_INFO_FMT("{}：本实例成为 leader（{}），恢复执行", m_taskName, describeContext());
        } else
        {
            LOG_WARN_FMT("{}：本实例不再是 leader（{}），暂停执行直到重新当选", m_taskName, describeContext());
        }
        return leader;
    }

    const std::string &LeaderGate::taskName() const noexcept
    {
        return m_taskName;
    }

    std::string LeaderGate::describeContext() const
    {
        std::string context;
        if (!m_instanceId.empty())
        {
            context = "instance=" + m_instanceId;
        }
        if (m_fence)
        {
            if (!context.empty())
            {
                context += " ";
            }
            context += "fence=" + std::to_string(m_fence());
        }
        return context.empty() ? "instance=-" : context;
    }
} // namespace EdgePulse::System
