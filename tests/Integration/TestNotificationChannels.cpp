#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Runtime/TimeFormat.h"
#include "Base/Security/Hmac.h"
#include "Base/Security/OutboundUrlGuard.h"
#include "Core/Notification/NotificationRepository.h"
#include "Core/Notification/NotificationService.h"
#include "Core/Telemetry/TelemetryModels.h"
#include "Plugins/SystemPlugin/NotificationController.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"
#include "TestSupport/HttpClientHarness.h"

#include <Net/Http/Message/HttpRequest.h>
#include <Net/Http/Message/HttpResponse.h>
#include <Net/Http/Routing/Router.h>

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

    [[nodiscard]] std::shared_ptr<EdgePulse::Common::OutboundUrlGuard> permissiveGuard()
    {
        // 本文件用 127.0.0.1 承接投递，只有地址段放行即可；JSON 与签名路径不受影响
        return std::make_shared<EdgePulse::Common::OutboundUrlGuard>(EdgePulse::Common::OutboundUrlGuardOptions{.allowPrivateNetworks = true},
                                                                     EdgePulse::Common::HostAddressResolver{});
    }

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

// 钉住：缺少出站校验器时通知服务拒绝构造，避免组合根静默关掉 SSRF 防护
TEST(NotificationChannels, RejectsMissingGuardAtConstruction)
{
    EdgePulse::TestSupport::AuthTestContext context;
    const auto                              repository = std::make_shared<EdgePulse::Notification::NotificationRepository>(context.database);
    EXPECT_THROW(static_cast<void>(EdgePulse::Notification::NotificationService{repository, nullptr}), AsynGyanis::Base::InvalidArgumentException);
}

// 钉住：通知路由在缺少出站防护时拒绝注册，启动期就失败而不是静默放行
TEST(NotificationChannels, RoutesRejectMissingGuardAtRegistration)
{
    AsynGyanis::Core::EventLoop                       loop;
    AsynGyanis::Net::Router                           router;
    EdgePulse::Notification::NotificationDependencies dependencies;
    EXPECT_THROW(static_cast<void>(EdgePulse::Notification::registerNotificationRoutes(router, loop, dependencies)), AsynGyanis::Base::InvalidArgumentException);
}

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

    EdgePulse::Notification::NotificationService service(repository, permissiveGuard());
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

// 钉住：Webhook 地址里的凭据不写进投递记录；地址本身被拒时也只记失败，不向告警链路抛出
TEST(NotificationChannels, DeliveryRecordKeepsOnlyWebhookHost)
{
    EdgePulse::TestSupport::AuthTestContext         context;
    EdgePulse::TestSupport::EventLoopRunner         runner;
    EdgePulse::Notification::NotificationRepository repository(context.database);
    EdgePulse::Notification::ChannelRecord          channel;
    const std::string                               host = "127.0.0.1:19999";
    channel.name                                         = "带凭据的 Webhook";
    channel.type                                         = "webhook";
    channel.endpoint                                     = "http://ops:topsecret@" + host + "/hook";
    channel.enabled                                      = true;
    const std::int64_t channelId                         = runner.run(repository.create(runner.loop(), channel));
    ASSERT_GT(channelId, 0);

    EdgePulse::Notification::NotificationService service(std::make_shared<EdgePulse::Notification::NotificationRepository>(context.database), permissiveGuard());
    runner.run(service.dispatch(runner.loop(), sampleAlarm(1)));

    const auto deliveries = runner.run(repository.scoped({1, false})->listDeliveries(runner.loop(), "", channelId, {1, 10}));
    ASSERT_EQ(1, deliveries.total);
    EXPECT_EQ("webhook://" + host, deliveries.rows.front().target);
    EXPECT_EQ("failed", deliveries.rows.front().status);
    EXPECT_EQ(std::string::npos, deliveries.rows.front().error.find("topsecret"));
}

// 钉住：投递前复检地址，元数据目标不发请求就记一条失败投递
TEST(NotificationChannels, DeliveryRejectsUnsafeWebhookWithoutSending)
{
    EdgePulse::TestSupport::AuthTestContext         context;
    EdgePulse::TestSupport::EventLoopRunner         runner;
    EdgePulse::Notification::NotificationRepository repository(context.database);
    EdgePulse::Notification::ChannelRecord          channel;
    channel.name                 = "元数据探测";
    channel.type                 = "webhook";
    channel.endpoint             = "http://169.254.169.254/latest/meta-data";
    channel.enabled              = true;
    const std::int64_t channelId = runner.run(repository.create(runner.loop(), channel));
    ASSERT_GT(channelId, 0);

    auto guard = std::make_shared<EdgePulse::Common::OutboundUrlGuard>(EdgePulse::Common::OutboundUrlGuardOptions{}, EdgePulse::Common::HostAddressResolver{});
    EdgePulse::Notification::NotificationService service(std::make_shared<EdgePulse::Notification::NotificationRepository>(context.database), guard);
    runner.run(service.dispatch(runner.loop(), sampleAlarm(1)));

    const auto deliveries = runner.run(repository.scoped({1, false})->listDeliveries(runner.loop(), "", channelId, {1, 10}));
    ASSERT_EQ(1, deliveries.total);
    EXPECT_EQ("failed", deliveries.rows.front().status);
    EXPECT_EQ(0, deliveries.rows.front().attempts);
    EXPECT_NE(std::string::npos, deliveries.rows.front().error.find("元数据"));
    EXPECT_EQ("webhook://169.254.169.254", deliveries.rows.front().target);
}

// 钉住：投递记录保留期只删窗口外的行，非正数天数不删任何数据
TEST(NotificationChannels, DeliveryPruneRespectsRetentionWindow)
{
    EdgePulse::TestSupport::AuthTestContext         context;
    EdgePulse::TestSupport::EventLoopRunner         runner;
    EdgePulse::Notification::NotificationRepository repository(context.database);

    EdgePulse::Notification::ChannelRecord channel;
    channel.name                 = "清理渠道";
    channel.type                 = "webhook";
    channel.endpoint             = "http://127.0.0.1:19999/hook";
    channel.enabled              = true;
    const std::int64_t channelId = runner.run(repository.create(runner.loop(), channel));
    ASSERT_GT(channelId, 0);

    auto stale        = EdgePulse::Notification::DeliveryRecord{};
    stale.tenantId    = 1;
    stale.channelId   = channelId;
    stale.channelName = channel.name;
    stale.channelType = channel.type;
    stale.target      = "webhook://127.0.0.1:19999";
    stale.status      = "succeeded";
    stale.createdAt   = "2020-01-01 00:00:00";
    auto fresh        = stale;
    fresh.createdAt   = EdgePulse::Common::nowDateTimeUtc();
    ASSERT_GT(runner.run(repository.recordDelivery(runner.loop(), stale)), 0);
    ASSERT_GT(runner.run(repository.recordDelivery(runner.loop(), fresh)), 0);

    EXPECT_FALSE(runner.run(repository.prune(runner.loop(), 0)));
    EXPECT_EQ(2, runner.run(repository.listDeliveries(runner.loop(), "", channelId, {1, 10})).total);

    ASSERT_TRUE(runner.run(repository.prune(runner.loop(), 30)));
    const auto remaining = runner.run(repository.listDeliveries(runner.loop(), "", channelId, {1, 10}));
    ASSERT_EQ(1, remaining.total);
    EXPECT_EQ(fresh.createdAt, remaining.rows.front().createdAt);
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
