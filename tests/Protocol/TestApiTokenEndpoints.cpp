#include "Base/Api/ApiResponse.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Database/Database.h"
#include "Base/Database/SqlHelpers.h"
#include "Core/Auth/ApiTokenService.h"
#include "Core/Auth/AuthService.h"
#include "Core/Http/MiddlewareSetup.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Core/Http/WebSocketEndpoints.h"
#include "Core/System/TenantRepository.h"
#include "Core/System/UserRepository.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "Plugins/AuthPlugin/AuthController.h"
#include "Plugins/SystemPlugin/ApiTokenController.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/HttpClientHarness.h"

#include <Net/WebSocket/WebSocketClient.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using AsynGyanis::Base::ConfigValue;
    using EdgePulse::TestSupport::HttpResult;

    constexpr std::int64_t kForeignTokenId = 990001; ///< 直接写入的别家租户令牌标识

    std::vector<std::pair<std::string, std::string>> bearer(const std::string &token)
    {
        return {{"Authorization", "Bearer " + token}};
    }

    [[nodiscard]] std::vector<std::string> keysOf(const ConfigValue &object)
    {
        std::vector<std::string> keys;
        keys.reserve(object.size());
        for (auto iterator = object.begin(); iterator != object.end(); ++iterator)
        {
            keys.push_back(iterator.key());
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    /**
     * @brief 递归断言载荷里没有任何键名含 hash 的字段
     * @param value 待扫描的 JSON 值
     * @param path 当前路径，用于失败时定位
     */
    void expectNoHashFields(const ConfigValue &value, const std::string &path)
    {
        if (value.is_array())
        {
            for (std::size_t index = 0; index < value.size(); ++index)
            {
                expectNoHashFields(value[index], path + "[" + std::to_string(index) + "]");
            }
            return;
        }
        if (!value.is_object())
        {
            return;
        }
        for (auto iterator = value.begin(); iterator != value.end(); ++iterator)
        {
            std::string key     = iterator.key();
            std::string lowered = key;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
            EXPECT_EQ(std::string::npos, lowered.find("hash")) << path << " 含有疑似哈希字段：" << key;
            expectNoHashFields(*iterator, path + "." + key);
        }
    }

    /**
     * @brief 断言响应禁止缓存
     * @param response HTTP 结果
     */
    void expectNoStore(const HttpResult &response)
    {
        const std::optional<std::string> header = response.header("cache-control");
        EXPECT_TRUE(header.has_value()) << response.body;
        if (header.has_value())
        {
            EXPECT_EQ("no-store", *header);
        }
    }

    struct SocketProbeResult
    {
        bool          closed{false}; ///< 收到关闭帧
        std::uint16_t closeCode{0};  ///< 关闭帧状态码
        std::string   firstText;     ///< 首帧为文本时的内容
        std::string   error;         ///< 失败原因
    };

    /**
     * @brief 用给定凭据完成浏览器实时通道的首帧认证，记录首帧与关闭状态
     * @param loop 服务器事件循环
     * @param port 回环端口
     * @param token 会话或 API 令牌
     * @param completion 结果回传
     */
    AsynGyanis::Core::Task<> probeBrowserSocket(AsynGyanis::Core::EventLoop &loop, std::uint16_t port, std::string token,
                                                std::shared_ptr<std::promise<SocketProbeResult>> completion)
    {
        SocketProbeResult result;
        try
        {
            AsynGyanis::Net::WebSocketClient::Configuration configuration;
            configuration.hostName      = "127.0.0.1";
            configuration.port          = port;
            configuration.requestTarget = "/api/ws";
            auto connection             = co_await AsynGyanis::Net::WebSocketClient::connect(loop, configuration);
            if (!connection)
            {
                result.error = "升级失败：" + connection.error().message;
                completion->set_value(std::move(result));
                co_return;
            }
            std::unique_ptr<AsynGyanis::Net::WebSocketClient> client  = std::move(*connection);
            const std::string                                 message = ConfigValue::object({{"type", "auth"}, {"token", token}}).dump();
            if (!co_await client->sendText(message))
            {
                result.error = "认证消息发送失败";
                completion->set_value(std::move(result));
                co_return;
            }
            const auto frame = co_await client->receiveFor(std::chrono::seconds{10});
            if (!frame.has_value())
            {
                result.error = "未在时限内收到首帧";
            } else if (frame->opCode == AsynGyanis::Net::WebSocketOpCode::Close)
            {
                result.closed = true;
                if (frame->payload.size() >= 2)
                {
                    result.closeCode = static_cast<std::uint16_t>((static_cast<std::uint8_t>(frame->payload[0]) << 8) | static_cast<std::uint8_t>(frame->payload[1]));
                }
            } else if (frame->opCode == AsynGyanis::Net::WebSocketOpCode::Text)
            {
                result.firstText = frame->payload;
                // 认证成功的一侧主动收口，避免把连接留到服务器停止之后
                static_cast<void>(co_await client->close(1000));
            } else
            {
                result.error = "首帧既不是文本也不是关闭帧";
            }
            client.reset();
        } catch (const std::exception &failure)
        {
            result.error = failure.what();
        }
        completion->set_value(std::move(result));
        co_return;
    }
} // namespace

class ApiTokenEndpointsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_context = std::make_unique<EdgePulse::TestSupport::AuthTestContext>();
        m_users   = std::make_shared<EdgePulse::System::UserRepository>(m_context->database);
        m_tokens  = std::make_shared<EdgePulse::Auth::ApiTokenService>(m_context->database, m_users, m_context->tenants);
        // 与生产装配一致：认证服务在会话未命中时转入令牌认证
        m_context->auth->setApiTokenService(m_tokens);
        m_browserWebSocket = std::make_shared<EdgePulse::Web::BrowserWebSocket>();
        m_server           = std::make_unique<EdgePulse::TestSupport::ServerHarness>(
                [this](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                {
                    EdgePulse::Web::applySecurityHeaders(router);
                    EdgePulse::Auth::registerRoutes(router, loop, *m_context->auth, *m_context->captcha, true);
                    EdgePulse::System::ApiTokenDependencies tokenDependencies;
                    tokenDependencies.auth   = m_context->auth.get();
                    tokenDependencies.tokens = m_tokens.get();
                    EdgePulse::System::registerApiTokenRoutes(router, loop, tokenDependencies);
                    // 只要求登录的端点用生产同款校验器：浏览器会话可用，API 令牌必须被拒
                    router.get("/api/test/session-only",
                               [this, &loop](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                               {
                                   const std::string traceId = EdgePulse::Web::resolveRequestId(request);
                                   const auto        session = co_await EdgePulse::Web::requireBrowserSession(*m_context->auth, loop, request, response, traceId);
                                   if (!session.has_value())
                                   {
                                       co_return;
                                   }
                                   EdgePulse::Common::writeSuccess(response, nullptr, traceId);
                                   co_return;
                               });
                    m_browserWebSocket->registerRoute(router, EdgePulse::Web::browserTokenValidator(m_context->auth, loop), {});
                });
    }

    void TearDown() override
    {
        // 先停服务器再销毁服务与数据库
        m_server.reset();
        m_browserWebSocket.reset();
        m_tokens.reset();
        m_users.reset();
        m_context.reset();
    }

    std::string login()
    {
        const auto captcha = m_server->get("/api/auth/captcha");
        EXPECT_EQ(200, captcha.status) << captcha.body;
        const auto captchaJson = captcha.json();
        EXPECT_TRUE(captchaJson.has_value());
        if (!captchaJson.has_value())
        {
            return {};
        }
        const std::string                   uuid     = (*captchaJson)["data"]["uuid"].get<std::string>();
        const std::string                   code     = (*captchaJson)["data"]["code"].get<std::string>();
        const AsynGyanis::Base::ConfigValue body     = ConfigValue::object({{"username", "admin"}, {"password", "admin123"}, {"captcha", code}, {"captchaUuid", uuid}});
        const auto                          response = m_server->post("/api/auth/login", body.dump());
        EXPECT_EQ(200, response.status) << response.body;
        const auto json = response.json();
        EXPECT_TRUE(json.has_value());
        if (!json.has_value())
        {
            return {};
        }
        return (*json)["data"]["token"].get<std::string>();
    }

    HttpResult createTokenRequest(const std::string &session, const std::string &name, const std::vector<std::string> &scopes)
    {
        ConfigValue body = ConfigValue::object({{"name", name}, {"scopes", ConfigValue::array()}});
        for (const std::string &scope: scopes)
        {
            body["scopes"].push_back(scope);
        }
        return m_server->post("/api/system/api-tokens", body.dump(), "application/json", bearer(session));
    }

    HttpResult rotateRequest(const std::string &session, const std::int64_t id)
    {
        return m_server->post("/api/system/api-tokens/" + std::to_string(id) + "/rotate", "{}", "application/json", bearer(session));
    }

    HttpResult deleteRequest(const std::string &session, const std::int64_t id)
    {
        return m_server->send("DELETE", "/api/system/api-tokens/" + std::to_string(id), "", "", bearer(session));
    }

    struct CreatedToken
    {
        std::int64_t id{0};     ///< 主键
        std::string  plaintext; ///< 一次性明文
        ConfigValue  data;      ///< 响应 data
    };

    CreatedToken createToken(const std::string &session, const std::string &name, const std::vector<std::string> &scopes)
    {
        const HttpResult response = createTokenRequest(session, name, scopes);
        EXPECT_EQ(200, response.status) << response.body;
        expectNoStore(response);
        const auto json = response.json();
        EXPECT_TRUE(json.has_value());
        if (!json.has_value())
        {
            return {};
        }
        CreatedToken created;
        created.data      = (*json)["data"];
        created.id        = created.data["id"].get<std::int64_t>();
        created.plaintext = created.data["token"].get<std::string>();
        expectNoHashFields(created.data, "data");
        return created;
    }

    void expirePreviousWindow(const std::int64_t id)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(m_context->databasePath().string());
        ASSERT_NE(nullptr, connection);
        ASSERT_NE(nullptr, connection->execute("UPDATE sys_api_token SET previous_valid_until = '2000-01-01 00:00:00' WHERE id = " + std::to_string(id)))
                << connection->lastError();
    }

    void insertForeignToken()
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(m_context->databasePath().string());
        ASSERT_NE(nullptr, connection);
        const std::string sql = "INSERT INTO sys_api_token (id, tenant_id, owner_user_id, name, token_prefix, token_hash, scopes_json, expires_at, created_by, created_at) "
                                "VALUES (" +
                                std::to_string(kForeignTokenId) + ", 2, 1, 'foreign', 'ep_api_foreign', '" + std::string(64, 'c') +
                                "', '[\"system:token:list\"]', datetime('now', '+1 hour'), 1, datetime('now'))";
        ASSERT_NE(nullptr, connection->execute(sql)) << connection->lastError();
    }

    [[nodiscard]] std::optional<std::string> revokedAtOf(const std::int64_t id)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(m_context->databasePath().string());
        if (connection == nullptr)
        {
            return std::nullopt;
        }
        const auto result = connection->execute("SELECT revoked_at FROM sys_api_token WHERE id = " + std::to_string(id));
        if (!result || !result->next())
        {
            return std::nullopt;
        }
        return EdgePulse::Sql::text(*result, "revoked_at");
    }

    std::unique_ptr<EdgePulse::TestSupport::AuthTestContext> m_context;
    std::shared_ptr<EdgePulse::System::UserRepository>       m_users;
    std::shared_ptr<EdgePulse::Auth::ApiTokenService>        m_tokens;
    std::shared_ptr<EdgePulse::Web::BrowserWebSocket>        m_browserWebSocket;
    std::unique_ptr<EdgePulse::TestSupport::ServerHarness>   m_server;
    std::vector<AsynGyanis::Core::Task<>>                    m_probeTasks;
};

