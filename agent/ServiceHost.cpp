#include "ServiceHost.h"

#include "AgentApplication.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace EdgePulse::Agent
{
    namespace
    {
        // 停止与启动阶段向 SCM 承诺的等待上限为 30 秒
        constexpr DWORD kServiceWaitHintMilliseconds{30000};
        // 停止期间每秒递增一次检查点，工作线程每秒被唤醒五次用于判断是否已退出
        constexpr auto kStopReportInterval = std::chrono::seconds{1};
        constexpr auto kWorkerPollInterval = std::chrono::milliseconds{200};

        wchar_t kServiceName[] = L"EdgePulseAgent";

        SERVICE_STATUS_HANDLE           g_statusHandle{nullptr};
        SERVICE_STATUS                  g_serviceStatus{};
        std::mutex                      g_statusMutex;
        std::atomic<AgentApplication *> g_application{nullptr};
        std::atomic<bool>               g_stopRequested{false};
        const AgentConfiguration       *g_configuration{nullptr};
        int                             g_serviceExitCode{0};

        /**
         * @brief 向 SCM 上报服务状态
         * @param currentState 目标状态
         * @param exitCode Win32 退出码
         * @param serviceSpecificExitCode 服务专属退出码，仅在 ERROR_SERVICE_SPECIFIC_ERROR 时有意义
         * @param checkpoint 检查点，停止阶段必须递增
         * @param waitHint 预计等待时间，单位毫秒
         */
        void reportStatus(const DWORD currentState, const DWORD exitCode = NO_ERROR, const DWORD serviceSpecificExitCode = 0, const DWORD checkpoint = 0, const DWORD waitHint = 0)
        {
            // 控制处理函数与工作线程都可能上报，SetServiceStatus 调用串行化以免状态块互相覆盖
            const std::lock_guard lock(g_statusMutex);
            g_serviceStatus.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
            g_serviceStatus.dwCurrentState     = currentState;
            g_serviceStatus.dwControlsAccepted = (currentState == SERVICE_RUNNING || currentState == SERVICE_STOP_PENDING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
            g_serviceStatus.dwWin32ExitCode    = exitCode;
            g_serviceStatus.dwServiceSpecificExitCode = serviceSpecificExitCode;
            g_serviceStatus.dwCheckPoint              = checkpoint;
            g_serviceStatus.dwWaitHint                = waitHint;
            if (g_statusHandle != nullptr)
            {
                static_cast<void>(SetServiceStatus(g_statusHandle, &g_serviceStatus));
            }
        }

        /**
         * @brief 处理 SCM 停止与关机控制
         * @param control 控制码
         * @return DWORD 处理结果
         */
        DWORD WINAPI controlHandler(const DWORD control, DWORD, void *, void *)
        {
            switch (control)
            {
                case SERVICE_CONTROL_STOP:
                case SERVICE_CONTROL_SHUTDOWN:
                    // 先记下停止请求：服务可能还在启动阶段，应用对象尚未就绪
                    g_stopRequested.store(true, std::memory_order_release);
                    reportStatus(SERVICE_STOP_PENDING, NO_ERROR, 0, 1, kServiceWaitHintMilliseconds);
                    if (auto *application = g_application.load(std::memory_order_acquire); application != nullptr)
                    {
                        application->requestStop();
                    }
                    return NO_ERROR;
                case SERVICE_CONTROL_INTERROGATE:
                    // 状态已经由 reportStatus 维护，直接确认即可
                    return NO_ERROR;
                default:
                    return ERROR_CALL_NOT_IMPLEMENTED;
            }
        }

        /**
         * @brief 服务入口：装配应用、等待工作线程退出并上报最终状态
         */
        void WINAPI serviceMain(DWORD, LPWSTR *)
        {
            g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, controlHandler, nullptr);
            if (g_statusHandle == nullptr)
            {
                g_serviceExitCode = 1;
                return;
            }
            reportStatus(SERVICE_START_PENDING, NO_ERROR, 0, 1, kServiceWaitHintMilliseconds);

            if (g_configuration == nullptr)
            {
                // 配置指针由 run() 在调度器启动前写入，为空说明调用顺序被破坏
                g_serviceExitCode = 1;
                reportStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, static_cast<DWORD>(g_serviceExitCode));
                return;
            }

            AgentApplication application(*g_configuration);
            g_application.store(&application, std::memory_order_release);
            // 启动阶段到达的停止请求不会丢：应用对象就绪后补发一次
            if (g_stopRequested.load(std::memory_order_acquire))
            {
                application.requestStop();
            } else
            {
                reportStatus(SERVICE_RUNNING);
            }

            std::atomic<int>  result{0};
            std::atomic<bool> finished{false};
            std::thread       worker(
                    [&application, &result, &finished]
                    {
                        result.store(application.run(), std::memory_order_release);
                        finished.store(true, std::memory_order_release);
                    });

            // 控制处理函数可能已经以检查点 1 上报过 STOP_PENDING，主循环从 2 起继续递增
            DWORD checkpoint  = 1;
            bool  stopPending = false;
            auto  lastReport  = std::chrono::steady_clock::now();
            while (!finished.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(kWorkerPollInterval);
                if (!g_stopRequested.load(std::memory_order_acquire))
                {
                    continue;
                }
                const auto now = std::chrono::steady_clock::now();
                // 停止可能持续数秒：按固定节奏递增检查点并给出等待提示，避免 SCM 判定服务挂起
                if (!stopPending || now - lastReport >= kStopReportInterval)
                {
                    ++checkpoint;
                    reportStatus(SERVICE_STOP_PENDING, NO_ERROR, 0, checkpoint, kServiceWaitHintMilliseconds);
                    stopPending = true;
                    lastReport  = now;
                }
            }
            worker.join();
            g_application.store(nullptr, std::memory_order_release);

            const int exitCode = result.load(std::memory_order_acquire);
            g_serviceExitCode  = exitCode;
            if (exitCode != 0)
            {
                // 失败用服务专属退出码上报，运维可在 sc queryex 中看到具体值
                reportStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, static_cast<DWORD>(exitCode));
            } else
            {
                reportStatus(SERVICE_STOPPED);
            }
        }
    } // namespace

    int ServiceHost::run(const AgentConfiguration &configuration)
    {
        g_configuration = &configuration;

        SERVICE_TABLE_ENTRYW serviceTable[] = {{kServiceName, serviceMain}, {nullptr, nullptr}};
        if (StartServiceCtrlDispatcherW(serviceTable) == FALSE)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            {
                // 在控制台直接运行 --service 时调度器必然返回该错误，按用法提示而不是崩溃
                std::cerr << "--service 必须在 SCM 中运行：请用 install-agent.ps1 安装为 Windows 服务，或改用 --console 前台运行\n";
                return 1;
            }
            std::cerr << "启动服务控制调度器失败：Windows 错误 " << error << "，请确认进程由服务控制管理器启动且服务已正确注册\n";
            return 1;
        }
        return g_serviceExitCode;
    }
} // namespace EdgePulse::Agent
