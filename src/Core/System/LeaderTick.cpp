#include "Core/System/LeaderTick.h"

#include "Core/System/LeaderGate.h"

#include <stdexcept>
#include <utility>

namespace EdgePulse::System
{
    LeaderTick::LeaderTick(LeaderGate &gate, Work leaderWork, Work alwaysWork) : m_gate(gate), m_leaderWork(std::move(leaderWork)), m_alwaysWork(std::move(alwaysWork))
    {
        if (!m_leaderWork)
        {
            throw std::invalid_argument("单飞周期任务的 leader 工作体不能为空");
        }
    }

    AsynGyanis::Core::Task<> LeaderTick::run()
    {
        if (m_gate.allowsWork())
        {
            co_await m_leaderWork();
        }
        if (m_alwaysWork)
        {
            co_await m_alwaysWork();
        }
        co_return;
    }
} // namespace EdgePulse::System
