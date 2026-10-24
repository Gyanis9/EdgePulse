#include "Base/Api/ApiResponse.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Database/Database.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/CaptchaService.h"
#include "Core/Cache/MemoryKeyValueStore.h"
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
#include "Plugins/SystemPlugin/TenantController.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"
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
    using AsynGyanis::Database::DatabaseValue;
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
                    EdgePulse::Web::applyIdempotency(router, loop, m_idempotency, *m_context->auth, "/edge");
                    EdgePulse::Auth::registerRoutes(router, loop, *m_context->auth, *m_context->captcha, true);
                    EdgePulse::Command::CommandDependencies dependencies;
                    dependencies.auth     = m_context->auth.get();
                    dependencies.commands = m_commands.get();
                    EdgePulse::Command::registerCommandRoutes(router, loop, dependencies);
                    EdgePulse::System::TenantDependencies tenantDependencies;
                    tenantDependencies.auth    = m_context->auth.get();
                    tenantDependencies.tenants = m_context->tenants.get();
                    EdgePulse::System::registerTenantRoutes(router, loop, tenantDependencies);

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

                    // 四种写方法共用的回显路由：方法、查询串与表示头都必须参与指纹
                    const auto echoHandler = [this](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = EdgePulse::Web::resolveRequestId(request);
                        const int         call    = m_echoCalls.fetch_add(1) + 1;
                        EdgePulse::Common::writeSuccess(response, ConfigValue::object({{"method", std::string{AsynGyanis::Net::methodKeyword(request.method())}}, {"call", call}}),
                                                        traceId);
                        co_return;
                    };
                    router.post("/api/test/idempotency/echo", echoHandler);
                    router.put("/api/test/idempotency/echo", echoHandler);
                    router.patch("/api/test/idempotency/echo", echoHandler);
                    router.del("/api/test/idempotency/echo", echoHandler);

                    // 201 与 204 也要能整条重放
                    router.post("/api/test/idempotency/created",
                                [this](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    const std::string traceId = EdgePulse::Web::resolveRequestId(request);
                                    static_cast<void>(m_createdCalls.fetch_add(1));
                                    EdgePulse::Common::writeSuccess(response, ConfigValue::object({{"created", true}}), traceId);
                                    response.setStatus(201);
                                    co_return;
                                });
                    router.post("/api/test/idempotency/nocontent",
                                [this](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    static_cast<void>(request);
                                    static_cast<void>(m_noContentCalls.fetch_add(1));
                                    response.setStatus(204);
                                    co_return;
                                });

                    // 处理器抛异常时要先清掉占位
                    router.post("/api/test/idempotency/throws",
                                [this](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &) -> AsynGyanis::Core::Task<>
                                {
                                    static_cast<void>(m_throwCalls.fetch_add(1));
                                    throw std::runtime_error("测试用处理器异常");
                                });

                    // 反向代理前缀下的写路由：排除后两次都要真正执行
                    router.post("/edge/echo",
                                [this](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    const std::string traceId = EdgePulse::Web::resolveRequestId(request);
                                    static_cast<void>(m_proxyCalls.fetch_add(1));
                                    EdgePulse::Common::writeSuccess(response, ConfigValue::object({{"proxied", true}}), traceId);
                                    co_return;
                                });

                    // 用户管理路径的替身：正文带明文口令，只验证整段排除后不会落幂等表
                    router.post("/api/system/users",
                                [this](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    const std::string traceId = EdgePulse::Web::resolveRequestId(request);
                                    static_cast<void>(m_userRouteCalls.fetch_add(1));
                                    EdgePulse::Common::writeSuccess(response, ConfigValue::object({{"created", true}}), traceId);
                                    co_return;
                                });

                    // 流式上传在中间件里拿不到完整正文，必须整段放行
                    router.postStreaming("/api/firmware",
                                         [this](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                         {
                                             const std::string traceId = EdgePulse::Web::resolveRequestId(request);
                                             static_cast<void>(m_firmwareRouteCalls.fetch_add(1));
                                             EdgePulse::Common::writeSuccess(response, ConfigValue::object({{"accepted", true}}), traceId);
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
     * @brief 对幂等表执行一条带参 SQL
     * @param sql 语句
     * @param parameters 绑定参数
     * @return bool 是否执行成功
     */
    bool executeIdempotencySql(const std::string &sql, const std::vector<DatabaseValue> &parameters) const
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(m_context->databasePath().string());
        if (connection == nullptr)
        {
            return false;
        }
        return connection->execute(sql, parameters) != nullptr;
    }

    /**
     * @brief 把已完成的记录改成处理中，用于构造租约内外两种现场
     * @param key 客户端给出的幂等键
     * @param age 占位创建距今的时长
     */
    void markInProgress(const std::string &key, const std::chrono::seconds age) const
    {
        const std::string                createdAt = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() - age);
        const std::vector<DatabaseValue> parameters{std::string{"in_progress"}, createdAt, AsynGyanis::Core::Digest::sha256Hex(key)};
        EXPECT_TRUE(executeIdempotencySql("UPDATE sys_idempotency SET status = ?, created_at = ?, response_status = NULL, response_trace_id = '', "
                                          "response_content_type = '', response_body = NULL WHERE key_hash = ?",
                                          parameters));
    }

    /**
     * @brief 把指定键的记录改成已过期
     * @param key 客户端给出的幂等键
     */
    void expireKey(const std::string &key) const
    {
        const std::string                expiresAt = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() - std::chrono::minutes{1});
        const std::vector<DatabaseValue> parameters{expiresAt, AsynGyanis::Core::Digest::sha256Hex(key)};
        EXPECT_TRUE(executeIdempotencySql("UPDATE sys_idempotency SET expires_at = ? WHERE key_hash = ?", parameters));
    }

    /**
     * @brief 直接改写会话快照里的权限集合，模拟登录后发生的权限降级
     * @param token 会话令牌
     * @param permissions 新的权限码
     * @note 会话快照由 SessionStore 序列化；这里写回同一份 JSON 形状，让下一次鉴权读到降级后的权限
     */
    void replaceSessionPermissions(const std::string &token, const std::vector<std::string> &permissions)
    {
        ConfigValue permissionArray = ConfigValue::array();
        for (const std::string &permission: permissions)
        {
            permissionArray.push_back(permission);
        }
        const ConfigValue                       record = ConfigValue::object({{"userId", std::int64_t{1}},
                                                                              {"tenantId", std::int64_t{1}},
                                                                              {"tenantCode", std::string{"default"}},
                                                                              {"tenantName", std::string{"默认租户"}},
                                                                              {"platformAdmin", true},
                                                                              {"username", std::string{"admin"}},
                                                                              {"nickname", std::string{"管理员"}},
                                                                              {"departmentId", std::int64_t{0}},
                                                                              {"departmentName", std::string{}},
                                                                              {"roles", ConfigValue::array()},
                                                                              {"permissions", std::move(permissionArray)}});
        EdgePulse::TestSupport::EventLoopRunner runner;
        const bool                              stored = runner.run(m_context->cache->set(runner.loop(), "edgepulse:session:" + token, record.dump(), std::chrono::hours{1}));
        ASSERT_TRUE(stored);
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
    std::atomic<int>                                         m_echoCalls{0};
    std::atomic<int>                                         m_createdCalls{0};
    std::atomic<int>                                         m_noContentCalls{0};
    std::atomic<int>                                         m_throwCalls{0};
    std::atomic<int>                                         m_proxyCalls{0};
    std::atomic<int>                                         m_userRouteCalls{0};
    std::atomic<int>                                         m_firmwareRouteCalls{0};
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

// 钉住：查询串参与指纹，同键换查询参数判冲突而不是重放
TEST_F(IdempotencyTest, RejectsSameKeyWithDifferentQuery)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key = "query-idem-000001";

    const HttpResult first = m_server->post("/api/test/idempotency/echo?client=a", "{}", "application/json", keyed(session, key));
    ASSERT_EQ(200, first.status) << first.body;
    EXPECT_EQ(1, m_echoCalls.load());

    const HttpResult second = m_server->post("/api/test/idempotency/echo?client=b", "{}", "application/json", keyed(session, key));
    ASSERT_EQ(409, second.status) << second.body;
    EXPECT_FALSE(second.header("idempotent-replayed").has_value());
    EXPECT_EQ(1, m_echoCalls.load());
    EXPECT_EQ(1, countRows("sys_idempotency"));
}

