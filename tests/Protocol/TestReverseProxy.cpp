/**
 * @file TestReverseProxy.cpp
 * @brief 反向代理的方法、路径、查询、正文与响应头转发
 * @author Gyanis
 * @date 2026-10-21
 */

#include "Base/Api/QueryParameters.h"
#include "Base/Config/ConfigValue.h"
#include "Core/Http/ReverseProxy.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/HttpClientHarness.h"
#include "Net/Http/HttpMethod.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using AsynGyanis::Base::ConfigValue;

    [[nodiscard]] EdgePulse::Web::ReverseProxyConfiguration proxyTo(const std::uint16_t port, const bool requireSession)
    {
        EdgePulse::Web::ReverseProxyConfiguration configuration;
        configuration.prefix         = "/edge";
        configuration.upstream       = "http://127.0.0.1:" + std::to_string(port);
        configuration.requireSession = requireSession;
        return configuration;
    }
} // namespace

// 钉住：前缀剥离、方法/查询/正文与响应状态头都按上游返回转发
TEST(ReverseProxy, ForwardsMethodPathQueryBodyAndResponseHeaders)
{
    EdgePulse::TestSupport::ServerHarness upstream(
            [](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
            {
                router.any("/hello",
                           [](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                           {
                               ConfigValue payload = ConfigValue::object({{"method", std::string{AsynGyanis::Net::methodKeyword(request.method())}},
                                                                          {"path", std::string{request.path()}},
                                                                          {"query", EdgePulse::Common::queryText(request.queryParams(), "a")},
                                                                          {"body", std::string{request.body()}},
                                                                          {"forwardedFor", request.headers().contains("x-forwarded-for")},
                                                                          {"privateHeader", request.headers().contains("x-private")}});
                               response.setStatus(201);
                               response.setHeader("X-Upstream", "yes");
                               response.setOwnedBody(payload.dump());
                               co_return;
                           });
            });

    EdgePulse::TestSupport::ServerHarness proxy([&upstream](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                                                { EdgePulse::Web::registerReverseProxy(router, loop, proxyTo(upstream.port(), false), nullptr); });

    const auto result = proxy.post("/edge/hello?a=42", "payload-body", "text/plain", {{"X-Private", "secret"}});
    ASSERT_EQ(201, result.status) << result.body;
    EXPECT_EQ("yes", result.header("x-upstream").value_or(""));
    const auto json = result.json();
    ASSERT_TRUE(json.has_value()) << result.body;
    EXPECT_EQ("POST", (*json)["method"].get<std::string>());
    EXPECT_EQ("/hello", (*json)["path"].get<std::string>());
    EXPECT_EQ("42", (*json)["query"].get<std::string>());
    EXPECT_EQ("payload-body", (*json)["body"].get<std::string>());
    EXPECT_TRUE((*json)["forwardedFor"].get<bool>());
    EXPECT_FALSE((*json)["privateHeader"].get<bool>());
}

// 钉住：上游状态码与正文原样返回；要求会话时匿名请求先被 401 拦下
TEST(ReverseProxy, PreservesUpstreamStatusAndRequiresSession)
{
    EdgePulse::TestSupport::ServerHarness upstream(
            [](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
            {
                router.any("/missing",
                           [](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                           {
                               response.setStatus(404);
                               response.setOwnedBody("upstream missing");
                               co_return;
                           });
            });

    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::ServerHarness   proxy([&](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                                                  { EdgePulse::Web::registerReverseProxy(router, loop, proxyTo(upstream.port(), true), context.auth.get()); });

    const auto anonymous = proxy.get("/edge/missing");
    EXPECT_EQ(401, anonymous.status) << anonymous.body;

    EdgePulse::TestSupport::ServerHarness openProxy([&](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                                                    { EdgePulse::Web::registerReverseProxy(router, loop, proxyTo(upstream.port(), false), nullptr); });
    const auto                            missing = openProxy.get("/edge/missing");
    EXPECT_EQ(404, missing.status);
    EXPECT_EQ("upstream missing", missing.body);
}

// 钉住：超过请求正文上限的代理请求被 413 拒绝
TEST(ReverseProxy, RejectsOversizedRequestBody)
{
    EdgePulse::TestSupport::ServerHarness upstream(
            [](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
            {
                router.any("/echo",
                           [](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                           {
                               response.setOwnedBody(std::string{request.body()});
                               co_return;
                           });
            });
    EdgePulse::Web::ReverseProxyConfiguration configuration = proxyTo(upstream.port(), false);
    configuration.maximumRequestBodyBytes                   = 8;
    EdgePulse::TestSupport::ServerHarness proxy([&](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                                                { EdgePulse::Web::registerReverseProxy(router, loop, configuration, nullptr); });

    const auto result = proxy.post("/edge/echo", "0123456789", "text/plain");
    EXPECT_EQ(413, result.status) << result.body;
}
