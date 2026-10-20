#include "Core/Http/SseBroadcaster.h"

#include <gtest/gtest.h>

#include <string>

// 钉住：告警 SSE 按租户投递，普通订阅者收不到别家租户的事件，平台管理员仍可跨租户汇总
TEST(SseBroadcaster, PartitionsEventsByTenant)
{
    EdgePulse::Web::SseBroadcaster broadcaster;
    const std::uint64_t           tenantOne    = broadcaster.subscribe(1, false);
    const std::uint64_t           tenantTwo    = broadcaster.subscribe(2, false);
    const std::uint64_t           platformView = broadcaster.subscribe(1, true);

    broadcaster.publish("tenant-one-alarm", 1);

    const auto tenantOneEvents = broadcaster.drain(tenantOne, 10);
    ASSERT_EQ(1U, tenantOneEvents.size());
    EXPECT_EQ("tenant-one-alarm", tenantOneEvents[0]);
    EXPECT_TRUE(broadcaster.drain(tenantTwo, 10).empty());
    const auto platformEvents = broadcaster.drain(platformView, 10);
    ASSERT_EQ(1U, platformEvents.size());

    broadcaster.publish("tenant-two-alarm", 2);
    EXPECT_TRUE(broadcaster.drain(tenantOne, 10).empty());
    const auto tenantTwoEvents = broadcaster.drain(tenantTwo, 10);
    ASSERT_EQ(1U, tenantTwoEvents.size());
    EXPECT_EQ("tenant-two-alarm", tenantTwoEvents[0]);
    EXPECT_EQ(1U, broadcaster.drain(platformView, 10).size());
}
