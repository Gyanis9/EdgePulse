#include "App/ListenerAssembly.h"
#include "Common/ApiResponse.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"
#include "TestSupport/HttpClientHarness.h"
#include "Web/MiddlewareSetup.h"
#include "Web/RequestId.h"

#include <gtest/gtest.h>

// 钉住：成功与失败的包络字段一致，失败时 data 为 null
TEST(ApiResponse, SuccessAndFailureShareEnvelopeShape)
{
    EdgePulse::TestSupport::ServerHarness server(
            [](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
            {
                EdgePulse::Web::applySecurityHeaders(router);
                router.get("/api/success",
                           [](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                           {
                               EdgePulse::Common::writeSuccess(response, AsynGyanis::Base::ConfigValue::object({{"value", 7}}), EdgePulse::Web::resolveRequestId(request));
                               co_return;
                           });
                router.get("/api/failure",
                           [](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                           {
                               EdgePulse::Common::writeFailure(response, 409, 409, "conflict", EdgePulse::Web::resolveRequestId(request));
                               co_return;
                           });
            });

    const auto success = server.get("/api/success");
    ASSERT_TRUE(success.transportOk) << success.error;
    EXPECT_EQ(200, success.status);
    const auto successJson = success.json();
    ASSERT_TRUE(successJson.has_value());
    EXPECT_EQ(7, (*successJson)["data"]["value"].get<int>());

    const auto failure = server.get("/api/failure");
    ASSERT_TRUE(failure.transportOk) << failure.error;
    EXPECT_EQ(409, failure.status);
    const auto failureJson = failure.json();
    ASSERT_TRUE(failureJson.has_value());
    EXPECT_EQ(409, (*failureJson)["code"].get<int>());
    EXPECT_EQ("conflict", (*failureJson)["message"].get<std::string>());
    EXPECT_TRUE((*failureJson)["data"].is_null());
}
