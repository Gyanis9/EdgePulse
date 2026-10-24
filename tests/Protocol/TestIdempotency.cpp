#include "Base/Api/ApiResponse.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Database/Database.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/CaptchaService.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Crypto/Digest.h"
#include "Core/Http/AuditMiddleware.h"
#include "Core/Http/IdempotencyMiddleware.h"
#include "Core/Http/IdempotencyRepository.h"
#include "Core/Http/MiddlewareSetup.h"
#include "Core/Http/RequestId.h"
#include "Core/System/LogRepository.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "Plugins/AuthPlugin/AuthController.h"
#include "Plugins/CommandPlugin/CommandController.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/HttpClientHarness.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using AsynGyanis::Base::ConfigValue;
    using EdgePulse::TestSupport::HttpResult;

    constexpr std::int64_t kNodeId = 9001; ///< 用例自建的节点标识

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> bearer(const std::string &token)
    {
        return {{"Authorization", "Bearer " + token}};
    }

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> keyed(const std::string &token, const std::string &key)
    {
        return {{"Authorization", "Bearer " + token}, {"Idempotency-Key", key}};
    }

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> keyOnly(const std::string &key)
    {
        return {{"Idempotency-Key", key}};
    }

    [[nodiscard]] std::string commandBody(const std::string &type = "reboot")
    {
        return ConfigValue::object({{"nodeId", kNodeId}, {"type", type}, {"payload", "{}"}}).dump();
    }

    [[nodiscard]] std::string commandRequestHash(const std::string &body)
    {
        return AsynGyanis::Core::Digest::sha256Hex("POST\n/api/commands\n" + body);
    }
} // namespace

class IdempotencyTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_context     = std::make_unique<EdgePulse::TestSupport::AuthTestContext>();
        m_commands    = std::make_shared<EdgePulse::Command::CommandRepository>(m_context->database);
        m_logs        = std::make_shared<EdgePulse::System::LogRepository>(m_context->database);
        m_idempotency = std::make_shared<EdgePulse::Web::IdempotencyRepository>(m_context->database);
        seedNode();
        m_server = std::make_unique<EdgePulse::TestSupport::ServerHarness>(
                [this](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                {
                    // 与生产装配同序：安全头、审计在外，幂等键紧贴业务处理器
                    EdgePulse::Web::applySecurityHeaders(router);
                    EdgePulse::Web::registerAuditMiddleware(router, loop, *m_context->auth, *m_logs);
                    EdgePulse::Web::applyIdempotency(router, loop, m_idempotency);
                    EdgePulse::Auth::registerRoutes(router, loop, *m_context->auth, *m_context->captcha, true);
                    EdgePulse::Command::CommandDependencies dependencies;
                    dependencies.auth     = m_context->auth.get();
                    dependencies.commands = m_commands.get();
                    EdgePulse::Command::registerCommandRoutes(router, loop, dependencies);

                    // 首次失败、重试成功的写路由：钉住 5xx 不缓存
                    router.post("/api/test/idempotency/flaky",
                                [this](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    const std::string traceId = EdgePulse::Web::resolveRequestId(request);
                                    const int         call    = m_flakyCalls.fetch_add(1) + 1;
                                    if (call == 1)
                                    {
                                        EdgePulse::Common::writeFailure(response, 500, 500, "首次发送失败", traceId);
                                        co_return;
                                    }
                                    EdgePulse::Common::writeSuccess(response, ConfigValue::object({{"call", call}}), traceId);
                                    co_return;
                                });

                    // 正文超过 256 KiB 的写路由：钉住超大响应不落缓存
                    router.post("/api/test/idempotency/large",
                                [this](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    const std::string traceId = EdgePulse::Web::resolveRequestId(request);
                                    static_cast<void>(m_largeCalls.fetch_add(1));
                                    response.setStatus(200);
                                    static_cast<void>(response.setHeader("content-type", "text/plain; charset=utf-8"));
                                    response.setBody(std::string(256 * 1024 + 1, 'x'));
                                    static_cast<void>(response.setHeader(EdgePulse::Common::kRequestIdHeader, traceId));
                                    co_return;
                                });
                });
    }

    void TearDown() override
    {
        // 先停服务器再放掉仓储与数据库，避免回环线程还在写已销毁的连接池
        m_server.reset();
        m_idempotency.reset();
        m_logs.reset();
        m_commands.reset();
        m_context.reset();
    }

    void seedNode()
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(m_context->databasePath().string());
        ASSERT_NE(nullptr, connection);
        const std::string sql = "INSERT INTO biz_node (id, name, status, tenant_id) VALUES (" + std::to_string(kNodeId) + ", 'idem-node', 'online', 1)";
        ASSERT_NE(nullptr, connection->execute(sql)) << connection->lastError();
    }

    [[nodiscard]] std::int64_t countRows(const std::string &table) const
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(m_context->databasePath().string());
        if (connection == nullptr)
        {
            return -1;
        }
        const auto result = connection->execute("SELECT COUNT(*) AS total FROM " + table);
        if (!result || !result->next())
        {
            return -1;
        }
        return EdgePulse::Sql::integer(*result, "total");
    }

    [[nodiscard]] std::string idempotencyStatus() const
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(m_context->databasePath().string());
        if (connection == nullptr)
        {
            return {};
        }
        const auto result = connection->execute("SELECT status FROM sys_idempotency LIMIT 1");
        if (!result || !result->next())
        {
            return {};
        }
        return EdgePulse::Sql::text(*result, "status");
    }

    /**
     * @brief 直接写入一条幂等记录，用于构造处理中与已过期两种现场
     * @param token 请求所用的 Bearer 明文
     * @param key 客户端给出的幂等键
     * @param requestHash 请求指纹
     * @param status in_progress 或 completed
     * @param ttl 相对当前时间的有效期；负数表示已过期
     * @param withResponse true 表示写入一次已完成响应
     */
    void insertIdempotencyRow(const std::string &token, const std::string &key, const std::string &requestHash, const std::string &status, const std::chrono::seconds ttl,
                              const bool withResponse)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(m_context->databasePath().string());
        ASSERT_NE(nullptr, connection);
        const std::string                         now       = EdgePulse::Common::nowDateTimeUtc();
        const std::string                         expiresAt = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() + ttl);
        const AsynGyanis::Database::DatabaseValue responseStatus =
                withResponse ? AsynGyanis::Database::DatabaseValue{static_cast<std::int64_t>(200)} : AsynGyanis::Database::DatabaseValue{std::monostate{}};
        const AsynGyanis::Database::DatabaseValue responseBody =
                withResponse ? AsynGyanis::Database::DatabaseValue{std::string{"{}"}} : AsynGyanis::Database::DatabaseValue{std::monostate{}};
        const std::vector<AsynGyanis::Database::DatabaseValue> parameters{EdgePulse::Common::nextIdentifier(),
                                                                          AsynGyanis::Core::Digest::sha256Hex(token),
                                                                          AsynGyanis::Core::Digest::sha256Hex(key),
                                                                          requestHash,
                                                                          std::string{"POST"},
                                                                          std::string{"/api/commands"},
                                                                          status,
                                                                          responseStatus,
                                                                          std::string{},
                                                                          std::string{},
                                                                          responseBody,
                                                                          now,
                                                                          expiresAt};
        const auto                                             result = connection->execute(
                "INSERT INTO sys_idempotency (id, token_hash, key_hash, request_hash, method, path, status, response_status, response_trace_id, response_content_type, "
                "response_body, created_at, expires_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                parameters);
        ASSERT_NE(nullptr, result) << connection->lastError();
    }

    std::string login(std::vector<std::pair<std::string, std::string>> headers = {})
    {
        const auto captcha = m_server->get("/api/auth/captcha", headers);
        EXPECT_EQ(200, captcha.status) << captcha.body;
        const auto captchaJson = captcha.json();
        EXPECT_TRUE(captchaJson.has_value());
        if (!captchaJson.has_value())
        {
            return {};
        }
        const std::string uuid     = (*captchaJson)["data"]["uuid"].get<std::string>();
        const std::string code     = (*captchaJson)["data"]["code"].get<std::string>();
        const ConfigValue body     = ConfigValue::object({{"username", "admin"}, {"password", "admin123"}, {"captcha", code}, {"captchaUuid", uuid}});
        const auto        response = m_server->post("/api/auth/login", body.dump(), "application/json", std::move(headers));
        EXPECT_EQ(200, response.status) << response.body;
        const auto json = response.json();
        EXPECT_TRUE(json.has_value());
        if (!json.has_value())
        {
            return {};
        }
        return (*json)["data"]["token"].get<std::string>();
    }

    std::unique_ptr<EdgePulse::TestSupport::AuthTestContext> m_context;
    std::shared_ptr<EdgePulse::Command::CommandRepository>   m_commands;
    std::shared_ptr<EdgePulse::System::LogRepository>        m_logs;
    std::shared_ptr<EdgePulse::Web::IdempotencyRepository>   m_idempotency;
    std::unique_ptr<EdgePulse::TestSupport::ServerHarness>   m_server;
    std::atomic<int>                                         m_flakyCalls{0};
    std::atomic<int>                                         m_largeCalls{0};
};

