/**
 * @file AgentVersion.h
 * @brief Agent 运行版本的唯一常量
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <string_view>

#ifndef EDGEPULSE_AGENT_VERSION
#define EDGEPULSE_AGENT_VERSION "unknown"
#endif

namespace EdgePulse::Agent
{
    /// 当前进程的 Agent 版本，由构建系统注入
    inline constexpr std::string_view kAgentVersion{EDGEPULSE_AGENT_VERSION};
} // namespace EdgePulse::Agent