TEST_F(ApiTokenEndpointsTest, AnonymousRequestsAreUnauthorized)
{
    EXPECT_EQ(401, m_server->get("/api/system/api-tokens").status);
    EXPECT_EQ(401, m_server->post("/api/system/api-tokens", R"({"name":"anon","scopes":["system:token:list"]})").status);
    EXPECT_EQ(401, m_server->post("/api/system/api-tokens/1/rotate", "{}", "application/json").status);
    EXPECT_EQ(401, m_server->send("DELETE", "/api/system/api-tokens/1", "", "").status);
}

TEST_F(ApiTokenEndpointsTest, CreatesListsAndRevokesTokens)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());

    const CreatedToken created = createToken(session, "ci-reader", {"system:token:list"});
    EXPECT_EQ("ep_api_", created.plaintext.substr(0, 7));
    EXPECT_EQ("ep_api_", created.data["prefix"].get<std::string>().substr(0, 7));

    std::vector<std::string> createKeys = keysOf(created.data);
    EXPECT_EQ((std::vector<std::string>{"createdAt", "expiresAt", "id", "lastUsedAt", "name", "prefix", "revokedAt", "scopes", "token"}), createKeys);

    // 浏览器会话能看到列表，摘要只含约定字段
    const HttpResult listed = m_server->get("/api/system/api-tokens", bearer(session));
    ASSERT_EQ(200, listed.status) << listed.body;
    const auto listedJson = listed.json();
    ASSERT_TRUE(listedJson.has_value());
    const ConfigValue &rows = (*listedJson)["data"];
    ASSERT_TRUE(rows.is_array());
    ASSERT_EQ(1U, rows.size());
    EXPECT_EQ(created.id, rows[0]["id"].get<std::int64_t>());
    EXPECT_EQ(created.data["prefix"], rows[0]["prefix"]);
    EXPECT_EQ((std::vector<std::string>{"createdAt", "expiresAt", "id", "lastUsedAt", "name", "prefix", "revokedAt", "scopes"}), keysOf(rows[0]));
    expectNoHashFields(rows, "data");

    // 令牌自身带 system:token:list，可以读列表
    EXPECT_EQ(200, m_server->get("/api/system/api-tokens", bearer(created.plaintext)).status);
    // 但没带 system:token:add，不能签发新令牌
    const HttpResult escalation = createTokenRequest(created.plaintext, "escalated", {"system:token:list"});
    ASSERT_EQ(403, escalation.status) << escalation.body;
    const auto escalationJson = escalation.json();
    ASSERT_TRUE(escalationJson.has_value());
    EXPECT_EQ("无权执行该操作", (*escalationJson)["message"].get<std::string>());

    // 吊销后原明文立即失效，列表仍保留已吊销行
    const HttpResult revoked = deleteRequest(session, created.id);
    ASSERT_EQ(200, revoked.status) << revoked.body;
    EXPECT_EQ(401, m_server->get("/api/system/api-tokens", bearer(created.plaintext)).status);

    const HttpResult afterRevoke = m_server->get("/api/system/api-tokens", bearer(session));
    ASSERT_EQ(200, afterRevoke.status) << afterRevoke.body;
    const auto afterJson = afterRevoke.json();
    ASSERT_TRUE(afterJson.has_value());
    ASSERT_EQ(1U, (*afterJson)["data"].size());
    EXPECT_FALSE((*afterJson)["data"][0]["revokedAt"].get<std::string>().empty());

    // 令牌会话即使带 system:token:add 也不能签发子令牌；服务层返回 403 与浏览器会话提示
    const CreatedToken adder = createToken(session, "adder", {"system:token:add"});
    const HttpResult  tokenCreate = createTokenRequest(adder.plaintext, "child", {"system:token:add"});
    ASSERT_EQ(403, tokenCreate.status) << tokenCreate.body;
    const auto tokenCreateJson = tokenCreate.json();
    ASSERT_TRUE(tokenCreateJson.has_value());
    EXPECT_NE(std::string::npos, (*tokenCreateJson)["message"].get<std::string>().find("API 令牌不能创建新令牌"));
}

