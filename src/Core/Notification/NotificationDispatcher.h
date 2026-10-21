/**
 * @file NotificationDispatcher.h
 * @brief 告警通知的异步排队与串行投递
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <Core/Coroutine/Task.h>

#include "Core/Telemetry/TelemetryModels.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Notification
{
    class NotificationService;

    /**
     * @brief 通知投递队列
     * @details 告警入库路径只入队，真正发 HTTP/SMTP 的协程在本对象里串行跑，
     *          避免 Webhook 慢响应拖住遥测处理。
     * @note 只在服务器所属事件循环上调用
     */
    class NotificationDispatcher
    {
    public:
        /**
         * @brief 构造
         * @param loop 服务器循环
         * @param service 投递服务
         */
        NotificationDispatcher(AsynGyanis::Core::EventLoop &loop, std::shared_ptr<NotificationService> service);

        /**
         * @brief 入队一条告警；已在投递时不另起协程
         * @param alarm 告警
         */
        void enqueue(const Alarm::AlarmRecord &alarm);

    private:
        /**
         * @brief 依次投递队列中的告警
         */
        AsynGyanis::Core::Task<> drain();

        AsynGyanis::Core::EventLoop            &m_loop;           ///< 服务器循环
        std::shared_ptr<NotificationService>    m_service;        ///< 投递服务
        std::deque<Alarm::AlarmRecord>          m_pending;        ///< 待投递告警
        std::optional<AsynGyanis::Core::Task<>> m_worker;         ///< 投递协程帧
        bool                                    m_running{false}; ///< 是否已有投递协程
    };
} // namespace EdgePulse::Notification