// 钉住：Content-Type 参与指纹，同键换表示类型判冲突
TEST_F(IdempotencyTest, RejectsSameKeyWithDifferentContentType)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key = "ctype-idem-000001";

    const HttpResult first = m_server->post("/api/test/idempotency/echo", "{}", "application/json", keyed(session, key));
    ASSERT_EQ(200, first.status) << first.body;
    EXPECT_EQ(1, m_echoCalls.load());

    const HttpResult second = m_server->post("/api/test/idempotency/echo", "{}", "text/plain", keyed(session, key));
    ASSERT_EQ(409, second.status) << second.body;
    EXPECT_EQ(1, m_echoCalls.load());
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

// 钉住：伪造令牌在鉴权阶段被挡下，既不能建占位也不能命中重放
TEST_F(IdempotencyTest, DoesNotCacheInvalidToken)
{
    const std::string key = "fake-token-000001";

    const HttpResult first = m_server->post("/api/commands", commandBody(), "application/json", keyed("forged-token-value", key));
    ASSERT_EQ(401, first.status) << first.body;
    const HttpResult second = m_server->post("/api/commands", commandBody(), "application/json", keyed("forged-token-value", key));
    ASSERT_EQ(401, second.status) << second.body;
    EXPECT_FALSE(second.header("idempotent-replayed").has_value());
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

    // 100 个非 ASCII 字符是 200 字节但只有 100 个字符；按字符计数时必须放行
    const std::string multibyteKey = []()
    {
        std::string value;
        for (int index = 0; index < 100; ++index)
        {
            value += "\xC3\xA9";
        }
        return value;
    }();
    const auto multibyte = m_server->post("/api/commands", commandBody("reboot-policy"), "application/json", keyed(session, multibyteKey));
    ASSERT_EQ(200, multibyte.status) << multibyte.body;
    EXPECT_EQ(2, countRows("biz_command"));
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

// 钉住：处理器抛异常时清掉占位，重试会重新执行而不是一直停在处理中
TEST_F(IdempotencyTest, AbortsPlaceholderWhenHandlerThrows)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key = "throws-idem-00001";

    const HttpResult first = m_server->post("/api/test/idempotency/throws", "{}", "application/json", keyed(session, key));
    ASSERT_EQ(500, first.status) << first.body;
    EXPECT_EQ(1, m_throwCalls.load());
    EXPECT_EQ(0, countRows("sys_idempotency"));

    const HttpResult second = m_server->post("/api/test/idempotency/throws", "{}", "application/json", keyed(session, key));
    ASSERT_EQ(500, second.status) << second.body;
    EXPECT_FALSE(second.header("idempotent-replayed").has_value());
    EXPECT_EQ(2, m_throwCalls.load());
    EXPECT_EQ(0, countRows("sys_idempotency"));
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

// 钉住：租约内的处理中占位返回 409，而不是当成功重放
TEST_F(IdempotencyTest, ReportsInProgressPlaceholder)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key  = "inflight-0000001";
    const std::string body = commandBody();

    const HttpResult first = m_server->post("/api/commands", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, first.status) << first.body;
    markInProgress(key, std::chrono::seconds{5});

    const HttpResult response = m_server->post("/api/commands", body, "application/json", keyed(session, key));
    ASSERT_EQ(409, response.status) << response.body;
    const auto json = response.json();
    ASSERT_TRUE(json.has_value());
    EXPECT_EQ("相同幂等键的请求正在处理中", (*json)["message"].get<std::string>());
    EXPECT_EQ(1, countRows("biz_command"));
}

