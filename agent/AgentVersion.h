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

#if defined(_WIN32)
#if defined(_M_ARM64)
    /// 当前进程运行的操作系统与架构，与安装包 manifest 的 platform 字段同口径
    inline constexpr std::string_view kAgentPlatform{"windows-aarch64"};
#else
    /// 当前进程运行的操作系统与架构，与安装包 manifest 的 platform 字段同口径
    inline constexpr std::string_view kAgentPlatform{"windows-x86_64"};
#endif
#else
#if defined(__aarch64__)
    /// 当前进程运行的操作系统与架构，与安装包 manifest 的 platform 字段同口径
    inline constexpr std::string_view kAgentPlatform{"linux-aarch64"};
#else
    /// 当前进程运行的操作系统与架构，与安装包 manifest 的 platform 字段同口径
    inline constexpr std::string_view kAgentPlatform{"linux-x86_64"};
#endif
#endif
} // namespace EdgePulse::Agent
