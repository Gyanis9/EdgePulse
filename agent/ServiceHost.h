/**
 * @file ServiceHost.h
 * @brief Windows 服务控制管理器托管入口
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#ifdef _WIN32

#include "AgentConfiguration.h"

#include <string>

namespace EdgePulse::Agent
{
    /**
     * @brief 在 Windows 服务控制管理器下运行 Agent
     * @details 注册控制处理函数，把 AgentApplication 放到工作线程执行；停止与关机控制
     *          只请求应用停止，由工作线程完成收尾后再把服务状态置为 STOPPED。
     */
    class ServiceHost
    {
    public:
        /**
         * @brief 接管服务控制调度并运行至停止
         * @param configuration 已通过形态校验的配置
         * @param serviceName SCM 注册名，必须与安装时 sc.exe create 使用的名字一致
         * @return int 正常停止返回 0；调度器启动失败或 Agent 运行失败返回非零
         * @note 该入口只能在服务进程内由 SCM 调用；在控制台直接调用时
         *       StartServiceCtrlDispatcherW 失败，函数打印中文提示后返回 1。
         */
        static int run(const AgentConfiguration &configuration, std::string serviceName);
    };
} // namespace EdgePulse::Agent

#endif
