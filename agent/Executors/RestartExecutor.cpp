#include "Executors/RestartExecutor.h"

#include "Base/Config/ConfigValue.h"

namespace EdgePulse::Agent
{
    RestartExecutor::RestartExecutor(const bool restartConfigured) : m_restartConfigured(restartConfigured)
    {
    }

    bool RestartExecutor::supports(const std::string_view type) const
    {
        return type == "agent.restart";
    }

    AsynGyanis::Core::Task<CommandResult> RestartExecutor::execute(AsynGyanis::Core::EventLoop &loop, const AgentCommand &command)
    {
        static_cast<void>(loop);
        static_cast<void>(command);

        if (!m_restartConfigured)
        {
            co_return CommandResult{false, "{}", "未配置重启处理器，无法执行 agent.restart"};
        }

        const AsynGyanis::Base::ConfigValue result = AsynGyanis::Base::ConfigValue::object({{"status", "restarting"}});
        co_return CommandResult{true, result.dump(), "重启请求已受理，结果上报后重启服务", true};
    }
} // namespace EdgePulse::Agent
