/**
 * @file NotificationService.h
 * @brief 告警 Webhook 与邮件投递
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Notification/NotificationRepository.h"

#include <Core/Coroutine/Task.h>

#include <memory>

namespace AsynGyanis::Core
{
    class EventLoop;
}
namespace EdgePulse::Alarm
{
    struct AlarmRecord;
}
namespace EdgePulse::Common
{
    class OutboundUrlGuard;
}

namespace EdgePulse::Notification
{
    /**
     * @brief 单次渠道投递结果
     */
    struct DeliveryOutcome
    {
        bool        ok{false};   ///< 是否送达
        int         attempts{0}; ///< 尝试次数
        std::string error;       ///< 失败原因
    };

    /**
     * @brief 通知投递服务
     * @note 只在服务器所属事件循环上调用
     */
    class NotificationService
    {
    public:
        /**
         * @brief 构造
         * @param repository 通知渠道仓储
         * @param guard 出站地址安全校验器，不能为空；投递 Webhook 前用它拒绝私网与元数据地址
         * @throws Base::InvalidArgumentException guard 为空：缺少出站校验时拒绝构造，避免 Webhook 绕过 SSRF 防护
         */
        explicit NotificationService(std::shared_ptr<NotificationRepository> repository, std::shared_ptr<Common::OutboundUrlGuard> guard);

        /**
         * @brief 按告警租户投递到所有启用渠道
         * @param loop 恢复协程用的事件循环
         * @param alarm 告警
         * @return AsynGyanis::Core::Task<> 投递协程；单个渠道失败只记日志
         */
        AsynGyanis::Core::Task<> dispatch(AsynGyanis::Core::EventLoop &loop, const Alarm::AlarmRecord &alarm);

    private:
        /**
         * @brief 投递一条 Webhook，失败重试一次
         */
        AsynGyanis::Core::Task<DeliveryOutcome> sendWebhook(AsynGyanis::Core::EventLoop &loop, const ChannelRecord &channel, const Alarm::AlarmRecord &alarm, std::string payload);

        /**
         * @brief 投递一封告警邮件，失败重试一次
         */
        AsynGyanis::Core::Task<DeliveryOutcome> sendEmail(AsynGyanis::Core::EventLoop &loop, const ChannelRecord &channel, const Alarm::AlarmRecord &alarm);

        /**
         * @brief 写入投递记录；失败只记日志，不影响告警链路
         */
        AsynGyanis::Core::Task<> recordDelivery(AsynGyanis::Core::EventLoop &loop, const ChannelRecord &channel, const Alarm::AlarmRecord &alarm, const DeliveryOutcome &outcome);

        std::shared_ptr<NotificationRepository>   m_repository; ///< 渠道仓储
        std::shared_ptr<Common::OutboundUrlGuard> m_guard;      ///< Webhook 出站地址校验器
    };
} // namespace EdgePulse::Notification
