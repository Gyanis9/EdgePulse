#include "Core/Http/MiddlewareSetup.h"
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

// 钉住：存活与就绪探针不消耗令牌，桶空了也照常放行；普通路径仍然 429
TEST(RateLimit, ExemptsLivenessAndProbePaths)
{
    auto                                  bucket = std::make_shared<AsynGyanis::Net::TokenBucket>(0.1, 1.0);
    EdgePulse::TestSupport::ServerHarness server(
            [bucket](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
            {
                router.addMiddleware(EdgePulse::Web::rateLimitExceptProbes(bucket));
                const auto healthy = [](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                {
                    response.setStatus(204);
                    co_return;
                };
                router.get("/ping", healthy);
                router.get("/healthz", healthy);
                router.get("/readyz", healthy);
                router.get("/api/health", healthy);
                router.get("/api/probe/h3", healthy);
            });

    EXPECT_EQ(204, server.get("/ping").status);
    EXPECT_EQ(204, server.get("/healthz").status);
    EXPECT_EQ(204, server.get("/readyz").status);
    EXPECT_EQ(204, server.get("/api/health").status);
    EXPECT_EQ(204, server.get("/api/probe/h3").status);
    // 探针放行不影响普通路径：桶早已取空，再请求非探针路径仍是 429
    EXPECT_EQ(429, server.get("/ping").status);
}
