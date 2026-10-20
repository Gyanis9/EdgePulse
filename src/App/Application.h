/**
 * @file Application.h
 * @brief 进程级装配、监听器与生命周期
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "App/ApplicationConfiguration.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <Core/Coroutine/Task.h>

namespace AsynGyanis::Core
{
    class IoContext;
}

namespace AsynGyanis::Net
{
    class HttpServer;
}

namespace EdgePulse::App
{
    /**
     * @brief 进程级装配：事件循环、监听器、协程生命周期与停止请求
     */
    class Application
    {
    public:
        /**
         * @brief 用已加载的配置构造应用（尚未绑定端口）
         * @param configuration 已校验的配置
         */
        explicit Application(ApplicationConfiguration configuration);

        ~Application();

        Application(const Application &)            = delete;
        Application &operator=(const Application &) = delete;

        /**
         * @brief 建立事件循环与 8080 监听器，并把接受协程投递到循环
         * @param error 失败原因
         * @return bool 是否启动成功
         * @note 必须在 run() 之前调用；监听器在此时尚未开始接受连接
         */
        bool start(std::string &error);

        /**
         * @brief 阻塞运行事件循环，直到 requestStop() 或进程收到停止信号
         * @return int 进程退出码
         */
        int run();

        /**
         * @brief 请求停止事件循环；可从非循环线程调用
         */
        void requestStop();

        /**
         * @brief 读回实际监听端口；端口 0 时用于获取内核分配的端口
         * @return std::uint16_t 端口，未启动时为 0
         */
        [[nodiscard]] std::uint16_t httpPort() const noexcept;

    private:
        ApplicationConfiguration                     m_configuration;
        std::unique_ptr<AsynGyanis::Core::IoContext> m_ioContext;
        std::unique_ptr<AsynGyanis::Net::HttpServer> m_httpServer;
        std::optional<AsynGyanis::Core::Task<>>      m_acceptTask;
        std::atomic<bool>                            m_stopRequested{false};
    };
} // namespace EdgePulse::App