TEST_F(ApiTokenEndpointsTest, RotatesWithOneHourOverlap)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const CreatedToken created = createToken(session, "rotating", {"system:token:list"});

    const HttpResult rotated = rotateRequest(session, created.id);
    ASSERT_EQ(200, rotated.status) << rotated.body;
    expectNoStore(rotated);
    const auto rotatedJson = rotated.json();
    ASSERT_TRUE(rotatedJson.has_value());
    const ConfigValue &data        = (*rotatedJson)["data"];
    const std::string  replacement = data["token"].get<std::string>();
    EXPECT_EQ("ep_api_", replacement.substr(0, 7));
    EXPECT_NE(created.plaintext, replacement);
    EXPECT_FALSE(data["previousValidUntil"].get<std::string>().empty());
    EXPECT_EQ((std::vector<std::string>{"createdAt", "expiresAt", "id", "lastUsedAt", "name", "prefix", "previousValidUntil", "revokedAt", "scopes", "token"}), keysOf(data));
    expectNoHashFields(data, "data");

    // 新旧明文在一小时窗口内都可用
    EXPECT_EQ(200, m_server->get("/api/system/api-tokens", bearer(created.plaintext)).status);
    EXPECT_EQ(200, m_server->get("/api/system/api-tokens", bearer(replacement)).status);

    // 直接改库把重叠窗口推到过去，旧明文应立即失效、新明文不受影响
    expirePreviousWindow(created.id);
    EXPECT_EQ(401, m_server->get("/api/system/api-tokens", bearer(created.plaintext)).status);
    EXPECT_EQ(200, m_server->get("/api/system/api-tokens", bearer(replacement)).status);
}