// 钉住：同键同指纹的第二次请求直接重放首次响应，不再次执行处理器
TEST_F(IdempotencyTest, ReplaysCompletedCommandResponse)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key  = "command-idem-0001";
    const std::string body = commandBody();

    const HttpResult first = m_server->post("/api/commands", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, first.status) << first.body;
    EXPECT_FALSE(first.header("idempotent-replayed").has_value());

    const HttpResult second = m_server->post("/api/commands", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, second.status) << second.body;
    EXPECT_EQ("true", second.header("idempotent-replayed").value_or(""));
    EXPECT_EQ(first.body, second.body);
    EXPECT_EQ(first.header("content-type"), second.header("content-type"));
    ASSERT_TRUE(first.header("x-request-id").has_value());
    EXPECT_EQ(first.header("x-request-id"), second.header("x-request-id"));

    const auto firstJson  = first.json();
    const auto secondJson = second.json();
    ASSERT_TRUE(firstJson.has_value());
    ASSERT_TRUE(secondJson.has_value());
    EXPECT_EQ((*firstJson)["data"]["id"].get<std::int64_t>(), (*secondJson)["data"]["id"].get<std::int64_t>());
    EXPECT_EQ(1, countRows("biz_command"));
    EXPECT_EQ(1, countRows("sys_idempotency"));
    EXPECT_EQ("completed", idempotencyStatus());
}

// 钉住：同键不同指纹被拒绝，且不会产生第二次业务写入
TEST_F(IdempotencyTest, RejectsSameKeyWithDifferentBody)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key = "command-idem-0002";

    const HttpResult first = m_server->post("/api/commands", commandBody("reboot"), "application/json", keyed(session, key));
    ASSERT_EQ(200, first.status) << first.body;

    const HttpResult conflict = m_server->post("/api/commands", commandBody("shutdown"), "application/json", keyed(session, key));
    ASSERT_EQ(409, conflict.status) << conflict.body;
    const auto conflictJson = conflict.json();
    ASSERT_TRUE(conflictJson.has_value());
    EXPECT_EQ("幂等键已被不同的请求内容使用", (*conflictJson)["message"].get<std::string>());
    EXPECT_EQ(1, countRows("biz_command"));
    EXPECT_EQ(1, countRows("sys_idempotency"));
}

// 钉住：没有幂等键时每次请求都执行，分别生成两行业务数据
TEST_F(IdempotencyTest, RunsHandlerForEveryRequestWithoutKey)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string body = commandBody();

    const HttpResult first = m_server->post("/api/commands", body, "application/json", bearer(session));
    ASSERT_EQ(200, first.status) << first.body;
    const HttpResult second = m_server->post("/api/commands", body, "application/json", bearer(session));
    ASSERT_EQ(200, second.status) << second.body;
    EXPECT_FALSE(second.header("idempotent-replayed").has_value());

    const auto firstJson  = first.json();
    const auto secondJson = second.json();
    ASSERT_TRUE(firstJson.has_value());
    ASSERT_TRUE(secondJson.has_value());
    EXPECT_NE((*firstJson)["data"]["id"].get<std::int64_t>(), (*secondJson)["data"]["id"].get<std::int64_t>());
    EXPECT_EQ(2, countRows("biz_command"));
    EXPECT_EQ(0, countRows("sys_idempotency"));
}

// 钉住：缺 Bearer 令牌时不缓存任何状态，后续 401 由处理器正常给出
TEST_F(IdempotencyTest, DoesNotCacheWithoutBearerToken)
{
    const std::vector<std::pair<std::string, std::string>> headers = keyOnly("no-token-0000001");
    const auto                                             first   = m_server->post("/api/commands", commandBody(), "application/json", headers);
    ASSERT_EQ(401, first.status) << first.body;
    const auto second = m_server->post("/api/commands", commandBody(), "application/json", headers);
    ASSERT_EQ(401, second.status) << second.body;

    const auto json = second.json();
    ASSERT_TRUE(json.has_value());
    EXPECT_EQ(401, (*json)["code"].get<std::int64_t>());
    EXPECT_EQ(0, countRows("sys_idempotency"));
    EXPECT_EQ(0, countRows("biz_command"));
}

// 钉住：键长边界 8..128，越界在任何业务写入之前返回 400
TEST_F(IdempotencyTest, RejectsInvalidKeyLength)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());

    const auto tooShort = m_server->post("/api/commands", commandBody(), "application/json", keyed(session, "1234567"));
    ASSERT_EQ(400, tooShort.status) << tooShort.body;
    const auto tooShortJson = tooShort.json();
    ASSERT_TRUE(tooShortJson.has_value());
    EXPECT_EQ("幂等键长度必须在 8 到 128 个字符之间", (*tooShortJson)["message"].get<std::string>());

    const auto tooLong = m_server->post("/api/commands", commandBody(), "application/json", keyed(session, std::string(129, 'k')));
    ASSERT_EQ(400, tooLong.status) << tooLong.body;
    EXPECT_EQ(0, countRows("biz_command"));
    EXPECT_EQ(0, countRows("sys_idempotency"));

    // 8 个字符正好在下界上，必须放行并正常落库
    const auto minimum = m_server->post("/api/commands", commandBody(), "application/json", keyed(session, "12345678"));
    ASSERT_EQ(200, minimum.status) << minimum.body;
    EXPECT_EQ(1, countRows("biz_command"));
    EXPECT_EQ(1, countRows("sys_idempotency"));
}

