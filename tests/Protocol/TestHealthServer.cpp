#include "App/ListenerAssembly.h"
#include "TestSupport/HttpClientHarness.h"
#include "Web/MiddlewareSetup.h"

#include <gtest/gtest.h>

using EdgePulse::TestSupport::ServerHarness;

namespace
{
    ServerHarness makeServer()
    {
        return ServerHarness(
                [](AsynGyanis::Net::Router &router)
                {
                    EdgePulse::Web::applySecurityHeaders(router);
                    EdgePulse::App::registerHealthRoutes(router);
                    EdgePulse::App::registerFallbackRoute(router);
                });
    }
} // namespace

// 钉住：健康检查返回统一包络、请求标识与安全头
TEST(HealthServer, ReturnsUnifiedEnvelope)
{
    ServerHarness server = makeServer();
    const auto    result = server.get("/api/health");

    ASSERT_TRUE(result.transportOk) << result.error;
    ASSERT_EQ(200, result.status);
    const auto json = result.json();
    ASSERT_TRUE(json.has_value()) << result.body;
    EXPECT_EQ(200, (*json)["code"].get<int>());
    EXPECT_EQ("ok", (*json)["message"].get<std::string>());
    EXPECT_EQ("UP", (*json)["data"]["status"].get<std::string>());
    EXPECT_FALSE((*json)["traceId"].get<std::string>().empty());
    EXPECT_TRUE(result.header("x-request-id").has_value());
    EXPECT_EQ("nosniff", result.header("x-content-type-options").value_or(""));
    EXPECT_EQ("DENY", result.header("x-frame-options").value_or(""));
}

// 钉住：未命中路径回 JSON 404 包络，而不是空正文或 HTML 错误页
TEST(HealthServer, UnknownPathReturnsJson404)
{
    ServerHarness server = makeServer();
    const auto    result = server.get("/api/does-not-exist");

    ASSERT_TRUE(result.transportOk) << result.error;
    EXPECT_EQ(404, result.status);
    const auto json = result.json();
    ASSERT_TRUE(json.has_value()) << result.body;
    EXPECT_EQ(404, (*json)["code"].get<int>());
    EXPECT_TRUE((*json)["data"].is_null());
    EXPECT_FALSE((*json)["traceId"].get<std::string>().empty());
}