TEST_F(ApiTokenEndpointsTest, EnforcesControllerPermissionsPerRoute)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());

    // 只有 list：可以读，轮换与吊销都在控制器权限检查处被拒
    const CreatedToken listOnly = createToken(session, "list-only", {"system:token:list"});
    EXPECT_EQ(200, m_server->get("/api/system/api-tokens", bearer(listOnly.plaintext)).status);

    const HttpResult rotateDenied = rotateRequest(listOnly.plaintext, listOnly.id);
    ASSERT_EQ(403, rotateDenied.status) << rotateDenied.body;
    const auto rotateDeniedJson = rotateDenied.json();
    ASSERT_TRUE(rotateDeniedJson.has_value());
    EXPECT_EQ("无权执行该操作", (*rotateDeniedJson)["message"].get<std::string>());

    const HttpResult deleteDenied = deleteRequest(listOnly.plaintext, listOnly.id);
    ASSERT_EQ(403, deleteDenied.status) << deleteDenied.body;
    const auto deleteDeniedJson = deleteDenied.json();
    ASSERT_TRUE(deleteDeniedJson.has_value());
    EXPECT_EQ("无权执行该操作", (*deleteDeniedJson)["message"].get<std::string>());
    // 拒绝没有副作用，原令牌仍可读列表
    EXPECT_EQ(200, m_server->get("/api/system/api-tokens", bearer(listOnly.plaintext)).status);

    // list + edit：控制器权限够，但令牌会话不能轮换，服务层拒绝
    const CreatedToken editor = createToken(session, "editor", {"system:token:list", "system:token:edit"});
    const HttpResult  editorRotate = rotateRequest(editor.plaintext, editor.id);
    ASSERT_EQ(403, editorRotate.status) << editorRotate.body;
    const auto editorRotateJson = editorRotate.json();
    ASSERT_TRUE(editorRotateJson.has_value());
    EXPECT_NE(std::string::npos, (*editorRotateJson)["message"].get<std::string>().find("API 令牌不能轮换令牌"));
    ASSERT_EQ(403, deleteRequest(editor.plaintext, editor.id).status);

    // list + remove：够吊销，不够轮换
    const CreatedToken remover = createToken(session, "remover", {"system:token:list", "system:token:remove"});
    ASSERT_EQ(403, rotateRequest(remover.plaintext, remover.id).status);
    ASSERT_EQ(200, deleteRequest(remover.plaintext, remover.id).status);
}

