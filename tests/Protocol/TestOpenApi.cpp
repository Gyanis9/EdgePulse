#include "App/ListenerAssembly.h"
#include "Core/Http/MiddlewareSetup.h"
#include "TestSupport/HttpClientHarness.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

using EdgePulse::TestSupport::ServerHarness;

namespace
{
    ServerHarness makeServer(const std::filesystem::path &webRoot)
    {
        return ServerHarness(
                [webRoot](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                {
                    EdgePulse::Web::applySecurityHeaders(router);
                    EdgePulse::App::registerOpenApiRoute(router, webRoot);
                    // 兜底路由与生产一致：缺失的 API 仍应拿到 JSON 404，而不是 HTML 错误页
                    EdgePulse::App::registerSiteFallback(router, std::filesystem::path{"build/gate/no-such-web"});
                });
    }
} // namespace

// 钉住：生成物 frontend/public/openapi.json 被按 JSON 提供，且认证、MFA 与 API 令牌端点出现在契约里
TEST(OpenApi, ServesGeneratedSpec)
{
    const std::filesystem::path webRoot{EDGEPULSE_SOURCE_DIR};
    ServerHarness               server = makeServer(webRoot / "frontend" / "public");
    const auto                  result = server.get("/api/openapi.json");

    ASSERT_TRUE(result.transportOk) << result.error;
    ASSERT_EQ(200, result.status);
    const std::string contentType = result.header("content-type").value_or("");
    EXPECT_EQ(0u, contentType.find("application/json"));
    EXPECT_EQ("no-cache", result.header("cache-control").value_or(""));

    const auto json = result.json();
    ASSERT_TRUE(json.has_value()) << result.body;
    ASSERT_TRUE((*json)["openapi"].is_string());
    EXPECT_EQ(0u, (*json)["openapi"].get<std::string>().find("3.1"));

    const auto paths = (*json)["paths"];
    ASSERT_TRUE(paths.is_object());
    ASSERT_TRUE(paths.contains("/api/auth/login"));
    EXPECT_TRUE(paths["/api/auth/login"].contains("post"));
    ASSERT_TRUE(paths.contains("/api/auth/mfa/verify"));
    EXPECT_TRUE(paths["/api/auth/mfa/verify"].contains("post"));
    ASSERT_TRUE(paths.contains("/api/system/api-tokens"));
    EXPECT_TRUE(paths["/api/system/api-tokens"].contains("get"));
    ASSERT_TRUE(paths.contains("/api/telemetry/latest"));
    EXPECT_TRUE(paths["/api/telemetry/latest"].contains("get"));
    // 路由参数归一为 OpenAPI 的 {name} 形式
    ASSERT_TRUE(paths.contains("/api/system/api-tokens/{id}"));
    EXPECT_TRUE(paths["/api/system/api-tokens/{id}"].contains("delete"));
}

// 钉住：缺少生成物时返回统一包络的 404 JSON，而不是 HTML 或空正文
TEST(OpenApi, MissingSpecReturnsJson404)
{
    const std::filesystem::path root =
            std::filesystem::temp_directory_path() / ("edgepulse-openapi-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);

    ServerHarness   server = makeServer(root);
    const auto      result = server.get("/api/openapi.json");
    std::error_code error;
    std::filesystem::remove_all(root, error);

    ASSERT_TRUE(result.transportOk) << result.error;
    EXPECT_EQ(404, result.status);
    const auto json = result.json();
    ASSERT_TRUE(json.has_value()) << result.body;
    EXPECT_EQ(404, (*json)["code"].get<int>());
    EXPECT_TRUE((*json)["data"].is_null());
    EXPECT_FALSE((*json)["traceId"].get<std::string>().empty());
}