// 钉住：超过租约的处理中占位按崩溃残留接管，同键请求可以重新执行
TEST_F(IdempotencyTest, ReclaimsStaleInProgress)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key  = "stale-inflight-01";
    const std::string body = commandBody();

    const HttpResult first = m_server->post("/api/commands", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, first.status) << first.body;
    markInProgress(key, std::chrono::seconds{120});

    const HttpResult reclaimed = m_server->post("/api/commands", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, reclaimed.status) << reclaimed.body;
    EXPECT_FALSE(reclaimed.header("idempotent-replayed").has_value());
    EXPECT_EQ(2, countRows("biz_command"));
    EXPECT_EQ(1, countRows("sys_idempotency"));
    EXPECT_EQ("completed", idempotencyStatus());
}

// 钉住：同键的过期记录先被清理，同一个键可以重新开始而不是一直冲突
TEST_F(IdempotencyTest, ClearsExpiredRecordBeforeStarting)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key  = "expired-00000001";
    const std::string body = commandBody();

    const HttpResult first = m_server->post("/api/commands", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, first.status) << first.body;
    expireKey(key);

    const HttpResult response = m_server->post("/api/commands", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, response.status) << response.body;
    EXPECT_FALSE(response.header("idempotent-replayed").has_value());
    EXPECT_EQ(2, countRows("biz_command"));
    EXPECT_EQ(1, countRows("sys_idempotency"));
    EXPECT_EQ("completed", idempotencyStatus());
}

