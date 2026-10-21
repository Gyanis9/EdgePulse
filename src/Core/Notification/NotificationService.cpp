#include "Core/Notification/NotificationService.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Base/Runtime/TimeFormat.h"
#include "Base/Security/Hmac.h"
#include "Core/Notification/SmtpMailer.h"
#include "Core/Telemetry/TelemetryModels.h"
#include "Net/Http/Client/HttpClient.h"

#include <Core/EventLoop/EventLoop.h>
#include <Core/EventLoop/Timer.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::Notification
{
    namespace
    {
        constexpr std::chrono::milliseconds kWebhookTimeout{5000};
        constexpr std::chrono::milliseconds kRetryDelay{300};

        [[nodiscard]] int levelRank(const std::string_view level)
        {
            if (level == "critical")
            {
                return 3;
            }
            if (level == "warning")
            {
                return 2;
            }
            return 1;
        }

        [[nodiscard]] std::vector<std::string> splitRecipients(const std::string &text)
        {
            std::vector<std::string> recipients;
            std::size_t              begin = 0;
            while (begin <= text.size())
            {
                const std::size_t comma = text.find(',', begin);
                std::string       entry = text.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
                entry.erase(0, entry.find_first_not_of(" \t"));
                entry.erase(entry.find_last_not_of(" \t") + 1);
                if (!entry.empty())
                {
                    recipients.push_back(std::move(entry));
                }
                if (comma == std::string::npos)
                {
                    break;
                }
                begin = comma + 1;
            }
            return recipients;
        }

        [[nodiscard]] std::string alarmPayload(const Alarm::AlarmRecord &alarm)
        {
            return AsynGyanis::Base::ConfigValue::object({{"type", "alarm"},
                                                          {"tenantId", alarm.tenantId},
                                                          {"data", AsynGyanis::Base::ConfigValue::object({{"id", alarm.id},
                                                                                                          {"nodeId", alarm.nodeId},
                                                                                                          {"nodeName", alarm.nodeName},
                                                                                                          {"level", alarm.level},
                                                                                                          {"metric", alarm.metric},
                                                                                                          {"value", alarm.value},
                                                                                                          {"threshold", alarm.threshold},
                                                                                                          {"status", alarm.status},
                                                                                                          {"message", alarm.message},
                                                                                                          {"triggeredAt", alarm.triggeredAt}})}})
                    .dump();
        }

        [[nodiscard]] std::string alarmMailBody(const Alarm::AlarmRecord &alarm)
        {
            return "节点：" + (alarm.nodeName.empty() ? std::to_string(alarm.nodeId) : alarm.nodeName) + "\n级别：" + alarm.level + "\n指标：" + alarm.metric + "\n触发值：" +
                   std::to_string(alarm.value) + "\n阈值：" + std::to_string(alarm.threshold) + "\n状态：" + alarm.status + "\n时间：" + alarm.triggeredAt + "\n说明：" +
                   alarm.message + "\n";
        }
    } // namespace

    NotificationService::NotificationService(std::shared_ptr<NotificationRepository> repository) : m_repository(std::move(repository))
    {
    }

    AsynGyanis::Core::Task<> NotificationService::dispatch(AsynGyanis::Core::EventLoop &loop, const Alarm::AlarmRecord &alarm)
    {
        if (m_repository == nullptr || alarm.tenantId <= 0)
        {
            co_return;
        }
        std::vector<ChannelRecord> channels;
        try
        {
            channels = co_await m_repository->scoped({alarm.tenantId, false})->enabledChannels(loop);
        } catch (const std::exception &failure)
        {
            LOG_WARN_FMT("通知渠道读取失败：tenant={} error={}", alarm.tenantId, failure.what());
            co_return;
        }
        for (const ChannelRecord &channel: channels)
        {
            if (levelRank(alarm.level) < levelRank(channel.minLevel))
            {
                continue;
            }
            DeliveryOutcome outcome;
            if (channel.type == "webhook")
            {
                outcome = co_await sendWebhook(loop, channel, alarm, alarmPayload(alarm));
            } else if (channel.type == "email")
            {
                outcome = co_await sendEmail(loop, channel, alarm);
            } else
            {
                continue;
            }
            co_await recordDelivery(loop, channel, alarm, outcome);
        }
        co_return;
    }

    AsynGyanis::Core::Task<DeliveryOutcome> NotificationService::sendWebhook(AsynGyanis::Core::EventLoop &loop, const ChannelRecord &channel, const Alarm::AlarmRecord &alarm,
                                                                             std::string payload)
    {
        DeliveryOutcome outcome;
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            outcome.attempts = attempt + 1;
            if (attempt > 0)
            {
                AsynGyanis::Core::Timer pause(loop);
                co_await pause.waitFor(kRetryDelay);
            }
            AsynGyanis::Net::HttpClientRequest request;
            request.method      = "POST";
            request.contentType = "application/json";
            request.body        = payload;
            request.headers.emplace_back("Accept", "application/json");
            if (!channel.secret.empty())
            {
                request.headers.emplace_back("X-EdgePulse-Signature", "sha256=" + Common::hmacSha256Hex(channel.secret, payload));
            }
            const auto response = co_await AsynGyanis::Net::HttpClient::send(loop, channel.endpoint, request, kWebhookTimeout);
            if (response && response->statusCode >= 200 && response->statusCode < 300)
            {
                outcome.ok = true;
                co_return outcome;
            }
            outcome.error = response ? "HTTP " + std::to_string(response->statusCode) : "网络不可用";
            LOG_WARN_FMT("告警 Webhook 投递失败：channel={} tenant={} node={} attempt={} status={}", channel.name, alarm.tenantId, alarm.nodeId, attempt + 1,
                         response ? response->statusCode : 0);
        }
        co_return outcome;
    }

    AsynGyanis::Core::Task<DeliveryOutcome> NotificationService::sendEmail(AsynGyanis::Core::EventLoop &loop, const ChannelRecord &channel, const Alarm::AlarmRecord &alarm)
    {
        DeliveryOutcome outcome;
        SmtpSettings    settings;
        settings.endpoint         = channel.endpoint;
        settings.username         = channel.username;
        settings.password         = channel.secret;
        settings.sender           = channel.sender;
        settings.caFile           = channel.caFile;
        settings.recipients       = splitRecipients(channel.recipients);
        const std::string subject = "[EdgePulse][" + alarm.level + "] " + (alarm.message.empty() ? alarm.metric : alarm.message);
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            outcome.attempts = attempt + 1;
            if (attempt > 0)
            {
                AsynGyanis::Core::Timer pause(loop);
                co_await pause.waitFor(kRetryDelay);
            }
            std::string error;
            try
            {
                if (co_await SmtpMailer::send(loop, settings, subject, alarmMailBody(alarm), error))
                {
                    outcome.ok = true;
                    co_return outcome;
                }
                outcome.error = error;
            } catch (const std::exception &failure)
            {
                error = failure.what();
            }
            LOG_WARN_FMT("告警邮件投递失败：channel={} tenant={} node={} attempt={} error={}", channel.name, alarm.tenantId, alarm.nodeId, attempt + 1, error);
        }
        co_return outcome;
    }

    AsynGyanis::Core::Task<> NotificationService::recordDelivery(AsynGyanis::Core::EventLoop &loop, const ChannelRecord &channel, const Alarm::AlarmRecord &alarm,
                                                                 const DeliveryOutcome &outcome)
    {
        DeliveryRecord record;
        record.tenantId    = alarm.tenantId;
        record.channelId   = channel.id;
        record.channelName = channel.name;
        record.channelType = channel.type;
        record.alarmId     = alarm.id;
        record.alarmLevel  = alarm.level;
        record.status      = outcome.ok ? "succeeded" : "failed";
        record.attempts    = outcome.attempts;
        record.error       = outcome.error.size() > 500 ? outcome.error.substr(0, 500) : outcome.error;
        record.createdAt   = Common::nowDateTimeUtc();
        if (channel.type == "email")
        {
            record.target = "email:" + std::to_string(splitRecipients(channel.recipients).size()) + " recipients";
        } else
        {
            const std::size_t scheme = channel.endpoint.find("://");
            std::string       host   = scheme == std::string::npos ? channel.endpoint : channel.endpoint.substr(scheme + 3);
            const std::size_t slash  = host.find('/');
            if (slash != std::string::npos)
            {
                host.resize(slash);
            }
            // 目标只保留主机，避免把 URL 中的凭据写进投递记录
            if (const std::size_t at = host.rfind('@'); at != std::string::npos)
            {
                host.erase(0, at + 1);
            }
            if (host.size() > 140)
            {
                host.resize(140);
            }
            record.target = "webhook://" + host;
        }
        try
        {
            static_cast<void>(co_await m_repository->scoped({alarm.tenantId, false})->recordDelivery(loop, std::move(record)));
        } catch (const std::exception &failure)
        {
            LOG_WARN_FMT("通知投递记录写入失败：channel={} error={}", channel.name, failure.what());
        }
        co_return;
    }
} // namespace EdgePulse::Notification
