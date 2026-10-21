#include "Base/Security/Hmac.h"
#include "Core/Notification/NotificationRepository.h"
#include "Core/Notification/NotificationService.h"
#include "Core/Telemetry/TelemetryModels.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"
#include "TestSupport/HttpClientHarness.h"

#include <Net/Http/HttpRequest.h>
#include <Net/Http/HttpResponse.h>
#include <Net/Http/Router.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>

namespace
{
    struct WebhookProbe
    {
        std::promise<std::string> body;
        std::promise<std::string> signature;
        std::atomic<int>          count{0};
    };

    [[nodiscard]] EdgePulse::Alarm::AlarmRecord sampleAlarm(const std::int64_t tenantId)
    {
        EdgePulse::Alarm::AlarmRecord alarm;
        alarm.id          = 9001;
        alarm.tenantId    = tenantId;
        alarm.nodeId      = 77;
        alarm.nodeName    = "edge-webhook";
        alarm.level       = "critical";
        alarm.metric      = "cpu.usage";
        alarm.value       = 97.5;
        alarm.threshold   = 90;
        alarm.status      = "open";
        alarm.message     = "CPU 持续过高";
        alarm.triggeredAt = "2026-10-21 12:00:00";
        return alarm;
    }
} // namespace

// 钉住：启用的 Webhook 渠道收到告警 JSON 与 HMAC 签名，未达最低级别的告警不投递
TEST(NotificationChannels, WebhookReceivesSignedAlarm)
{
    auto                                  probe = std::make_shared<WebhookProbe>();
    EdgePulse::TestSupport::ServerHarness receiver(
            [probe](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
            {
                router.post("/hook",
                            [probe](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                            {
                                ++probe->count;
                                probe->body.set_value(std::string{request.body()});
                                probe->signature.set_value(request.getHeader("x-edgepulse-signature").value_or(""));
                                response.setStatus(204);
                                co_return;
                            });
            });

    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    repository = std::make_shared<EdgePulse::Notification::NotificationRepository>(context.database);
    EdgePulse::Notification::ChannelRecord  channel;
    channel.name                 = "值班 Webhook";
    channel.type                 = "webhook";
    channel.endpoint             = "http://127.0.0.1:" + std::to_string(receiver.port()) + "/hook";
    channel.secret               = "hook-secret";
    channel.minLevel             = "warning";
    channel.enabled              = true;
    const std::int64_t channelId = runner.run(repository->create(runner.loop(), channel));
    ASSERT_GT(channelId, 0);

    EdgePulse::Notification::NotificationService service(repository);
    runner.run(service.dispatch(runner.loop(), sampleAlarm(1)));

    auto bodyFuture = probe->body.get_future();
    ASSERT_EQ(std::future_status::ready, bodyFuture.wait_for(std::chrono::seconds(5)));
    const std::string body = bodyFuture.get();
    EXPECT_NE(std::string::npos, body.find("\"type\":\"alarm\""));
    EXPECT_NE(std::string::npos, body.find("\"metric\":\"cpu.usage\""));
    EXPECT_NE(std::string::npos, body.find("\"tenantId\":1"));
    EXPECT_EQ("sha256=" + EdgePulse::Common::hmacSha256Hex("hook-secret", body), probe->signature.get_future().get());
    EXPECT_EQ(1, probe->count.load());

    // 低于渠道最低级别的告警直接过滤，不再发起第二次请求
    auto quiet  = sampleAlarm(1);
    quiet.level = "info";
    runner.run(service.dispatch(runner.loop(), quiet));
    EXPECT_EQ(1, probe->count.load());
}

// 钉住：黑名单之外的邮箱口令不出现在查询结果里，仓库只在给出新口令时覆盖
TEST(NotificationChannels, RepositoryKeepsSecretAndScopesTenant)
{
    EdgePulse::TestSupport::AuthTestContext         context;
    EdgePulse::TestSupport::EventLoopRunner         runner;
    EdgePulse::Notification::NotificationRepository repository(context.database);

    EdgePulse::Notification::ChannelRecord channel;
    channel.name          = "邮件值班";
    channel.type          = "email";
    channel.endpoint      = "smtp://127.0.0.1:2525";
    channel.recipients    = "ops@example.com";
    channel.sender        = "edgepulse@example.com";
    channel.secret        = "smtp-pass";
    channel.enabled       = true;
    const std::int64_t id = runner.run(repository.create(runner.loop(), channel));
    ASSERT_GT(id, 0);
    ASSERT_TRUE(runner.run(repository.scoped({1, false})->find(runner.loop(), id)).has_value());
    EXPECT_FALSE(runner.run(repository.scoped({2, false})->find(runner.loop(), id)).has_value());

    auto edit = *runner.run(repository.find(runner.loop(), id));
    edit.secret.clear();
    edit.minLevel = "critical";
    ASSERT_TRUE(runner.run(repository.update(runner.loop(), id, edit)));
    const auto stored = runner.run(repository.find(runner.loop(), id));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ("smtp-pass", stored->secret);
    EXPECT_EQ("critical", stored->minLevel);
}