TEST_F(ApiTokenEndpointsTest, ScopesLookupsToCurrentTenant)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const CreatedToken own = createToken(session, "own-tenant", {"system:token:list"});
    insertForeignToken();

    // 别家租户的令牌不出现在当前租户列表里
    const HttpResult listed = m_server->get("/api/system/api-tokens", bearer(session));
    ASSERT_EQ(200, listed.status) << listed.body;
    const auto listedJson = listed.json();
    ASSERT_TRUE(listedJson.has_value());
    const ConfigValue &rows = (*listedJson)["data"];
    ASSERT_TRUE(rows.is_array());
    ASSERT_EQ(1U, rows.size());
    EXPECT_EQ(own.id, rows[0]["id"].get<std::int64_t>());

    // 跨租户标识在轮换与吊销上都按不存在处理，且不产生写入
    ASSERT_EQ(404, rotateRequest(session, kForeignTokenId).status);
    ASSERT_EQ(404, deleteRequest(session, kForeignTokenId).status);
    const std::optional<std::string> foreignRevokedAt = revokedAtOf(kForeignTokenId);
    ASSERT_TRUE(foreignRevokedAt.has_value());
    EXPECT_TRUE(foreignRevokedAt->empty());

    // 同一个浏览器管理员在本租户内仍可轮换与吊销
    ASSERT_EQ(200, rotateRequest(session, own.id).status);
    ASSERT_EQ(200, deleteRequest(session, own.id).status);
}

TEST_F(ApiTokenEndpointsTest, TokenSessionCannotUsePermissionlessEndpoint)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const CreatedToken created = createToken(session, "sessionless", {"system:token:list"});

    const HttpResult browser = m_server->get("/api/test/session-only", bearer(session));
    ASSERT_EQ(200, browser.status) << browser.body;

    const HttpResult token = m_server->get("/api/test/session-only", bearer(created.plaintext));
    ASSERT_EQ(403, token.status) << token.body;
    const auto tokenJson = token.json();
    ASSERT_TRUE(tokenJson.has_value());
    EXPECT_NE(std::string::npos, (*tokenJson)["message"].get<std::string>().find("API 令牌"));
}