// 钉住：全表清理会物理删除已到期记录，不依赖同键再次访问
TEST_F(IdempotencyTest, PrunesExpiredRows)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key = "prune-idem-000001";

    const HttpResult created = m_server->post("/api/test/idempotency/echo", "{}", "application/json", keyed(session, key));
    ASSERT_EQ(200, created.status) << created.body;
    ASSERT_EQ(1, countRows("sys_idempotency"));
    expireKey(key);

    EdgePulse::TestSupport::EventLoopRunner runner;
    const std::size_t                       removed = runner.run(m_idempotency->pruneExpired(runner.loop()));
    EXPECT_EQ(1, removed);
    EXPECT_EQ(0, countRows("sys_idempotency"));
}

// 钉住：权限降级后同键重放判冲突，不再把旧的已授权响应交回
TEST_F(IdempotencyTest, ConflictsAfterPermissionDowngrade)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key  = "perm-idem-0000001";
    const std::string body = commandBody();

    const HttpResult first = m_server->post("/api/commands", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, first.status) << first.body;

    replaceSessionPermissions(session, {});

    const HttpResult replay = m_server->post("/api/commands", body, "application/json", keyed(session, key));
    ASSERT_EQ(409, replay.status) << replay.body;
    EXPECT_FALSE(replay.header("idempotent-replayed").has_value());
    const auto json = replay.json();
    ASSERT_TRUE(json.has_value());
    EXPECT_EQ("幂等键已被不同的请求内容使用", (*json)["message"].get<std::string>());
    EXPECT_EQ(1, countRows("biz_command"));
    EXPECT_EQ(1, countRows("sys_idempotency"));
}

// 钉住：403 不落缓存，权限不足的请求每次都要重新判定
TEST_F(IdempotencyTest, DoesNotCacheForbiddenResponses)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    replaceSessionPermissions(session, {});
    const std::string key = "forbidden-0000001";

    const HttpResult first = m_server->post("/api/commands", commandBody(), "application/json", keyed(session, key));
    ASSERT_EQ(403, first.status) << first.body;
    const HttpResult second = m_server->post("/api/commands", commandBody(), "application/json", keyed(session, key));
    ASSERT_EQ(403, second.status) << second.body;
    EXPECT_FALSE(second.header("idempotent-replayed").has_value());
    EXPECT_EQ(0, countRows("sys_idempotency"));
    EXPECT_EQ(0, countRows("biz_command"));
}

