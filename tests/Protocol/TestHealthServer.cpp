#include "App/ListenerAssembly.h"
#include "TestSupport/HttpClientHarness.h"
#include "Core/Http/MiddlewareSetup.h"
#include "Core/Http/ReadinessState.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

using EdgePulse::TestSupport::ServerHarness;

namespace
{
    ServerHarness makeServer(AsynGyanis::Net::ReadinessCheck readiness = {})
    {
        return ServerHarness(
                [](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                {
                    EdgePulse::Web::applySecurityHeaders(router);
                    EdgePulse::App::registerHealthRoutes(router);
                    EdgePulse::App::registerSiteFallback(router, std::filesystem::path{"build/gate/no-such-web"});
                },
                std::move(readiness));
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

// 钉住：非 API 深链回 SPA 入口，API 未命中仍是 JSON 404
TEST(HealthServer, ServesSpaFallbackAndKeepsApi404)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("edgepulse-spa-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "assets");
    {
        std::ofstream index(root / "index.html");
        index << "<html><body>EdgePulse SPA</body></html>";
    }
    ServerHarness server(
            [root](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
            {
                EdgePulse::Web::applySecurityHeaders(router);
                EdgePulse::App::registerSiteFallback(router, root);
            });
    const auto deepLink = server.get("/device/nodes");
    ASSERT_TRUE(deepLink.transportOk) << deepLink.error;
    EXPECT_EQ(200, deepLink.status);
    EXPECT_NE(std::string::npos, deepLink.body.find("EdgePulse SPA"));

    const auto apiMissing = server.get("/api/does-not-exist");
    EXPECT_EQ(404, apiMissing.status);
    EXPECT_TRUE(apiMissing.json().has_value());
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// 钉住：/readyz 依赖全绿回 200，任一依赖掉线回 503 且 reason 点名该依赖
TEST(HealthServer, ReadinessReportsDependencyState)
{
    EdgePulse::Web::ReadinessState state;
    state.setDatabaseReady(true);
    state.setServiceReady(true);
    state.setCacheReady(true);
    state.setRealtimeHealthy(true);
    state.setFirmwareDirectoryReady(true);

    ServerHarness server = makeServer([&state] { return state.check(); });

    const auto ready = server.get("/readyz");
    ASSERT_TRUE(ready.transportOk) << ready.error;
    EXPECT_EQ(200, ready.status);

    state.setCacheReady(false);
    const auto unready = server.get("/readyz");
    ASSERT_TRUE(unready.transportOk) << unready.error;
    EXPECT_EQ(503, unready.status);
    EXPECT_EQ("1", unready.header("retry-after").value_or(""));
    const auto json = unready.json();
    ASSERT_TRUE(json.has_value()) << unready.body;
    EXPECT_EQ("not_ready", (*json)["status"].get<std::string>());
    EXPECT_EQ("缓存探针失败", (*json)["reason"].get<std::string>());

    // 存活性不受依赖影响：/api/health 仍回答 200，只有 /readyz 变 503
    const auto alive = server.get("/api/health");
    ASSERT_TRUE(alive.transportOk) << alive.error;
    EXPECT_EQ(200, alive.status);

    state.setCacheReady(true);
    state.setFirmwareDirectoryReady(false);
    const auto firmware = server.get("/readyz");
    ASSERT_TRUE(firmware.transportOk) << firmware.error;
    EXPECT_EQ(503, firmware.status);
    const auto firmwareJson = firmware.json();
    ASSERT_TRUE(firmwareJson.has_value()) << firmware.body;
    EXPECT_EQ("固件目录不可写", (*firmwareJson)["reason"].get<std::string>());
}