TEST_F(ApiTokenEndpointsTest, TokenSessionCannotUseBrowserOnlyAuthEndpoints)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const CreatedToken created = createToken(session, "browser-only", {"system:token:list"});

    // 浏览器会话可读菜单树
    const HttpResult browserRoutes = m_server->get("/api/auth/routes", bearer(session));
    ASSERT_EQ(200, browserRoutes.status) << browserRoutes.body;

    // 令牌既不能枚举 owner 的菜单与权限，也不能注销会话
    const HttpResult tokenRoutes = m_server->get("/api/auth/routes", bearer(created.plaintext));
    ASSERT_EQ(403, tokenRoutes.status) << tokenRoutes.body;
    const auto routesJson = tokenRoutes.json();
    ASSERT_TRUE(routesJson.has_value());
    EXPECT_NE(std::string::npos, (*routesJson)["message"].get<std::string>().find("API 令牌"));

    const HttpResult tokenLogout = m_server->post("/api/auth/logout", "{}", "application/json", bearer(created.plaintext));
    ASSERT_EQ(403, tokenLogout.status) << tokenLogout.body;
    const auto logoutJson = tokenLogout.json();
    ASSERT_TRUE(logoutJson.has_value());
    EXPECT_NE(std::string::npos, (*logoutJson)["message"].get<std::string>().find("API 令牌"));

    // 被拒的注销没有吊销令牌，也没有影响浏览器会话
    EXPECT_EQ(200, m_server->get("/api/auth/me", bearer(created.plaintext)).status);
    EXPECT_EQ(200, m_server->get("/api/auth/me", bearer(session)).status);
    EXPECT_EQ(200, m_server->post("/api/auth/logout", "{}", "application/json", bearer(session)).status);
}

TEST_F(ApiTokenEndpointsTest, TokenSessionCannotOpenBrowserWebSocket)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());
    const CreatedToken created = createToken(session, "ws-rejected", {"system:token:list"});

    auto                           promise = std::make_shared<std::promise<SocketProbeResult>>();
    std::future<SocketProbeResult> future  = promise->get_future();
    AsynGyanis::Core::Task<>       probe   = probeBrowserSocket(m_server->loop(), m_server->port(), created.plaintext, promise);
    m_server->loop().scheduler().scheduleRemote(probe.handle());
    m_probeTasks.push_back(std::move(probe));

    ASSERT_EQ(std::future_status::ready, future.wait_for(std::chrono::seconds{10})) << "令牌连接未在时限内收口";
    const SocketProbeResult result = future.get();
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_TRUE(result.closed);
    EXPECT_EQ(1008, result.closeCode);
}

TEST_F(ApiTokenEndpointsTest, BrowserSessionOpensBrowserWebSocket)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());

    auto                           promise = std::make_shared<std::promise<SocketProbeResult>>();
    std::future<SocketProbeResult> future  = promise->get_future();
    AsynGyanis::Core::Task<>       probe   = probeBrowserSocket(m_server->loop(), m_server->port(), session, promise);
    m_server->loop().scheduler().scheduleRemote(probe.handle());
    m_probeTasks.push_back(std::move(probe));

    ASSERT_EQ(std::future_status::ready, future.wait_for(std::chrono::seconds{10})) << "浏览器会话连接未在时限内完成认证";
    const SocketProbeResult result = future.get();
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_FALSE(result.closed);
    EXPECT_EQ(R"({"type":"auth","ok":true})", result.firstText);
}

TEST_F(ApiTokenEndpointsTest, RejectsMalformedCreateBody)
{
    const std::string session = login();
    ASSERT_FALSE(session.empty());

    EXPECT_EQ(400, m_server->post("/api/system/api-tokens", R"({"name":"missing-scopes"})", "application/json", bearer(session)).status);
    EXPECT_EQ(400, m_server->post("/api/system/api-tokens", R"({"name":"bad-scope","scopes":[1]})", "application/json", bearer(session)).status);
    EXPECT_EQ(
            400,
            m_server->post("/api/system/api-tokens", R"({"name":"bad-expiry","scopes":["system:token:list"],"expiresInSeconds":299})", "application/json", bearer(session)).status);
    EXPECT_EQ(400, m_server->post("/api/system/api-tokens/not-a-number/rotate", "{}", "application/json", bearer(session)).status);
}
