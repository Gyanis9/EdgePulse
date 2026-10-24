#include "AgentClient.h"

#include "AgentConfiguration.h"
#include "AgentIdentity.h"
#include "TestSupport/HttpClientHarness.h"

#include "Base/Config/ConfigValue.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace EdgePulse::Agent
{
    namespace
    {
        /**
         * @brief 服务端收到的注册正文
         */
        struct RegistrationCapture
        {
            std::mutex  mutex; ///< 保护 body
            std::string body;  ///< 原始请求正文
        };

        /**
         * @brief 拼出指向回环服务器的明文测试配置
         * @param port 服务端端口
         * @return AgentConfiguration 已允许明文的配置
         */
        [[nodiscard]] AgentConfiguration plaintextConfiguration(const std::uint16_t port)
        {
            AgentConfiguration configuration;
            configuration.baseUrl                = "http://127.0.0.1:" + std::to_string(port);
            configuration.allowPlaintextForTests = true;
            configuration.name                   = "edge-01";
            configuration.tenantCode             = "tenant-a";
            configuration.requestTimeoutSeconds  = 5;
            return configuration;
        }

        /**
         * @brief 测试身份：指纹固定，不依赖真实证书文件
         * @return AgentIdentity 身份
         */
        [[nodiscard]] AgentIdentity testIdentity()
        {
            AgentIdentity identity;
            identity.fingerprint = std::string(64, 'a');
            identity.expiresAt   = "2030-01-01T00:00:00Z";
            return identity;
        }

        /**
         * @brief 在协程里执行注册并把结果交给等待方
         * @param client 注册客户端
         * @param token 注册令牌
         * @param version 版本
         * @param promise 结果通道
         * @return 协程任务
         */
        AsynGyanis::Core::Task<> deliverRegistration(AgentClient &client, std::string token, std::string version,
                                                     std::shared_ptr<std::promise<std::expected<std::int64_t, RegistrationFailure>>> promise)
        {
            promise->set_value(co_await client.registerAgent(std::move(token), std::move(version)));
            co_return;
        }

        /**
         * @brief 在独占事件循环上执行一次注册，返回前停掉循环
         * @param configuration 测试配置
         * @param identity 测试身份
         * @param token 注册令牌
         * @param version 版本
         * @return 注册结果
         * @note 客户端必须在循环停止之后才析构，否则连接池会从错误的线程收尾
         */
        [[nodiscard]] std::expected<std::int64_t, RegistrationFailure> runRegistration(const AgentConfiguration &configuration, AgentIdentity identity, std::string token,
                                                                                       std::string version)
        {
            AsynGyanis::Core::EventLoop           loop;
            std::jthread                          loopThread([&loop] { loop.run(); });
            std::vector<AsynGyanis::Core::Task<>> pendingTasks;
            AgentClient                           client(loop, configuration, std::move(identity));

            auto promise = std::make_shared<std::promise<std::expected<std::int64_t, RegistrationFailure>>>();
            auto future  = promise->get_future();
            auto task    = deliverRegistration(client, std::move(token), std::move(version), promise);
            loop.scheduler().scheduleRemote(task.handle());
            pendingTasks.push_back(std::move(task));

            std::expected<std::int64_t, RegistrationFailure> outcome = std::unexpected(RegistrationFailure{"注册未在时限内返回", false, false});
            if (future.wait_for(std::chrono::seconds{15}) == std::future_status::ready)
            {
                outcome = future.get();
            }

            loop.stop();
            if (loopThread.joinable())
            {
                loopThread.join();
            }
            return outcome;
        }

        /**
         * @brief 按给定状态码与正文注册一条注册路由
         * @param router 目标路由器
         * @param capture 请求正文捕获点
         * @param status 响应状态码
         * @param body 响应正文
         */
        void installRegisterRoute(AsynGyanis::Net::Router &router, const std::shared_ptr<RegistrationCapture> &capture, const int status, const std::string &body)
        {
            router.post("/api/agent/register",
                        [capture, status, body](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                        {
                            {
                                const std::lock_guard lock(capture->mutex);
                                capture->body = std::string{request.body()};
                            }
                            response.setStatus(status);
                            response.setBody(body);
                            co_return;
                        });
        }
    } // namespace

    TEST(AgentClient, RegistrationBodyCarriesIdentityAndOmitsEmptyToken)
    {
        auto                       capture = std::make_shared<RegistrationCapture>();
        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                                          { installRegisterRoute(router, capture, 200, R"({"code":200,"message":"ok","data":{"id":4242},"traceId":"trace-1"})"); });

        const auto outcome = runRegistration(plaintextConfiguration(server.port()), testIdentity(), {}, "1.2.3");
        ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
        EXPECT_EQ(4242, *outcome);

        std::string body;
        {
            const std::lock_guard lock(capture->mutex);
            body = capture->body;
        }
        const auto registration = AsynGyanis::Base::parseConfigValue(body);
        ASSERT_TRUE(registration.has_value()) << body;
        ASSERT_TRUE(registration->is_object()) << body;
        EXPECT_EQ("edge-01", registration->value("name", std::string{}));
        EXPECT_EQ("1.2.3", registration->value("agentVersion", std::string{}));
        EXPECT_EQ(std::string(64, 'a'), registration->value("certFingerprint", std::string{}));
        EXPECT_EQ("tenant-a", registration->value("tenantCode", std::string{}));
        EXPECT_EQ("{}", registration->value("labels", std::string{}));
        EXPECT_FALSE(registration->contains("enrollmentToken"));
    }

    TEST(AgentClient, RegistrationBodyIncludesTokenWhenSupplied)
    {
        auto                       capture = std::make_shared<RegistrationCapture>();
        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                                          { installRegisterRoute(router, capture, 200, R"({"code":200,"message":"ok","data":{"id":7},"traceId":"trace-1"})"); });

        const auto outcome = runRegistration(plaintextConfiguration(server.port()), testIdentity(), "enroll-token-value", "2.0.0");
        ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
        EXPECT_EQ(7, *outcome);

        std::string body;
        {
            const std::lock_guard lock(capture->mutex);
            body = capture->body;
        }
        const auto registration = AsynGyanis::Base::parseConfigValue(body);
        ASSERT_TRUE(registration.has_value()) << body;
        EXPECT_EQ("enroll-token-value", registration->value("enrollmentToken", std::string{}));
    }

    TEST(AgentClient, ForbiddenResponseReportsChineseReasonWithoutEchoingToken)
    {
        const std::string          token   = "secret-enrollment-token";
        auto                       capture = std::make_shared<RegistrationCapture>();
        TestSupport::ServerHarness server(
                [capture, token](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                { installRegisterRoute(router, capture, 403, R"({"code":403,"message":"令牌 )" + token + R"( 已失效","data":null,"traceId":"trace-2"})"); });

        const auto outcome = runRegistration(plaintextConfiguration(server.port()), testIdentity(), token, "1.2.3");
        ASSERT_FALSE(outcome.has_value());
        EXPECT_TRUE(outcome.error().authorizationRejected);
        EXPECT_FALSE(outcome.error().retryable);
        EXPECT_NE(std::string::npos, outcome.error().message.find("令牌"));
        EXPECT_EQ(std::string::npos, outcome.error().message.find(token));
    }

    TEST(AgentClient, ServerErrorIsRetryable)
    {
        auto                       capture = std::make_shared<RegistrationCapture>();
        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                                          { installRegisterRoute(router, capture, 503, R"({"code":503,"message":"服务暂不可用","data":null,"traceId":"trace-3"})"); });

        const auto outcome = runRegistration(plaintextConfiguration(server.port()), testIdentity(), {}, "1.2.3");
        ASSERT_FALSE(outcome.has_value());
        EXPECT_TRUE(outcome.error().retryable);
        EXPECT_FALSE(outcome.error().authorizationRejected);
        EXPECT_NE(std::string::npos, outcome.error().message.find("503"));
    }

    TEST(AgentClient, SuccessEnvelopeWithoutNodeIdIsRejected)
    {
        auto                       capture = std::make_shared<RegistrationCapture>();
        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                                          { installRegisterRoute(router, capture, 200, R"({"code":200,"message":"ok","data":{},"traceId":"trace-4"})"); });

        const auto outcome = runRegistration(plaintextConfiguration(server.port()), testIdentity(), {}, "1.2.3");
        ASSERT_FALSE(outcome.has_value());
        EXPECT_FALSE(outcome.error().retryable);
        EXPECT_NE(std::string::npos, outcome.error().message.find("data.id"));
    }
} // namespace EdgePulse::Agent