// 钉住：会话被吊销后，同键重放拿到 401 而不是缓存里的成功响应
TEST_F(IdempotencyTest, RejectsReplayAfterSessionRevoked)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key  = "revoked-idem-0001";
    const std::string body = commandBody();

    const HttpResult first = m_server->post("/api/commands", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, first.status) << first.body;

    const HttpResult logout = m_server->post("/api/auth/logout", "{}", "application/json", bearer(session));
    ASSERT_EQ(200, logout.status) << logout.body;

    const HttpResult replay = m_server->post("/api/commands", body, "application/json", keyed(session, key));
    ASSERT_EQ(401, replay.status) << replay.body;
    EXPECT_FALSE(replay.header("idempotent-replayed").has_value());
    EXPECT_EQ(1, countRows("biz_command"));
    // 完成态记录仍在表里，但已无法被吊销后的会话使用
    EXPECT_EQ(1, countRows("sys_idempotency"));
}

// 钉住：凭证不同的两个会话共用同一个键互不干扰
TEST_F(IdempotencyTest, KeepsPrincipalIsolation)
{
    const std::string firstSession  = login();
    const std::string secondSession = login();
    ASSERT_FALSE(firstSession.empty());
    ASSERT_FALSE(secondSession.empty());
    ASSERT_NE(firstSession, secondSession);
    const std::string key  = "shared-key-000001";
    const std::string body = commandBody();

    const HttpResult first = m_server->post("/api/commands", body, "application/json", keyed(firstSession, key));
    ASSERT_EQ(200, first.status) << first.body;
    const HttpResult second = m_server->post("/api/commands", body, "application/json", keyed(secondSession, key));
    ASSERT_EQ(200, second.status) << second.body;
    EXPECT_FALSE(second.header("idempotent-replayed").has_value());
    EXPECT_EQ(2, countRows("biz_command"));
    EXPECT_EQ(2, countRows("sys_idempotency"));
}

// 钉住：PUT/PATCH/DELETE 与 POST 一样参与幂等
TEST_F(IdempotencyTest, ReplaysEveryMutatingMethod)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());

    const std::vector<std::string> methods{"PUT", "PATCH", "DELETE"};
    for (const std::string &method: methods)
    {
        const std::string key    = "method-" + method + "-0001";
        const int         before = m_echoCalls.load();
        const HttpResult  first  = m_server->send(method, "/api/test/idempotency/echo", "{}", "application/json", keyed(session, key));
        ASSERT_EQ(200, first.status) << method << ' ' << first.body;
        EXPECT_EQ(before + 1, m_echoCalls.load()) << method;

        const HttpResult second = m_server->send(method, "/api/test/idempotency/echo", "{}", "application/json", keyed(session, key));
        ASSERT_EQ(200, second.status) << method << ' ' << second.body;
        EXPECT_EQ("true", second.header("idempotent-replayed").value_or("")) << method;
        EXPECT_EQ(first.body, second.body) << method;
        EXPECT_EQ(before + 1, m_echoCalls.load()) << method;
    }
}

// 钉住：201 与 204 也要整条重放状态码与正文
TEST_F(IdempotencyTest, ReplaysCreatedAndNoContentResponses)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());

    const HttpResult createdFirst = m_server->post("/api/test/idempotency/created", "{}", "application/json", keyed(session, "created-idem-0001"));
    ASSERT_EQ(201, createdFirst.status) << createdFirst.body;
    const HttpResult createdReplay = m_server->post("/api/test/idempotency/created", "{}", "application/json", keyed(session, "created-idem-0001"));
    ASSERT_EQ(201, createdReplay.status) << createdReplay.body;
    EXPECT_EQ("true", createdReplay.header("idempotent-replayed").value_or(""));
    EXPECT_EQ(createdFirst.body, createdReplay.body);
    EXPECT_EQ(1, m_createdCalls.load());

    const HttpResult noContentFirst = m_server->post("/api/test/idempotency/nocontent", "{}", "application/json", keyed(session, "nocontent-idem-01"));
    ASSERT_EQ(204, noContentFirst.status) << noContentFirst.body;
    const HttpResult noContentReplay = m_server->post("/api/test/idempotency/nocontent", "{}", "application/json", keyed(session, "nocontent-idem-01"));
    ASSERT_EQ(204, noContentReplay.status) << noContentReplay.body;
    EXPECT_EQ("true", noContentReplay.header("idempotent-replayed").value_or(""));
    EXPECT_EQ(1, m_noContentCalls.load());
}

