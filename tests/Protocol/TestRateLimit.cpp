#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Middleware.h"
#include "Net/Http/Routing/Router.h"
#include "TestSupport/HttpClientHarness.h"

#include <gtest/gtest.h>

#include <memory>

// 钉住：同一令牌桶超速返回 429
TEST(RateLimit, RejectsBurstBeyondCapacity)
{
    // 0.1 rps：两次回环请求不可能在 10 秒内补出第二枚令牌，避免快机器把用例跑成假绿
    auto                                  bucket = std::make_shared<AsynGyanis::Net::TokenBucket>(0.1, 1.0);
    EdgePulse::TestSupport::ServerHarness server(
            [bucket](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
            {
                router.addMiddleware(AsynGyanis::Net::tokenBucketRateLimiterMiddleware(bucket));
                router.get("/ping",
                           [](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                           {
                               response.setStatus(204);
                               co_return;
                           });
            });

    EXPECT_EQ(204, server.get("/ping").status);
    EXPECT_EQ(429, server.get("/ping").status);
}