// 钉住：5xx 删除占位，重试会重新执行处理器；成功之后才进入重放
TEST_F(IdempotencyTest, RetriesAfterServerError)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key  = "flaky-idem-00001";
    const std::string body = "{}";

    const HttpResult failed = m_server->post("/api/test/idempotency/flaky", body, "application/json", keyed(session, key));
    ASSERT_EQ(500, failed.status) << failed.body;
    EXPECT_FALSE(failed.header("idempotent-replayed").has_value());
    EXPECT_EQ(1, m_flakyCalls.load());
    EXPECT_EQ(0, countRows("sys_idempotency"));

    const HttpResult retried = m_server->post("/api/test/idempotency/flaky", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, retried.status) << retried.body;
    EXPECT_FALSE(retried.header("idempotent-replayed").has_value());
    EXPECT_EQ(2, m_flakyCalls.load());

    const HttpResult replayed = m_server->post("/api/test/idempotency/flaky", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, replayed.status) << replayed.body;
    EXPECT_EQ("true", replayed.header("idempotent-replayed").value_or(""));
    EXPECT_EQ(retried.body, replayed.body);
    EXPECT_EQ(2, m_flakyCalls.load());
    EXPECT_EQ(1, countRows("sys_idempotency"));
}

// 钉住：正文超过 256 KiB 时不落缓存，客户端重试仍会重新执行处理器
TEST_F(IdempotencyTest, DoesNotCacheOversizedResponse)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key  = "large-idem-00001";
    const std::string body = "{}";

    const HttpResult first = m_server->post("/api/test/idempotency/large", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, first.status) << first.body.substr(0, 200);
    EXPECT_EQ(1, m_largeCalls.load());

    const HttpResult second = m_server->post("/api/test/idempotency/large", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, second.status) << second.body.substr(0, 200);
    EXPECT_FALSE(second.header("idempotent-replayed").has_value());
    EXPECT_EQ(2, m_largeCalls.load());
    EXPECT_EQ(0, countRows("sys_idempotency"));
}

// 钉住：同键同指纹的占位仍在处理中时返回 409，而不是当成功重放
TEST_F(IdempotencyTest, ReportsInProgressPlaceholder)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key  = "inflight-0000001";
    const std::string body = commandBody();
    insertIdempotencyRow(session, key, commandRequestHash(body), "in_progress", std::chrono::hours{1}, false);

    const HttpResult response = m_server->post("/api/commands", body, "application/json", keyed(session, key));
    ASSERT_EQ(409, response.status) << response.body;
    const auto json = response.json();
    ASSERT_TRUE(json.has_value());
    EXPECT_EQ("相同幂等键的请求正在处理中", (*json)["message"].get<std::string>());
    EXPECT_EQ(0, countRows("biz_command"));
}

// 钉住：同键的过期记录先被清理，同一个键可以重新开始而不是一直冲突
TEST_F(IdempotencyTest, ClearsExpiredRecordBeforeStarting)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key            = "expired-00000001";
    const std::string oldRequestHash = commandRequestHash(commandBody("shutdown"));
    insertIdempotencyRow(session, key, oldRequestHash, "completed", std::chrono::hours{-1}, true);

    const std::string body     = commandBody();
    const HttpResult  response = m_server->post("/api/commands", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, response.status) << response.body;
    EXPECT_FALSE(response.header("idempotent-replayed").has_value());
    EXPECT_EQ(1, countRows("biz_command"));
    EXPECT_EQ(1, countRows("sys_idempotency"));
    EXPECT_EQ("completed", idempotencyStatus());
}

// 钉住：认证路径整段排除，登录请求即使带同键也不会被缓存或冲突
TEST_F(IdempotencyTest, SkipsAuthenticationPaths)
{
    const std::string key        = "login-idem-000001";
    const std::string firstToken = login(keyOnly(key));
    ASSERT_FALSE(firstToken.empty());
    const std::string secondToken = login(keyOnly(key));
    ASSERT_FALSE(secondToken.empty());
    EXPECT_NE(firstToken, secondToken);
    EXPECT_EQ(0, countRows("sys_idempotency"));
}
