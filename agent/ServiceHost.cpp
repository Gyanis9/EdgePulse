#include "ServiceHost.h"

#include "AgentApplication.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
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

        std::wstring                      g_serviceName;              ///< 服务名宽字符副本，存活到进程结束
        const wchar_t                    *g_serviceNameText{nullptr}; ///< 传给 SCM 的服务名，指向 g_serviceName
        SERVICE_STATUS_HANDLE             g_statusHandle{nullptr};    ///< SCM 状态句柄
        SERVICE_STATUS                    g_serviceStatus{};          ///< 最近一次上报的状态块
        bool                              g_stopRequested{false};     ///< 是否已收到停止或关机控制
        std::mutex                        g_serviceMutex;             ///< 保护状态块、状态句柄与停止标记
        std::atomic<DWORD>                g_checkpoint{0};            ///< 只增不减的检查点
        std::shared_ptr<AgentApplication> g_application;              ///< 工作线程正在运行的应用，受 g_applicationMutex 保护
        std::mutex                        g_applicationMutex;         ///< 保护 g_application
        const AgentConfiguration         *g_configuration{nullptr};   ///< run() 传入的配置，调度器返回前一直有效
        int                               g_serviceExitCode{0};       ///< 服务专属退出码

        /**
         * @brief 把 UTF-8 服务名转换为 UTF-16
         * @param text UTF-8 文本
         * @return std::wstring 转换结果；输入为空或包含非法 UTF-8 时返回空串
         */
        [[nodiscard]] std::wstring toWide(const std::string &text)
        {
            if (text.empty())
            {
                return {};
            }
            const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (length <= 0)
            {
                return {};
            }
            std::wstring wide(static_cast<std::size_t>(length), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), length) != length)
            {
                return {};
            }
            return wide;
        }

        /**
         * @brief 取得下一个单调递增的检查点
         * @return DWORD 不小于 1 的检查点
         * @note 检查点跨启动与停止阶段只增不减，SCM 才认可状态推进。
         */
        DWORD nextCheckpoint()
        {
            return g_checkpoint.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        /**
         * @brief 向 SCM 上报服务状态
         * @param currentState 目标状态
         * @param exitCode Win32 退出码
         * @param serviceSpecificExitCode 服务专属退出码，仅在 ERROR_SERVICE_SPECIFIC_ERROR 时有意义
         * @param checkpoint 检查点，停止阶段必须递增
         * @param waitHint 预计等待时间，单位毫秒
         * @note 调用方必须持有 g_serviceMutex；停止标记与状态上报共用一把锁，
         *       启动收尾的 RUNNING 不会在停止请求之后才被上报。
         */
        void reportStatusLocked(const DWORD currentState, const DWORD exitCode = NO_ERROR, const DWORD serviceSpecificExitCode = 0, const DWORD checkpoint = 0,
                                const DWORD waitHint = 0)
        {
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
         * @details 先置停止标记并上报 STOP_PENDING，再复制应用共享指针请求停止。
         *          复制而非直接使用裸指针，避免应用对象已在 serviceMain 中释放。
         */
        DWORD WINAPI controlHandler(const DWORD control, DWORD, void *, void *)
        {
            switch (control)
            {
                case SERVICE_CONTROL_STOP:
                case SERVICE_CONTROL_SHUTDOWN:
                {
                    {
                        const std::lock_guard lock(g_serviceMutex);
                        // STOPPED 已上报后不再回退状态；此时进程即将退出，停止请求没有意义
                        if (g_serviceStatus.dwCurrentState == SERVICE_STOPPED)
                        {
                            return NO_ERROR;
                        }
                        g_stopRequested = true;
                        reportStatusLocked(SERVICE_STOP_PENDING, NO_ERROR, 0, nextCheckpoint(), kServiceWaitHintMilliseconds);
                    }
                    std::shared_ptr<AgentApplication> application;
                    {
                        const std::lock_guard lock(g_applicationMutex);
                        application = g_application;
                    }
                    if (application != nullptr)
                    {
                        application->requestStop();
                    }
                    return NO_ERROR;
                }
                case SERVICE_CONTROL_INTERROGATE:
                    // 状态已经由 reportStatusLocked 维护，直接确认即可
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
            {
                const std::lock_guard lock(g_serviceMutex);
                if (g_serviceNameText == nullptr)
                {
                    g_serviceExitCode = 1;
                    return;
                }
                g_statusHandle = RegisterServiceCtrlHandlerExW(g_serviceNameText, controlHandler, nullptr);
                if (g_statusHandle == nullptr)
                {
                    g_serviceExitCode = 1;
                    return;
                }
                reportStatusLocked(SERVICE_START_PENDING, NO_ERROR, 0, nextCheckpoint(), kServiceWaitHintMilliseconds);
            }

            if (g_configuration == nullptr)
            {
                // 配置指针由 run() 在调度器启动前写入，为空说明调用顺序被破坏
                g_serviceExitCode = 1;
                const std::lock_guard lock(g_serviceMutex);
                reportStatusLocked(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, static_cast<DWORD>(g_serviceExitCode));
                return;
            }

            const auto application = std::make_shared<AgentApplication>(*g_configuration);
            {
                const std::lock_guard lock(g_applicationMutex);
                g_application = application;
            }

            bool stopRequested = false;
            {
                // 停止标记与 RUNNING 在同一把锁内判定：停止先到就不上报 RUNNING
                const std::lock_guard lock(g_serviceMutex);
                stopRequested = g_stopRequested;
                if (!stopRequested)
                {
                    reportStatusLocked(SERVICE_RUNNING);
                }
            }
            if (stopRequested)
            {
                // 停止请求可能早于应用对象就绪：控制处理函数当时拿不到指针，这里补发一次
                application->requestStop();
            }

            std::atomic<int>  result{0};
            std::atomic<bool> finished{false};
            std::thread       worker(
                    [application, &result, &finished]
                    {
                        result.store(application->run(), std::memory_order_release);
                        finished.store(true, std::memory_order_release);
                    });

            bool stopPending = false;
            auto lastReport  = std::chrono::steady_clock::now();
            while (!finished.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(kWorkerPollInterval);
                const std::lock_guard lock(g_serviceMutex);
                if (!g_stopRequested)
                {
                    continue;
                }
                const auto now = std::chrono::steady_clock::now();
                // 停止可能持续数秒：按固定节奏递增检查点并给出等待提示，避免 SCM 判定服务挂起
                if (!stopPending || now - lastReport >= kStopReportInterval)
                {
                    reportStatusLocked(SERVICE_STOP_PENDING, NO_ERROR, 0, nextCheckpoint(), kServiceWaitHintMilliseconds);
                    stopPending = true;
                    lastReport  = now;
                }
            }
            worker.join();
            {
                // 复位与销毁都在锁内完成；控制处理函数已复制出去的共享指针会延长应用寿命
                const std::lock_guard lock(g_applicationMutex);
                g_application.reset();
            }

            const int exitCode = result.load(std::memory_order_acquire);
            g_serviceExitCode  = exitCode;
            {
                const std::lock_guard lock(g_serviceMutex);
                if (exitCode != 0)
                {
                    // 失败用服务专属退出码上报，运维可在 sc queryex 中看到具体值
                    reportStatusLocked(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, static_cast<DWORD>(exitCode));
                } else
                {
                    reportStatusLocked(SERVICE_STOPPED);
                }
            }
        }
    } // namespace

    int ServiceHost::run(const AgentConfiguration &configuration, std::string serviceName)
    {
        g_serviceName = toWide(serviceName);
        if (g_serviceName.empty())
        {
            std::cerr << "服务名无法转换为 UTF-16：服务名只允许 ASCII 字母、数字、点、下划线与连字符\n";
            return 1;
        }
        g_serviceNameText = g_serviceName.c_str();
        g_configuration   = &configuration;

        // 调度器表和 serviceMain 都引用 g_serviceName 的字符缓冲区；该对象在 run() 返回前一直有效
        SERVICE_TABLE_ENTRYW serviceTable[] = {{const_cast<LPWSTR>(g_serviceNameText), serviceMain}, {nullptr, nullptr}};
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
