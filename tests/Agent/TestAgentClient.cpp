#include "AgentClient.h"

#include "AgentConfiguration.h"
#include "AgentIdentity.h"
#include "AgentLoop.h"
#include "CertificateFixture.h"
#include "TestSupport/HttpClientHarness.h"
#include "TestSupport/TemporaryDirectory.h"

#include "Base/Config/ConfigValue.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include "Platform/FileSystem/AtomicFileWriter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
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

    namespace
    {
        /**
         * @brief 服务端收到的续期请求正文
         */
        struct RenewalCapture
        {
            std::mutex  mutex; ///< 保护 path 与 body
            std::string path;  ///< 请求路径
            std::string body;  ///< 原始请求正文
        };

        /**
         * @brief 构造指向回环服务器的明文续期配置
         * @param port 服务端端口
         * @return AgentConfiguration 已允许明文的配置
         */
        [[nodiscard]] AgentConfiguration renewalConfiguration(const std::uint16_t port)
        {
            AgentConfiguration configuration = plaintextConfiguration(port);
            configuration.name               = "edge-01";
            return configuration;
        }

        /**
         * @brief 构造一份不发起连接（握手前即被调用）的 HTTPS 配置
         * @param material 初始证书材料
         * @return AgentConfiguration 指向本机不可达端口的 TLS 配置
         * @note 只用于身份装载与重载：用例不会真正发起请求
         */
        [[nodiscard]] AgentConfiguration httpsConfiguration(const Testing::CertificateMaterial &material)
        {
            AgentConfiguration configuration;
            configuration.baseUrl               = "https://127.0.0.1:1";
            configuration.caFile                = material.certificateFile;
            configuration.certificateFile       = material.certificateFile;
            configuration.privateKeyFile        = material.privateKeyFile;
            configuration.name                  = "edge-01";
            configuration.requestTimeoutSeconds = 5;
            return configuration;
        }

        /**
         * @brief 安装一条续期路由
         * @param router 目标路由器
         * @param capture 请求捕获点
         * @param status 响应状态码
         * @param body 响应正文
         */
        void installRenewalRoute(AsynGyanis::Net::Router &router, const std::shared_ptr<RenewalCapture> &capture, const int status, const std::string &body)
        {
            router.post("/api/agent/certificate/renew",
                        [capture, status, body](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                        {
                            {
                                const std::lock_guard lock(capture->mutex);
                                capture->path = std::string{request.path()};
                                capture->body = std::string{request.body()};
                            }
                            response.setStatus(status);
                            response.setBody(body);
                            co_return;
                        });
        }

        /**
         * @brief 在协程里执行续期并把结果交给等待方
         * @param client 续期客户端
         * @param nodeId 节点标识
         * @param csrPem CSR 文本
         * @param promise 结果通道
         * @return 协程任务
         */
        AsynGyanis::Core::Task<> deliverRenewal(AgentClient &client, std::int64_t nodeId, std::string csrPem,
                                                std::shared_ptr<std::promise<std::expected<RenewedCertificate, CertificateRenewalFailure>>> promise)
        {
            promise->set_value(co_await client.renewCertificate(nodeId, std::move(csrPem)));
            co_return;
        }

        /**
         * @brief 在独占事件循环上执行一次续期
         * @param configuration 测试配置
         * @param nodeId 节点标识
         * @param csrPem CSR 文本
         * @return 续期结果
         * @note 客户端必须在循环停止之后才析构
         */
        [[nodiscard]] std::expected<RenewedCertificate, CertificateRenewalFailure> runRenewal(const AgentConfiguration &configuration, const std::int64_t nodeId,
                                                                                              std::string csrPem)
        {
            AsynGyanis::Core::EventLoop           loop;
            std::jthread                          loopThread([&loop] { loop.run(); });
            std::vector<AsynGyanis::Core::Task<>> pendingTasks;
            AgentClient                           client(loop, configuration, testIdentity());

            auto promise = std::make_shared<std::promise<std::expected<RenewedCertificate, CertificateRenewalFailure>>>();
            auto future  = promise->get_future();
            auto task    = deliverRenewal(client, nodeId, std::move(csrPem), promise);
            loop.scheduler().scheduleRemote(task.handle());
            pendingTasks.push_back(std::move(task));

            std::expected<RenewedCertificate, CertificateRenewalFailure> outcome = std::unexpected(CertificateRenewalFailure{"续期未在时限内返回", false, false, 0});
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
         * @brief 读取文件正文
         * @param path 文件路径
         * @return std::string 内容；打不开时为空
         */
        [[nodiscard]] std::string readFileText(const std::filesystem::path &path)
        {
            std::ifstream stream(path, std::ios::binary);
            return std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        }

        /**
         * @brief 原子覆盖文件，私钥用 0600
         * @param path 目标路径
         * @param text 正文
         * @return bool 写入成功
         */
        [[nodiscard]] bool writeIdentityFile(const std::filesystem::path &path, const std::string &text)
        {
            std::string error;
            return AsynGyanis::Platform::AtomicFileWriter::writeText(path, text, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, &error);
        }

        /**
         * @brief 在事件循环上执行一次身份重载
         * @param client 目标客户端
         * @param restore 失败时的备份恢复回调，可为空
         * @return 协程任务，结果为重载是否成功
         */
        AsynGyanis::Core::Task<bool> reloadIdentityTask(AgentClient &client, std::function<bool()> restore)
        {
            co_return client.reloadIdentityFromDisk(restore);
        }
    } // namespace

    TEST(AgentClient, RenewalSendsNodeIdAndCsrAndMapsResponse)
    {
        auto                                capture        = std::make_shared<RenewalCapture>();
        const std::string                   certificatePem = "-----BEGIN CERTIFICATE-----\nMIIBfake\n-----END CERTIFICATE-----\n";
        const std::string                   fingerprint    = std::string(64U, 'b');
        const AsynGyanis::Base::ConfigValue data           = AsynGyanis::Base::ConfigValue::object({{"certificatePem", certificatePem},
                                                                                                    {"fingerprint", fingerprint},
                                                                                                    {"notBefore", "2026-10-25 08:00:00"},
                                                                                                    {"notAfter", "2027-10-25 08:00:00"},
                                                                                                    {"serial", "0a1b2c"}});
        const std::string                   response = AsynGyanis::Base::ConfigValue::object({{"code", 200}, {"message", "ok"}, {"data", data}, {"traceId", "trace-renew"}}).dump();
        TestSupport::ServerHarness          server([capture, response](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                                                   { installRenewalRoute(router, capture, 200, response); });

        const std::string csrPem  = "-----BEGIN CERTIFICATE REQUEST-----\nMIIBcsr\n-----END CERTIFICATE REQUEST-----\n";
        const auto        outcome = runRenewal(renewalConfiguration(server.port()), 4242, csrPem);
        ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
        EXPECT_EQ(certificatePem, outcome->certificatePem);
        EXPECT_EQ(fingerprint, outcome->fingerprint);
        EXPECT_EQ("2026-10-25 08:00:00", outcome->notBefore);
        EXPECT_EQ("2027-10-25 08:00:00", outcome->notAfter);
        EXPECT_EQ("0a1b2c", outcome->serial);

        std::string path;
        std::string body;
        {
            const std::lock_guard lock(capture->mutex);
            path = capture->path;
            body = capture->body;
        }
        EXPECT_EQ("/api/agent/certificate/renew", path);
        const auto request = AsynGyanis::Base::parseConfigValue(body);
        ASSERT_TRUE(request.has_value()) << body;
        EXPECT_EQ(4242, request->value("nodeId", 0));
        EXPECT_EQ(csrPem, request->value("csrPem", std::string{}));
    }

    TEST(AgentClient, RenewalForbiddenIsTerminalAndDoesNotEchoServerText)
    {
        const std::string          secret  = "revoked-node-secret";
        auto                       capture = std::make_shared<RenewalCapture>();
        TestSupport::ServerHarness server(
                [capture, secret](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                { installRenewalRoute(router, capture, 403, R"({"code":403,"message":"节点证书已吊销：)" + secret + R"(","data":null,"traceId":"trace-renew"})"); });

        const auto outcome = runRenewal(renewalConfiguration(server.port()), 7, "csr");
        ASSERT_FALSE(outcome.has_value());
        EXPECT_TRUE(outcome.error().authorizationRejected);
        EXPECT_FALSE(outcome.error().retryable);
        EXPECT_EQ(403, outcome.error().httpStatus);
        EXPECT_NE(std::string::npos, outcome.error().message.find("403"));
        EXPECT_EQ(std::string::npos, outcome.error().message.find(secret));
    }

    TEST(AgentClient, RenewalRateLimitIsRetryable)
    {
        auto                       capture = std::make_shared<RenewalCapture>();
        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                                          { installRenewalRoute(router, capture, 429, R"({"code":429,"message":"证书续期过于频繁","data":null,"traceId":"trace-renew"})"); });

        const auto outcome = runRenewal(renewalConfiguration(server.port()), 7, "csr");
        ASSERT_FALSE(outcome.has_value());
        EXPECT_TRUE(outcome.error().retryable);
        EXPECT_FALSE(outcome.error().authorizationRejected);
        EXPECT_EQ(429, outcome.error().httpStatus);
        EXPECT_NE(std::string::npos, outcome.error().message.find("频繁"));
    }

    TEST(AgentClient, RenewalInvalidCsrIsRetryable)
    {
        auto                       capture = std::make_shared<RenewalCapture>();
        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                                          { installRenewalRoute(router, capture, 400, R"({"code":400,"message":"CSR 不合法","data":null,"traceId":"trace-renew"})"); });

        // 400 不再按终止处理：续期循环要给服务端与本地时钟留出纠正机会，按更长退避重试
        const auto outcome = runRenewal(renewalConfiguration(server.port()), 7, "csr");
        ASSERT_FALSE(outcome.has_value());
        EXPECT_TRUE(outcome.error().retryable);
        EXPECT_FALSE(outcome.error().authorizationRejected);
        EXPECT_EQ(400, outcome.error().httpStatus);
    }

    TEST(AgentClient, RenewalNotFoundIsRetryable)
    {
        auto                       capture = std::make_shared<RenewalCapture>();
        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                                          { installRenewalRoute(router, capture, 404, R"({"code":404,"message":"节点不存在","data":null,"traceId":"trace-renew"})"); });

        const auto outcome = runRenewal(renewalConfiguration(server.port()), 7, "csr");
        ASSERT_FALSE(outcome.has_value());
        EXPECT_TRUE(outcome.error().retryable);
        EXPECT_FALSE(outcome.error().authorizationRejected);
        EXPECT_EQ(404, outcome.error().httpStatus);
    }

    TEST(AgentClient, RenewalMalformedSuccessBodyIsRetryable)
    {
        auto                       capture = std::make_shared<RenewalCapture>();
        TestSupport::ServerHarness server(
                [capture](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                {
                    installRenewalRoute(router, capture, 200,
                                        R"({"code":200,"message":"ok","data":{"certificatePem":"","fingerprint":")" + std::string(64U, 'c') +
                                                R"(","notAfter":"2027-10-25 08:00:00"},"traceId":"trace-renew"})");
                });

        // 200 但正文不可用时同样可能只是服务端瞬时故障，按可重试处理
        const auto outcome = runRenewal(renewalConfiguration(server.port()), 7, "csr");
        ASSERT_FALSE(outcome.has_value());
        EXPECT_TRUE(outcome.error().retryable);
        EXPECT_NE(std::string::npos, outcome.error().message.find("certificatePem"));
    }

    TEST(AgentClient, RenewalServerErrorIsRetryable)
    {
        auto                       capture = std::make_shared<RenewalCapture>();
        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                                          { installRenewalRoute(router, capture, 503, R"({"code":503,"message":"CA 未启用","data":null,"traceId":"trace-renew"})"); });

        const auto outcome = runRenewal(renewalConfiguration(server.port()), 7, "csr");
        ASSERT_FALSE(outcome.has_value());
        EXPECT_TRUE(outcome.error().retryable);
        EXPECT_EQ(503, outcome.error().httpStatus);
    }

    TEST(AgentClient, RenewalResponseWithoutCertificateIsRejected)
    {
        auto                       capture = std::make_shared<RenewalCapture>();
        TestSupport::ServerHarness server(
                [capture](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                {
                    installRenewalRoute(router, capture, 200,
                                        R"({"code":200,"message":"ok","data":{"fingerprint":")" + std::string(64U, 'c') +
                                                R"(","notAfter":"2027-10-25 08:00:00"},"traceId":"trace-renew"})");
                });

        const auto outcome = runRenewal(renewalConfiguration(server.port()), 7, "csr");
        ASSERT_FALSE(outcome.has_value());
        EXPECT_TRUE(outcome.error().retryable) << "200 但正文缺证书仍可能是服务端瞬时故障";
        EXPECT_NE(std::string::npos, outcome.error().message.find("certificatePem"));
    }

    TEST(AgentClient, ReloadIdentityFromDiskSwapsCertificate)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      initial       = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        const auto                      next          = Testing::writeSelfSignedCertificate(directory, 60L * 24L * 60L * 60L, "next");
        const auto                      configuration = httpsConfiguration(initial);

        AgentLoop     agentLoop;
        AgentClient   client(agentLoop.loop(), configuration, testIdentity());
        LoopStopGuard guard(agentLoop);

        ASSERT_TRUE(writeIdentityFile(configuration.certificateFile, readFileText(next.certificateFile)));
        ASSERT_TRUE(writeIdentityFile(configuration.privateKeyFile, readFileText(next.privateKeyFile)));

        const auto reloaded = agentLoop.run(reloadIdentityTask(client, {}), std::chrono::seconds{10}, "身份重载");
        ASSERT_TRUE(reloaded.has_value());
        EXPECT_TRUE(*reloaded);
        EXPECT_EQ(next.fingerprint, client.identity().fingerprint);
    }

    TEST(AgentClient, ReloadIdentityFromDiskRestoresBackupsOnFailure)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      initial       = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        const auto                      next          = Testing::writeSelfSignedCertificate(directory, 60L * 24L * 60L * 60L, "next");
        const auto                      configuration = httpsConfiguration(initial);

        AgentLoop     agentLoop;
        AgentClient   client(agentLoop.loop(), configuration, testIdentity());
        LoopStopGuard guard(agentLoop);

        ASSERT_TRUE(writeIdentityFile(configuration.certificateFile, readFileText(next.certificateFile)));
        ASSERT_TRUE(writeIdentityFile(configuration.privateKeyFile, readFileText(next.privateKeyFile)));
        const auto first = agentLoop.run(reloadIdentityTask(client, {}), std::chrono::seconds{10}, "首次身份重载");
        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(*first);

        const std::string goodCertificate = readFileText(configuration.certificateFile);
        const std::string goodKey         = readFileText(configuration.privateKeyFile);
        // 只改坏私钥：证书仍是新的，重载必须在装载失败后把两份文件都恢复成上一份
        ASSERT_TRUE(writeIdentityFile(configuration.privateKeyFile, "not-a-private-key"));

        const auto restore = [&configuration, goodCertificate, goodKey]()
        { return writeIdentityFile(configuration.certificateFile, goodCertificate) && writeIdentityFile(configuration.privateKeyFile, goodKey); };
        const auto reloaded = agentLoop.run(reloadIdentityTask(client, restore), std::chrono::seconds{10}, "失败身份重载");
        ASSERT_TRUE(reloaded.has_value());
        EXPECT_FALSE(*reloaded);
        EXPECT_EQ(next.fingerprint, client.identity().fingerprint);
        EXPECT_EQ(goodCertificate, readFileText(configuration.certificateFile));
        EXPECT_EQ(goodKey, readFileText(configuration.privateKeyFile));
    }

    TEST(AgentClient, ReloadIdentityFromDiskKeepsClientUsableAfterFailedLoad)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      initial       = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");
        const auto                      next          = Testing::writeSelfSignedCertificate(directory, 60L * 24L * 60L * 60L, "next");
        const auto                      configuration = httpsConfiguration(initial);

        AgentLoop     agentLoop;
        AgentClient   client(agentLoop.loop(), configuration, testIdentity());
        LoopStopGuard guard(agentLoop);

        // 新私钥坏了：构造候选客户端必须失败，旧客户端与旧身份原样保留
        ASSERT_TRUE(writeIdentityFile(configuration.certificateFile, readFileText(next.certificateFile)));
        ASSERT_TRUE(writeIdentityFile(configuration.privateKeyFile, "not-a-private-key"));
        const auto failed = agentLoop.run(reloadIdentityTask(client, {}), std::chrono::seconds{10}, "失败身份重载");
        ASSERT_TRUE(failed.has_value());
        EXPECT_FALSE(*failed);
        EXPECT_EQ(testIdentity().fingerprint, client.identity().fingerprint);

        // 修好私钥后同一条客户端仍能完成换代，说明失败没有把内部状态改坏
        ASSERT_TRUE(writeIdentityFile(configuration.privateKeyFile, readFileText(next.privateKeyFile)));
        const auto reloaded = agentLoop.run(reloadIdentityTask(client, {}), std::chrono::seconds{10}, "再次身份重载");
        ASSERT_TRUE(reloaded.has_value());
        EXPECT_TRUE(*reloaded);
        EXPECT_EQ(next.fingerprint, client.identity().fingerprint);
    }
} // namespace EdgePulse::Agent
