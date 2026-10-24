#include "Executors/RestartExecutor.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"

#include <exception>
#include <string>
#include <utility>

namespace EdgePulse::Agent
{
    RestartExecutor::RestartExecutor(RestartCallback restartCallback) : m_restartCallback(std::move(restartCallback))
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

        if (!m_restartCallback)
        {
            co_return CommandResult{false, "{}", "未配置重启回调，无法执行 agent.restart"};
        }

        try
        {
            m_restartCallback();
        } catch (const std::exception &failure)
        {
            co_return CommandResult{false, "{}", "触发重启失败：" + std::string{failure.what()}};
        } catch (...)
        {
            co_return CommandResult{false, "{}", "触发重启失败：未知异常"};
        }

        const AsynGyanis::Base::ConfigValue result = AsynGyanis::Base::ConfigValue::object({{"status", "restarting"}});
        co_return CommandResult{true, result.dump(), "重启请求已受理"};
    }
} // namespace EdgePulse::Agent
