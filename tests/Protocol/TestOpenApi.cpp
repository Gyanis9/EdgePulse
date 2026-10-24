#include "App/ListenerAssembly.h"
#include "Core/Http/MiddlewareSetup.h"
#include "TestSupport/HttpClientHarness.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

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

    std::vector<std::string> pathParameterNames(const std::string &path)
    {
        std::vector<std::string> names;
        for (std::size_t start = path.find('{'); start != std::string::npos; start = path.find('{', start + 1))
        {
            const std::size_t end = path.find('}', start + 1);
            if (end == std::string::npos)
            {
                break;
            }
            names.push_back(path.substr(start + 1, end - start - 1));
        }
        return names;
    }

    std::filesystem::path makeTempRoot()
    {
        const std::filesystem::path root =
                std::filesystem::temp_directory_path() / ("edgepulse-openapi-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        return root;
    }

    void writeSpec(const std::filesystem::path &root, std::string_view content)
    {
        std::ofstream stream(root / "openapi.json", std::ios::binary);
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
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
    EXPECT_FALSE(result.header("x-openapi-error").has_value());

    const auto json = result.json();
    ASSERT_TRUE(json.has_value()) << result.body;
    ASSERT_TRUE((*json)["openapi"].is_string());
    EXPECT_EQ(0u, (*json)["openapi"].get<std::string>().find("3.1"));

    const auto &paths = (*json)["paths"];
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

    // 每个 {name} 段都必须在操作级声明为 required 的 path 参数；id 与 batchId 用 int64
    for (const auto &[path, item]: paths.items())
    {
        const std::vector<std::string> expected = pathParameterNames(path);
        for (const auto &[method, operation]: item.items())
        {
            ASSERT_TRUE(operation.is_object()) << path << " " << method;
            ASSERT_TRUE(operation.contains("parameters")) << path << " " << method;
            std::set<std::string> declared;
            for (const auto &parameter: operation.at("parameters"))
            {
                ASSERT_TRUE(parameter.contains("name")) << path << " " << method;
                ASSERT_TRUE(parameter.contains("in")) << path << " " << method;
                ASSERT_TRUE(parameter.contains("required")) << path << " " << method;
                ASSERT_TRUE(parameter.contains("schema")) << path << " " << method;
                const std::string name = parameter.at("name").get<std::string>();
                EXPECT_EQ("path", parameter.at("in").get<std::string>()) << path << " " << name;
                EXPECT_TRUE(parameter.at("required").get<bool>()) << path << " " << name;
                EXPECT_TRUE(declared.insert(name).second) << path << " 重复声明 " << name;
                if (name == "id" || name == "batchId")
                {
                    EXPECT_EQ("integer", parameter.at("schema").at("type").get<std::string>()) << path << " " << name;
                    EXPECT_EQ("int64", parameter.at("schema").at("format").get<std::string>()) << path << " " << name;
                } else
                {
                    EXPECT_EQ("string", parameter.at("schema").at("type").get<std::string>()) << path << " " << name;
                }
            }
            EXPECT_EQ(expected.size(), declared.size()) << path << " " << method;
            for (const std::string &name: expected)
            {
                EXPECT_TRUE(declared.contains(name)) << path << " " << method << " 缺少 " << name;
            }
        }
    }

    // CSV 导出不是 JSON 包络：契约里必须是 text/csv 的 200
    const std::array<std::string_view, 9> exportPaths = {
            "/api/system/users/export",      "/api/system/roles/export",   "/api/system/menus/export",      "/api/system/depts/export",       "/api/system/posts/export",
            "/api/system/dict/types/export", "/api/system/configs/export", "/api/monitor/oper-logs/export", "/api/monitor/login-logs/export",
    };
    for (const std::string_view path: exportPaths)
    {
        const std::string key{path};
        ASSERT_TRUE(paths.contains(key)) << path;
        const auto &responses = paths.at(key).at("get").at("responses");
        ASSERT_TRUE(responses.contains("200")) << path;
        const auto &content = responses.at("200").at("content");
        EXPECT_TRUE(content.contains("text/csv")) << path;
        EXPECT_FALSE(content.contains("application/json")) << path;
    }

    // OIDC 两端都是 302 加 Location，不是 JSON 200
    const std::array<std::string_view, 2> redirectPaths = {"/api/auth/oidc/start", "/api/auth/oidc/callback"};
    for (const std::string_view path: redirectPaths)
    {
        const std::string key{path};
        ASSERT_TRUE(paths.contains(key)) << path;
        const auto &responses = paths.at(key).at("get").at("responses");
        EXPECT_TRUE(responses.contains("302")) << path;
        EXPECT_FALSE(responses.contains("200")) << path;
        ASSERT_TRUE(responses.contains("302")) << path;
        EXPECT_TRUE(responses.at("302").contains("headers")) << path;
        EXPECT_TRUE(responses.at("302").at("headers").contains("Location")) << path;
    }
}

// 钉住：缺失、空文件与不是 JSON 对象的生成物都按缺失处理，且与 SPA 兜底的 404 可区分
TEST(OpenApi, MissingSpecReturnsJson404)
{
    const std::filesystem::path root = makeTempRoot();
    for (const std::string_view name: {"absent", "empty", "blank", "invalid"})
    {
        std::filesystem::create_directories(root / name);
    }
    writeSpec(root / "empty", "");
    writeSpec(root / "blank", " \n\t\r\n");
    writeSpec(root / "invalid", "not a json document");

    const auto expectMissing = [](const std::filesystem::path &webRoot)
    {
        ServerHarness server = makeServer(webRoot);
        const auto    result = server.get("/api/openapi.json");

        ASSERT_TRUE(result.transportOk) << result.error;
        EXPECT_EQ(404, result.status);
        EXPECT_EQ("missing", result.header("x-openapi-error").value_or(""));
        const auto json = result.json();
        ASSERT_TRUE(json.has_value()) << result.body;
        EXPECT_EQ(404, (*json)["code"].get<int>());
        EXPECT_EQ("openapi spec not found", (*json)["message"].get<std::string>());
        EXPECT_TRUE((*json)["data"].is_null());
        EXPECT_FALSE((*json)["traceId"].get<std::string>().empty());
    };

    expectMissing(root / "absent");
    expectMissing(root / "empty");
    expectMissing(root / "blank");
    expectMissing(root / "invalid");

    std::error_code error;
    std::filesystem::remove_all(root, error);
}