// 钉住：认证路由整段排除，带合法令牌的注销请求两次都真正执行
TEST_F(IdempotencyTest, SkipsExcludedAuthRouteWithValidToken)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key = "logout-idem-00001";

    const HttpResult first = m_server->post("/api/auth/logout", "{}", "application/json", keyed(session, key));
    ASSERT_EQ(200, first.status) << first.body;
    EXPECT_FALSE(first.header("idempotent-replayed").has_value());

    const HttpResult second = m_server->post("/api/auth/logout", "{}", "application/json", keyed(session, key));
    ASSERT_EQ(401, second.status) << second.body;
    EXPECT_FALSE(second.header("idempotent-replayed").has_value());
    EXPECT_EQ(0, countRows("sys_idempotency"));
}

// 钉住：带明文口令的用户管理接口不参与幂等，口令不会进幂等表
TEST_F(IdempotencyTest, SkipsPasswordBearingUserRoute)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key  = "user-idem-0000001";
    const std::string body = ConfigValue::object({{"username", std::string{"idem-user-1"}}, {"password", std::string{"userpass1"}}, {"nickname", std::string{"幂等用户"}}}).dump();

    const HttpResult first = m_server->post("/api/system/users", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, first.status) << first.body;
    const HttpResult second = m_server->post("/api/system/users", body, "application/json", keyed(session, key));
    ASSERT_EQ(200, second.status) << second.body;
    EXPECT_FALSE(second.header("idempotent-replayed").has_value());
    EXPECT_EQ(2, m_userRouteCalls.load());
    EXPECT_EQ(0, countRows("sys_idempotency"));
}

// 钉住：租户管理接口整段排除，同键重复请求仍由处理器给出真实结果
TEST_F(IdempotencyTest, SkipsTenantRoutes)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key = "tenant-idem-000001";

    const HttpResult first = m_server->send("PUT", "/api/system/tenants/99999", "{}", "application/json", keyed(session, key));
    ASSERT_EQ(404, first.status) << first.body;
    const HttpResult second = m_server->send("PUT", "/api/system/tenants/99999", "{}", "application/json", keyed(session, key));
    ASSERT_EQ(404, second.status) << second.body;
    EXPECT_FALSE(second.header("idempotent-replayed").has_value());
    EXPECT_EQ(0, countRows("sys_idempotency"));
}

// 钉住：流式上传路径整段排除，中间件不读正文也不落缓存
TEST_F(IdempotencyTest, SkipsStreamingFirmwareUpload)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key  = "firmware-idem-0001";
    const std::string body = "firmware-bytes";

    const HttpResult first = m_server->post("/api/firmware", body, "application/octet-stream", keyed(session, key));
    ASSERT_EQ(200, first.status) << first.body;
    const HttpResult second = m_server->post("/api/firmware", body, "application/octet-stream", keyed(session, key));
    ASSERT_EQ(200, second.status) << second.body;
    EXPECT_FALSE(second.header("idempotent-replayed").has_value());
    EXPECT_EQ(2, m_firmwareRouteCalls.load());
    EXPECT_EQ(0, countRows("sys_idempotency"));
}

// 钉住：反向代理前缀下的写请求不参与幂等，上游响应不会被本进程缓存
TEST_F(IdempotencyTest, SkipsReverseProxyPrefix)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const std::string key = "proxy-idem-0000001";

    const HttpResult first = m_server->post("/edge/echo", "{}", "application/json", keyed(session, key));
    ASSERT_EQ(200, first.status) << first.body;
    const HttpResult second = m_server->post("/edge/echo", "{}", "application/json", keyed(session, key));
    ASSERT_EQ(200, second.status) << second.body;
    EXPECT_FALSE(second.header("idempotent-replayed").has_value());
    EXPECT_EQ(2, m_proxyCalls.load());
    EXPECT_EQ(0, countRows("sys_idempotency"));
}
