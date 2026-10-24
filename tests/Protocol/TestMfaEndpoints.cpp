#include "Base/Coding/Base64.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Database/Database.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/TimeFormat.h"
#include "Base/Security/RandomToken.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/DataScopeService.h"
#include "Core/Auth/MfaService.h"
#include "Core/Auth/OidcService.h"
#include "Core/Auth/Totp.h"
#include "Core/Cache/KeyValueStore.h"
#include "Core/Cache/MemoryKeyValueStore.h"
#include "Core/Http/AuditMiddleware.h"
#include "Core/Http/MiddlewareSetup.h"
#include "Core/System/DictionaryRepository.h"
#include "Core/System/LogRepository.h"
#include "Core/System/MenuRepository.h"
#include "Core/System/OrgRepository.h"
#include "Core/System/RoleRepository.h"
#include "Core/System/TenantRepository.h"
#include "Core/System/UserRepository.h"
#include "Plugins/AuthPlugin/AuthController.h"
#include "Plugins/AuthPlugin/MfaController.h"
#include "Plugins/AuthPlugin/OidcController.h"
#include "Plugins/MonitorPlugin/MonitorController.h"
#include "Plugins/SystemPlugin/SystemController.h"
#include "Plugins/SystemPlugin/TenantController.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"
#include "TestSupport/HttpClientHarness.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    using EdgePulse::Auth::MfaService;
    using EdgePulse::Auth::SessionRecord;
    using EdgePulse::TestSupport::AuthTestContext;
    using EdgePulse::TestSupport::EventLoopRunner;
    using EdgePulse::TestSupport::HttpResult;
    using EdgePulse::TestSupport::ServerHarness;

    namespace Totp = EdgePulse::Auth::Totp;

    /**
     * @brief 固定 32 字节主密钥，测试进程内可复现
     */
    [[nodiscard]] std::string rawMfaKey()
    {
        std::string key;
        key.reserve(32);
        for (int index = 0; index < 32; ++index)
        {
            key.push_back(static_cast<char>(index + 11));
        }
        return key;
    }

    [[nodiscard]] std::string encodedMfaKey()
    {
        return AsynGyanis::Base::base64Encode(rawMfaKey());
    }

    /**
     * @brief 计算与窗口内有效码不同的验证码
     */
    [[nodiscard]] std::string wrongCode(const std::string &secret, const std::int64_t step)
    {
        std::string code = Totp::code(secret, step);
        if (code.empty())
        {
            return "000000";
        }
        code.back() = code.back() == '9' ? '0' : static_cast<char>(code.back() + 1);
        return code;
    }

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> bearer(const std::string &token)
    {
        return {{"Authorization", "Bearer " + token}};
    }

    /**
     * @brief 把挑战缓存写入改成失败的包装，用于验证 503 不被改写成 401
     */
    class FailingChallengeStore final : public EdgePulse::Cache::KeyValueStore
    {
    public:
        explicit FailingChallengeStore(std::shared_ptr<EdgePulse::Cache::KeyValueStore> inner) : m_inner(std::move(inner))
        {
        }

        AsynGyanis::Core::Task<std::optional<std::string>> get(AsynGyanis::Core::EventLoop &loop, std::string key) override
        {
            co_return co_await m_inner->get(loop, std::move(key));
        }

        AsynGyanis::Core::Task<std::optional<std::string>> take(AsynGyanis::Core::EventLoop &loop, std::string key) override
        {
            co_return co_await m_inner->take(loop, std::move(key));
        }

        AsynGyanis::Core::Task<bool> set(AsynGyanis::Core::EventLoop &loop, std::string key, std::string value, std::chrono::seconds ttl) override
        {
            if (key.starts_with("edgepulse:mfa:challenge:"))
            {
                co_return false;
            }
            co_return co_await m_inner->set(loop, std::move(key), std::move(value), ttl);
        }

        AsynGyanis::Core::Task<bool> erase(AsynGyanis::Core::EventLoop &loop, std::string key) override
        {
            co_return co_await m_inner->erase(loop, std::move(key));
        }

        AsynGyanis::Core::Task<std::int64_t> increment(AsynGyanis::Core::EventLoop &loop, std::string key, std::chrono::seconds ttl) override
        {
            co_return co_await m_inner->increment(loop, std::move(key), ttl);
        }

        [[nodiscard]] std::string_view backendName() const noexcept override
        {
            return m_inner->backendName();
        }

    private:
        std::shared_ptr<EdgePulse::Cache::KeyValueStore> m_inner; ///< 实际存储
    };

    /**
     * @brief 把协程结果搬进 promise，供同一条事件循环上的并发用例各自等待
     */
    template<typename ResultType>
    AsynGyanis::Core::Task<> forwardInto(AsynGyanis::Core::Task<ResultType> task, std::shared_ptr<std::promise<ResultType>> promise)
    {
        try
        {
            if constexpr (std::is_void_v<ResultType>)
            {
                co_await task;
                promise->set_value();
            } else
            {
                promise->set_value(co_await task);
            }
        } catch (...)
        {
            promise->set_exception(std::current_exception());
        }
    }

    [[nodiscard]] std::int64_t nowEpochSeconds()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    [[nodiscard]] std::optional<AsynGyanis::Base::ConfigValue> responseData(const HttpResult &result)
    {
        const auto json = result.json();
        if (!json.has_value() || !json->is_object())
        {
            return std::nullopt;
        }
        const auto iterator = json->find("data");
        if (iterator == json->end() || !iterator->is_object())
        {
            return std::nullopt;
        }
        return *iterator;
    }

    [[nodiscard]] std::string stringData(const HttpResult &result, const std::string &field)
    {
        const auto data = responseData(result);
        if (!data.has_value())
        {
            return {};
        }
        return data->value(field, std::string{});
    }

    [[nodiscard]] AsynGyanis::Base::ConfigValue stringArray(const std::vector<std::string> &values)
    {
        AsynGyanis::Base::ConfigValue array = AsynGyanis::Base::ConfigValue::array();
        for (const std::string &value: values)
        {
            array.push_back(value);
        }
        return array;
    }

    struct CaptchaPair
    {
        std::string uuid;
        std::string code;
    };

    struct MfaActivation
    {
        std::string              secret;
        std::vector<std::string> recoveryCodes;
        std::int64_t             enableStep{0};
    };

    struct LoginAuditRow
    {
        std::int64_t userId{0};
        std::string  message;
    };

    struct OperAuditRow
    {
        std::string  url;
        std::string  title;
        std::string  params;
        std::string  errorMessage;
        std::int64_t operId{0};
        std::string  ip;
        int          result{0};
    };

    struct LoginFailureState
    {
        int         count{0};
        std::string lockedUntil;
    };

    /**
     * @brief 打开临时库的直连，用于核对审计落库内容
     */
    [[nodiscard]] std::unique_ptr<AsynGyanis::Database::DatabaseConnection> openDirect(const AuthTestContext &context)
    {
        return EdgePulse::Common::Database::openDirectSqlite(context.databasePath().string());
    }

    [[nodiscard]] std::vector<LoginAuditRow> readLoginAudits(const AuthTestContext &context)
    {
        const auto                 connection = openDirect(context);
        const auto                 result     = connection->execute("SELECT user_id, message FROM sys_login_log ORDER BY login_time, id");
        std::vector<LoginAuditRow> rows;
        if (!result)
        {
            return rows;
        }
        while (result->next())
        {
            rows.push_back(LoginAuditRow{EdgePulse::Sql::integer(*result, "user_id"), EdgePulse::Sql::text(*result, "message")});
        }
        return rows;
    }

    [[nodiscard]] std::vector<OperAuditRow> readOperAudits(const AuthTestContext &context)
    {
        const auto                connection = openDirect(context);
        const auto                result     = connection->execute("SELECT url, title, params, error_msg, oper_id, ip, result FROM sys_oper_log ORDER BY oper_time, id");
        std::vector<OperAuditRow> rows;
        if (!result)
        {
            return rows;
        }
        while (result->next())
        {
            rows.push_back(OperAuditRow{EdgePulse::Sql::text(*result, "url"), EdgePulse::Sql::text(*result, "title"), EdgePulse::Sql::text(*result, "params"),
                                        EdgePulse::Sql::text(*result, "error_msg"), EdgePulse::Sql::integer(*result, "oper_id"), EdgePulse::Sql::text(*result, "ip"),
                                        static_cast<int>(EdgePulse::Sql::integer(*result, "result"))});
        }
        return rows;
    }

    [[nodiscard]] std::int64_t userIdByUsername(const AuthTestContext &context, const std::string &username)
    {
        const auto connection = openDirect(context);
        const auto result     = connection->execute("SELECT id FROM sys_user WHERE username = '" + username + "' AND deleted = 0");
        if (!result || !result->next())
        {
            return 0;
        }
        return EdgePulse::Sql::integer(*result, "id");
    }

    [[nodiscard]] LoginFailureState readLoginFailureState(const AuthTestContext &context, const std::int64_t userId)
    {
        const auto connection = openDirect(context);
        const auto result     = connection->execute("SELECT failed_login_count, locked_until FROM sys_user WHERE id = " + std::to_string(userId));
        if (!result || !result->next())
        {
            return {};
        }
        return LoginFailureState{static_cast<int>(EdgePulse::Sql::integer(*result, "failed_login_count")), EdgePulse::Sql::text(*result, "locked_until")};
    }

    [[nodiscard]] bool containsAnyAuditSecret(const std::string &text, const MfaActivation &activation)
    {
        if (!activation.secret.empty() && text.find(activation.secret) != std::string::npos)
        {
            return true;
        }
        return std::any_of(activation.recoveryCodes.begin(), activation.recoveryCodes.end(), [&text](const std::string &code) { return text.find(code) != std::string::npos; });
    }

    /**
     * @brief 显式构造一个已确认的用户身份
     */
    [[nodiscard]] SessionRecord sessionFor(const EdgePulse::System::UserRecord &user)
    {
        SessionRecord session;
        session.userId        = user.id;
        session.tenantId      = user.tenantId;
        session.tenantCode    = user.tenantCode;
        session.tenantName    = user.tenantName;
        session.platformAdmin = user.platformAdmin;
        session.username      = user.username;
        session.nickname      = user.nickname;
        session.departmentId  = user.departmentId;
        return session;
    }
} // namespace

class MfaEndpointsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_context = std::make_unique<AuthTestContext>(EdgePulse::Auth::AuthOptions{6, false, 900, 100, 900});
        m_mfa     = std::make_shared<MfaService>(m_context->database, m_context->cache, encodedMfaKey());
        m_context->auth->setMfaService(m_mfa);

        m_users      = std::make_shared<EdgePulse::System::UserRepository>(m_context->database);
        m_roles      = std::make_shared<EdgePulse::System::RoleRepository>(m_context->database);
        m_menus      = std::make_shared<EdgePulse::System::MenuRepository>(m_context->database);
        m_org        = std::make_shared<EdgePulse::System::OrgRepository>(m_context->database);
        m_dictionary = std::make_shared<EdgePulse::System::DictionaryRepository>(m_context->database);
        m_logs       = std::make_shared<EdgePulse::System::LogRepository>(m_context->database);
        m_dataScopes = std::make_shared<EdgePulse::Auth::DataScopeService>(m_context->database);
        m_tenants    = std::make_shared<EdgePulse::System::TenantRepository>(m_context->database);
        EdgePulse::Auth::OidcConfiguration oidcConfiguration;
        oidcConfiguration.enabled    = true;
        oidcConfiguration.tenantCode = "default";
        m_oidc                       = std::make_unique<EdgePulse::Auth::OidcService>(oidcConfiguration, *m_users, *m_tenants, *m_context->cache, *m_context->auth);
        m_server                     = std::make_unique<ServerHarness>(
                [this](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                {
                    EdgePulse::Web::applySecurityHeaders(router);
                    EdgePulse::Web::registerAuditMiddleware(router, loop, *m_context->auth, *m_logs);
                    EdgePulse::Auth::registerRoutes(router, loop, *m_context->auth, *m_context->captcha, true);
                    EdgePulse::Auth::registerMfaRoutes(router, loop, *m_context->auth, *m_mfa, *m_logs);
                    EdgePulse::Auth::registerOidcRoutes(router, loop, *m_oidc);
                    EdgePulse::System::SystemDependencies systemDependencies;
                    systemDependencies.auth         = m_context->auth.get();
                    systemDependencies.dataScopes   = m_dataScopes.get();
                    systemDependencies.users        = m_users.get();
                    systemDependencies.roles        = m_roles.get();
                    systemDependencies.menus        = m_menus.get();
                    systemDependencies.org          = m_org.get();
                    systemDependencies.dictionaries = m_dictionary.get();
                    systemDependencies.tenants      = m_tenants.get();
                    systemDependencies.mfa          = m_mfa.get();
                    EdgePulse::System::registerSystemRoutes(router, loop, systemDependencies);
                    EdgePulse::System::TenantDependencies tenantDependencies;
                    tenantDependencies.auth    = m_context->auth.get();
                    tenantDependencies.tenants = m_tenants.get();
                    EdgePulse::System::registerTenantRoutes(router, loop, tenantDependencies);
                    EdgePulse::Monitor::MonitorDependencies monitorDependencies;
                    monitorDependencies.auth       = m_context->auth.get();
                    monitorDependencies.dataScopes = m_dataScopes.get();
                    monitorDependencies.logs       = m_logs.get();
                    EdgePulse::Monitor::registerMonitorRoutes(router, loop, monitorDependencies);
                });
    }

    void TearDown() override
    {
        m_server.reset();
        m_oidc.reset();
        m_mfa.reset();
        m_context.reset();
    }

    [[nodiscard]] std::optional<CaptchaPair> fetchCaptcha()
    {
        const auto response = m_server->get("/api/auth/captcha");
        const auto data     = responseData(response);
        if (!data.has_value())
        {
            return std::nullopt;
        }
        return CaptchaPair{data->value("uuid", std::string{}), data->value("code", std::string{})};
    }

    [[nodiscard]] HttpResult attemptLogin(const std::string &username, const std::string &password, const std::string &tenantCode = {})
    {
        const std::optional<CaptchaPair> captcha = fetchCaptcha();
        if (!captcha.has_value())
        {
            ADD_FAILURE() << "无法取得验证码";
            return {};
        }
        AsynGyanis::Base::ConfigValue body =
                AsynGyanis::Base::ConfigValue::object({{"username", username}, {"password", password}, {"captcha", captcha->code}, {"captchaUuid", captcha->uuid}});
        if (!tenantCode.empty())
        {
            body["tenantCode"] = tenantCode;
        }
        return m_server->post("/api/auth/login", body.dump(), "application/json");
    }

    [[nodiscard]] std::string loginToken(const std::string &username, const std::string &password, const std::string &tenantCode = {})
    {
        const HttpResult response = attemptLogin(username, password, tenantCode);
        EXPECT_EQ(200, response.status) << response.body;
        const auto data = responseData(response);
        if (!data.has_value())
        {
            return {};
        }
        return data->value("token", std::string{});
    }

    /**
     * @brief 用当前会话与当前口令设置并启用两步验证
     */
    [[nodiscard]] MfaActivation activateMfa(const std::string &token, const std::string &password)
    {
        MfaActivation activation;
        const auto    before = m_server->get("/api/auth/mfa", bearer(token));
        EXPECT_EQ(200, before.status) << before.body;
        const auto beforeData = responseData(before);
        if (!beforeData.has_value())
        {
            ADD_FAILURE() << "状态响应缺少 data";
            return activation;
        }
        EXPECT_FALSE(beforeData->value("enabled", true));

        const AsynGyanis::Base::ConfigValue setupBody = AsynGyanis::Base::ConfigValue::object({{"password", password}});
        const auto                          setup     = m_server->post("/api/auth/mfa/setup", setupBody.dump(), "application/json", bearer(token));
        EXPECT_EQ(200, setup.status) << setup.body;
        EXPECT_EQ(std::string{"no-store"}, setup.header("Cache-Control").value_or(""));
        activation.secret = stringData(setup, "secret");
        if (activation.secret.empty())
        {
            ADD_FAILURE() << "设置响应缺少 secret";
            return activation;
        }

        activation.enableStep = Totp::currentStep();
        const AsynGyanis::Base::ConfigValue enableBody =
                AsynGyanis::Base::ConfigValue::object({{"password", password}, {"code", Totp::code(activation.secret, activation.enableStep)}});
        const auto enabled = m_server->post("/api/auth/mfa/enable", enableBody.dump(), "application/json", bearer(token));
        EXPECT_EQ(200, enabled.status) << enabled.body;
        EXPECT_EQ(std::string{"no-store"}, enabled.header("Cache-Control").value_or(""));
        const auto enabledData = responseData(enabled);
        if (enabledData.has_value())
        {
            const auto iterator = enabledData->find("recoveryCodes");
            if (iterator != enabledData->end() && iterator->is_array())
            {
                for (const AsynGyanis::Base::ConfigValue &value: *iterator)
                {
                    activation.recoveryCodes.push_back(value.get<std::string>());
                }
            }
        }
        EXPECT_EQ(10U, activation.recoveryCodes.size());

        const auto after = m_server->get("/api/auth/mfa", bearer(token));
        EXPECT_EQ(200, after.status) << after.body;
        const auto afterData = responseData(after);
        if (!afterData.has_value())
        {
            ADD_FAILURE() << "状态响应缺少 data";
            return activation;
        }
        EXPECT_TRUE(afterData->value("enabled", false));
        EXPECT_EQ(static_cast<std::int64_t>(activation.recoveryCodes.size()), afterData->value("recoveryCodesRemaining", std::int64_t{0}));
        return activation;
    }

    /**
     * @brief 用口令登录并断言进入了挑战分支
     */
    [[nodiscard]] std::string startChallenge(const std::string &username, const std::string &password, const std::string &tenantCode = {})
    {
        const HttpResult response = attemptLogin(username, password, tenantCode);
        EXPECT_EQ(200, response.status) << response.body;
        const auto data = responseData(response);
        if (!data.has_value())
        {
            return {};
        }
        EXPECT_TRUE(data->value("mfaRequired", false));
        EXPECT_EQ(static_cast<std::int64_t>(MfaService::kChallengeTtlSeconds), data->value("expiresIn", std::int64_t{0}));
        EXPECT_FALSE(data->contains("token"));
        EXPECT_EQ(std::string{"no-store"}, response.header("Cache-Control").value_or(""));
        return data->value("challengeToken", std::string{});
    }

    /**
     * @brief 用 TOTP 完成挑战并返回会话令牌
     */
    [[nodiscard]] HttpResult verifyChallenge(const std::string &challengeToken, const std::string &code)
    {
        const AsynGyanis::Base::ConfigValue body = AsynGyanis::Base::ConfigValue::object({{"challengeToken", challengeToken}, {"code", code}});
        return m_server->post("/api/auth/mfa/verify", body.dump(), "application/json");
    }

    [[nodiscard]] std::string verifyChallengeToken(const std::string &challengeToken, const std::string &code)
    {
        const HttpResult response = verifyChallenge(challengeToken, code);
        EXPECT_EQ(200, response.status) << response.body;
        EXPECT_EQ(std::string{"no-store"}, response.header("Cache-Control").value_or(""));
        const auto data = responseData(response);
        if (!data.has_value())
        {
            return {};
        }
        return data->value("token", std::string{});
    }

    /**
     * @brief 直接写入一份 OIDC 一次性 code，模拟回调已经完成身份校验
     */
    [[nodiscard]] std::string issueOidcCode(EventLoopRunner &runner, const std::int64_t userId, const std::string &returnPath)
    {
        const std::string code   = EdgePulse::Common::randomTokenHex(24);
        const auto        stored = AsynGyanis::Base::ConfigValue::object({{"userId", userId}, {"returnPath", returnPath}});
        EXPECT_TRUE(runner.run(m_context->cache->set(runner.loop(), "oidc:code:" + code, stored.dump(), std::chrono::seconds{120})));
        return code;
    }

    [[nodiscard]] std::int64_t createRole(const std::string &token, const std::string &name, const std::string &code)
    {
        const std::string body     = AsynGyanis::Base::ConfigValue::object(
                                             {{"name", name}, {"code", code}, {"dataScope", "SELF"}, {"sort", 50}, {"status", 1}, {"menuIds", std::vector<std::int64_t>{1300}}})
                                             .dump();
        const auto        response = m_server->post("/api/system/roles", body, "application/json", bearer(token));
        EXPECT_EQ(200, response.status) << response.body;
        const auto data = responseData(response);
        if (!data.has_value())
        {
            return 0;
        }
        return data->value("id", std::int64_t{0});
    }

    [[nodiscard]] std::int64_t createUser(const std::string &token, const std::string &username, const std::string &nickname, const std::int64_t roleId)
    {
        const std::string body     = AsynGyanis::Base::ConfigValue::object({{"username", username},
                                                                            {"password", "userpass1"},
                                                                            {"nickname", nickname},
                                                                            {"deptId", 101},
                                                                            {"status", 1},
                                                                            {"roleIds", std::vector<std::int64_t>{roleId}},
                                                                            {"postIds", std::vector<std::int64_t>{}}})
                                             .dump();
        const auto        response = m_server->post("/api/system/users", body, "application/json", bearer(token));
        EXPECT_EQ(200, response.status) << response.body;
        const auto data = responseData(response);
        if (!data.has_value())
        {
            return 0;
        }
        return data->value("id", std::int64_t{0});
    }

    [[nodiscard]] bool listMfaEnabled(const std::string &token, const std::string &username)
    {
        const auto list = m_server->get("/api/system/users?pageNum=1&pageSize=10&keyword=" + username, bearer(token));
        EXPECT_EQ(200, list.status) << list.body;
        const auto data = responseData(list);
        if (!data.has_value())
        {
            return false;
        }
        const auto rows = data->find("rows");
        if (rows == data->end() || !rows->is_array() || rows->empty())
        {
            ADD_FAILURE() << "用户列表没有目标用户：" << username;
            return false;
        }
        return (*rows)[0].value("mfaEnabled", false);
    }

    std::unique_ptr<AuthTestContext>                         m_context;
    std::shared_ptr<MfaService>                              m_mfa;
    std::shared_ptr<EdgePulse::System::UserRepository>       m_users;
    std::shared_ptr<EdgePulse::System::RoleRepository>       m_roles;
    std::shared_ptr<EdgePulse::System::MenuRepository>       m_menus;
    std::shared_ptr<EdgePulse::System::OrgRepository>        m_org;
    std::shared_ptr<EdgePulse::System::DictionaryRepository> m_dictionary;
    std::shared_ptr<EdgePulse::System::LogRepository>        m_logs;
    std::shared_ptr<EdgePulse::Auth::DataScopeService>       m_dataScopes;
    std::shared_ptr<EdgePulse::System::TenantRepository>     m_tenants;
    std::unique_ptr<EdgePulse::Auth::OidcService>            m_oidc;
    std::unique_ptr<ServerHarness>                           m_server;
};

// 钉住：设置、启用、挑战登录、同一步重放拒绝、下一步通过、令牌可用与审计文案
TEST_F(MfaEndpointsTest, LoginIssuesChallengeAndVerifiesTotp)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());

    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_FALSE(activation.secret.empty());
    ASSERT_EQ(10U, activation.recoveryCodes.size());

    const std::string challenge = startChallenge("admin", "admin123");
    ASSERT_FALSE(challenge.empty());

    // 启用时消费了当前步，同一口令不能直接用于挑战校验
    const HttpResult replay = verifyChallenge(challenge, Totp::code(activation.secret, activation.enableStep));
    EXPECT_EQ(401, replay.status) << replay.body;

    const std::int64_t loginStep = std::max(Totp::currentStep(), activation.enableStep + 1);
    const std::string  mfaToken  = verifyChallengeToken(challenge, Totp::code(activation.secret, loginStep));
    ASSERT_FALSE(mfaToken.empty());

    const auto me = m_server->get("/api/auth/me", bearer(mfaToken));
    ASSERT_EQ(200, me.status) << me.body;
    EXPECT_EQ("admin", stringData(me, "username"));

    // 挑战是一次性凭据，成功后不可重放
    EXPECT_EQ(401, verifyChallenge(challenge, Totp::code(activation.secret, loginStep + 1)).status);

    const std::vector<LoginAuditRow> loginAudits = readLoginAudits(*m_context);
    const std::int64_t               adminId     = userIdByUsername(*m_context, "admin");
    ASSERT_GT(adminId, 0);
    EXPECT_NE(loginAudits.end(), std::find_if(loginAudits.begin(), loginAudits.end(),
                                              [adminId](const LoginAuditRow &row) { return row.userId == adminId && row.message == "登录成功，等待两步验证"; }));
    EXPECT_NE(loginAudits.end(),
              std::find_if(loginAudits.begin(), loginAudits.end(), [adminId](const LoginAuditRow &row) { return row.userId == adminId && row.message == "登录成功（两步验证）"; }));
    for (const LoginAuditRow &row: loginAudits)
    {
        EXPECT_FALSE(containsAnyAuditSecret(row.message, activation)) << row.message;
    }
}

// 钉住：设置与启用都要当前口令，光有会话不能登记新的第二因素
TEST_F(MfaEndpointsTest, SetupAndEnableRequireCurrentPassword)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());

    EXPECT_EQ(400, m_server->post("/api/auth/mfa/setup", "{}", "application/json", bearer(token)).status);
    const auto wrongSetup = AsynGyanis::Base::ConfigValue::object({{"password", "not-the-password"}});
    EXPECT_EQ(401, m_server->post("/api/auth/mfa/setup", wrongSetup.dump(), "application/json", bearer(token)).status);

    const auto rightSetup = AsynGyanis::Base::ConfigValue::object({{"password", "admin123"}});
    const auto setup      = m_server->post("/api/auth/mfa/setup", rightSetup.dump(), "application/json", bearer(token));
    ASSERT_EQ(200, setup.status) << setup.body;
    const std::string secret = stringData(setup, "secret");
    ASSERT_FALSE(secret.empty());

    const std::int64_t enableStep = Totp::currentStep();
    const std::string  enableCode = Totp::code(secret, enableStep);
    const auto         onlyCode   = AsynGyanis::Base::ConfigValue::object({{"code", enableCode}});
    EXPECT_EQ(400, m_server->post("/api/auth/mfa/enable", onlyCode.dump(), "application/json", bearer(token)).status);
    const auto wrongPassword = AsynGyanis::Base::ConfigValue::object({{"password", "not-the-password"}, {"code", enableCode}});
    EXPECT_EQ(401, m_server->post("/api/auth/mfa/enable", wrongPassword.dump(), "application/json", bearer(token)).status);
    // 口令失败发生在验证码校验之前，没有消费待启用秘密的步号
    const auto enableBody = AsynGyanis::Base::ConfigValue::object({{"password", "admin123"}, {"code", enableCode}});
    EXPECT_EQ(200, m_server->post("/api/auth/mfa/enable", enableBody.dump(), "application/json", bearer(token)).status);
}

// 钉住：连续错码 5 次销毁挑战，第 6 次即使口令正确也拒绝
TEST_F(MfaEndpointsTest, FiveWrongCodesDestroyChallenge)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());
    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_FALSE(activation.secret.empty());

    const std::string challenge = startChallenge("admin", "admin123");
    ASSERT_FALSE(challenge.empty());
    const std::string badCode = wrongCode(activation.secret, Totp::currentStep());
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        EXPECT_EQ(401, verifyChallenge(challenge, badCode).status) << "attempt " << attempt;
    }

    const std::int64_t correctStep = std::max(Totp::currentStep(), activation.enableStep + 1);
    EXPECT_EQ(401, verifyChallenge(challenge, Totp::code(activation.secret, correctStep)).status);

    // 第 5 次失败按次数上限留痕，而不是与缓存写入失败混成一句
    const std::vector<LoginAuditRow> audits  = readLoginAudits(*m_context);
    const std::int64_t               adminId = userIdByUsername(*m_context, "admin");
    EXPECT_NE(audits.end(),
              std::find_if(audits.begin(), audits.end(), [adminId](const LoginAuditRow &row) { return row.userId == adminId && row.message == "两步验证失败次数已达上限"; }));

    // 账号本身没有因挑战失败被锁定，重新登录仍可拿到新挑战
    const std::string freshChallenge = startChallenge("admin", "admin123");
    ASSERT_FALSE(freshChallenge.empty());
    // 前一次失败没有消费步号，同一枚正确码仍可用；失败只说明旧挑战已销毁
    EXPECT_FALSE(verifyChallengeToken(freshChallenge, Totp::code(activation.secret, correctStep)).empty());
}

// 钉住：挑战续期写入失败与次数上限分成两条审计文案
TEST_F(MfaEndpointsTest, ChallengeRenewalWriteFailureIsAuditedDistinctly)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());
    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_FALSE(activation.secret.empty());
    const std::int64_t adminId = userIdByUsername(*m_context, "admin");
    ASSERT_GT(adminId, 0);

    EventLoopRunner runner;
    const auto      challenge = runner.run(m_mfa->createChallenge(runner.loop(), adminId, 1, "127.0.0.1"));
    ASSERT_TRUE(challenge.has_value());

    auto failingStore = std::make_shared<FailingChallengeStore>(m_context->cache);
    auto brokenMfa    = std::make_shared<MfaService>(m_context->database, failingStore, encodedMfaKey());
    m_context->auth->setMfaService(brokenMfa);

    const auto rejected = runner.run(m_context->auth->verifyMfa(runner.loop(), *challenge, wrongCode(activation.secret, Totp::currentStep()), "127.0.0.1", "trace-mfa-write"));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(401, rejected.error().httpStatus);

    const std::vector<LoginAuditRow> audits = readLoginAudits(*m_context);
    EXPECT_NE(audits.end(),
              std::find_if(audits.begin(), audits.end(), [adminId](const LoginAuditRow &row) { return row.userId == adminId && row.message == "两步验证失败且挑战续期写入失败"; }));
}

// 钉住：关闭接口连续错码 5 次后，正确验证码也被按用户失败计数挡住
TEST_F(MfaEndpointsTest, DisableFailureLimitBlocksCorrectCode)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());
    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_FALSE(activation.secret.empty());

    const std::string badCode = wrongCode(activation.secret, Totp::currentStep());
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        const auto body = AsynGyanis::Base::ConfigValue::object({{"password", "admin123"}, {"code", badCode}});
        EXPECT_EQ(401, m_server->post("/api/auth/mfa/disable", body.dump(), "application/json", bearer(token)).status) << "attempt " << attempt;
    }

    const std::int64_t correctStep = std::max(Totp::currentStep(), activation.enableStep + 1);
    const auto         correctBody = AsynGyanis::Base::ConfigValue::object({{"password", "admin123"}, {"code", Totp::code(activation.secret, correctStep)}});
    EXPECT_EQ(429, m_server->post("/api/auth/mfa/disable", correctBody.dump(), "application/json", bearer(token)).status);
    EXPECT_TRUE(listMfaEnabled(token, "admin"));
}

// 钉住：换发恢复码连续错码 5 次后，正确验证码也被按用户失败计数挡住
TEST_F(MfaEndpointsTest, RecoveryCodeRegenerationFailureLimitBlocksCorrectCode)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());
    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_FALSE(activation.secret.empty());

    const std::string badCode = wrongCode(activation.secret, Totp::currentStep());
    const auto        badBody = AsynGyanis::Base::ConfigValue::object({{"code", badCode}});
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        EXPECT_EQ(401, m_server->post("/api/auth/mfa/recovery-codes", badBody.dump(), "application/json", bearer(token)).status) << "attempt " << attempt;
    }

    const std::int64_t correctStep = std::max(Totp::currentStep(), activation.enableStep + 1);
    const auto         correctBody = AsynGyanis::Base::ConfigValue::object({{"code", Totp::code(activation.secret, correctStep)}});
    EXPECT_EQ(429, m_server->post("/api/auth/mfa/recovery-codes", correctBody.dump(), "application/json", bearer(token)).status);
}

// 钉住：一次成功的管理操作清空共享失败计数，后续仍保有完整的 5 次机会
TEST_F(MfaEndpointsTest, SuccessfulManagementOperationClearsFailureLimit)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());
    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_FALSE(activation.secret.empty());

    const std::string badCode = wrongCode(activation.secret, Totp::currentStep());
    const auto        badBody = AsynGyanis::Base::ConfigValue::object({{"code", badCode}});
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        EXPECT_EQ(401, m_server->post("/api/auth/mfa/recovery-codes", badBody.dump(), "application/json", bearer(token)).status) << "attempt " << attempt;
    }

    const std::int64_t regenerateStep = std::max(Totp::currentStep(), activation.enableStep + 1);
    const auto         regenerateBody = AsynGyanis::Base::ConfigValue::object({{"code", Totp::code(activation.secret, regenerateStep)}});
    ASSERT_EQ(200, m_server->post("/api/auth/mfa/recovery-codes", regenerateBody.dump(), "application/json", bearer(token)).status);

    // 若成功没有清零，计数会从 3 起算，第二次错码就会撞到上限
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        const auto body = AsynGyanis::Base::ConfigValue::object({{"password", "admin123"}, {"code", badCode}});
        EXPECT_EQ(401, m_server->post("/api/auth/mfa/disable", body.dump(), "application/json", bearer(token)).status) << "attempt " << attempt;
    }
}

// 钉住：已启用时重新设置返回 409，未启用时关闭返回 404
TEST_F(MfaEndpointsTest, SetupWhenEnabledAndDisableWhenMissing)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());

    const auto disableBody = AsynGyanis::Base::ConfigValue::object({{"password", "admin123"}, {"code", "123456"}});
    EXPECT_EQ(404, m_server->post("/api/auth/mfa/disable", disableBody.dump(), "application/json", bearer(token)).status);

    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_FALSE(activation.secret.empty());
    const auto setupBody = AsynGyanis::Base::ConfigValue::object({{"password", "admin123"}});
    EXPECT_EQ(409, m_server->post("/api/auth/mfa/setup", setupBody.dump(), "application/json", bearer(token)).status);
}

// 钉住：挑战绑定的来源 IP 与本次请求不一致时拒绝，且挑战已被消费
TEST_F(MfaEndpointsTest, ChallengeRejectsDifferentClientIp)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());
    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_FALSE(activation.secret.empty());
    const std::int64_t adminId = userIdByUsername(*m_context, "admin");
    ASSERT_GT(adminId, 0);

    EventLoopRunner runner;
    const auto      challenge = runner.run(m_mfa->createChallenge(runner.loop(), adminId, 1, "198.51.100.10"));
    ASSERT_TRUE(challenge.has_value());
    const std::int64_t loginStep = std::max(Totp::currentStep(), activation.enableStep + 1);
    const std::string  code      = Totp::code(activation.secret, loginStep);

    const auto rejected = runner.run(m_context->auth->verifyMfa(runner.loop(), *challenge, code, "198.51.100.11", "trace-mfa-ip"));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(401, rejected.error().httpStatus);

    // 校验入口先 take 再比 IP，挑战不会留给同一令牌的下一次尝试
    const auto retry = runner.run(m_context->auth->verifyMfa(runner.loop(), *challenge, code, "198.51.100.10", "trace-mfa-ip"));
    EXPECT_FALSE(retry.has_value());

    const std::vector<LoginAuditRow> audits = readLoginAudits(*m_context);
    EXPECT_NE(audits.end(),
              std::find_if(audits.begin(), audits.end(), [adminId](const LoginAuditRow &row) { return row.userId == adminId && row.message == "两步验证来源与登录请求不一致"; }));
}

// 钉住：挑战寿命是签发时刻起算的绝对 300 秒，失败重写不延长
TEST_F(MfaEndpointsTest, ExpiredChallengeIsRejected)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());
    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_FALSE(activation.secret.empty());
    const std::int64_t adminId = userIdByUsername(*m_context, "admin");
    ASSERT_GT(adminId, 0);

    EventLoopRunner runner;
    const auto      challenge = runner.run(m_mfa->createChallenge(runner.loop(), adminId, 1, "127.0.0.1"));
    ASSERT_TRUE(challenge.has_value());

    const auto expired =
            AsynGyanis::Base::ConfigValue::object({{"userId", adminId}, {"tenantId", 1}, {"clientIp", "127.0.0.1"}, {"attempts", 0}, {"expiresAt", nowEpochSeconds() - 1}});
    ASSERT_TRUE(runner.run(m_context->cache->set(runner.loop(), "edgepulse:mfa:challenge:" + *challenge, expired.dump(), std::chrono::seconds{300})));
    EXPECT_FALSE(runner.run(m_mfa->takeChallenge(runner.loop(), *challenge)).has_value());

    const std::int64_t loginStep = std::max(Totp::currentStep(), activation.enableStep + 1);
    const auto         rejected  = runner.run(m_context->auth->verifyMfa(runner.loop(), *challenge, Totp::code(activation.secret, loginStep), "127.0.0.1", "trace-mfa-expired"));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(401, rejected.error().httpStatus);
}

// 钉住：同一挑战并发校验只有一方能拿到挑战并签发会话
TEST_F(MfaEndpointsTest, ConcurrentVerifyIssuesExactlyOneSession)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());
    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_FALSE(activation.secret.empty());
    const std::int64_t adminId = userIdByUsername(*m_context, "admin");
    ASSERT_GT(adminId, 0);

    EventLoopRunner runner;
    const auto      challenge = runner.run(m_mfa->createChallenge(runner.loop(), adminId, 1, "127.0.0.1"));
    ASSERT_TRUE(challenge.has_value());
    const std::int64_t loginStep = std::max(Totp::currentStep(), activation.enableStep + 1);
    const std::string  code      = Totp::code(activation.secret, loginStep);

    using Outcome     = std::expected<EdgePulse::Auth::LoginResult, EdgePulse::Auth::AuthError>;
    auto first        = std::make_shared<std::promise<Outcome>>();
    auto second       = std::make_shared<std::promise<Outcome>>();
    auto firstResult  = first->get_future();
    auto secondResult = second->get_future();

    AsynGyanis::Core::Task<> firstTask  = forwardInto(m_context->auth->verifyMfa(runner.loop(), *challenge, code, "127.0.0.1", "trace-mfa-concurrent-1"), first);
    AsynGyanis::Core::Task<> secondTask = forwardInto(m_context->auth->verifyMfa(runner.loop(), *challenge, code, "127.0.0.1", "trace-mfa-concurrent-2"), second);
    runner.loop().scheduler().scheduleRemote(firstTask.handle());
    runner.loop().scheduler().scheduleRemote(secondTask.handle());

    ASSERT_EQ(std::future_status::ready, firstResult.wait_for(std::chrono::seconds{5}));
    ASSERT_EQ(std::future_status::ready, secondResult.wait_for(std::chrono::seconds{5}));
    const Outcome one       = firstResult.get();
    const Outcome two       = secondResult.get();
    const int     successes = (one.has_value() ? 1 : 0) + (two.has_value() ? 1 : 0);
    EXPECT_EQ(1, successes);
    if (one.has_value())
    {
        EXPECT_FALSE(one->token.empty());
    }
    if (two.has_value())
    {
        EXPECT_FALSE(two->token.empty());
    }
}

// 钉住：恢复码可完成一次登录，同一枚第二次提交被拒绝
TEST_F(MfaEndpointsTest, RecoveryCodeWorksOnce)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());
    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_GE(activation.recoveryCodes.size(), 2U);

    const std::string challenge = startChallenge("admin", "admin123");
    ASSERT_FALSE(challenge.empty());
    const std::string recoveryToken = verifyChallengeToken(challenge, activation.recoveryCodes.front());
    ASSERT_FALSE(recoveryToken.empty());

    const auto status = m_server->get("/api/auth/mfa", bearer(recoveryToken));
    ASSERT_EQ(200, status.status) << status.body;
    const auto statusData = responseData(status);
    ASSERT_TRUE(statusData.has_value());
    EXPECT_TRUE(statusData->value("enabled", false));
    EXPECT_EQ(static_cast<std::int64_t>(activation.recoveryCodes.size() - 1), statusData->value("recoveryCodesRemaining", std::int64_t{0}));

    const std::string secondChallenge = startChallenge("admin", "admin123");
    ASSERT_FALSE(secondChallenge.empty());
    EXPECT_EQ(401, verifyChallenge(secondChallenge, activation.recoveryCodes.front()).status);
}

// 钉住：关闭两步验证必须同时给出正确口令与未消费过的 TOTP，关闭后登录不再要求 MFA
TEST_F(MfaEndpointsTest, DisableRequiresPasswordAndFreshTotp)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());
    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_FALSE(activation.secret.empty());

    // 启用时用过的口令不能关闭
    const auto replayBody = AsynGyanis::Base::ConfigValue::object({{"password", "admin123"}, {"code", Totp::code(activation.secret, activation.enableStep)}});
    EXPECT_EQ(401, m_server->post("/api/auth/mfa/disable", replayBody.dump(), "application/json", bearer(token)).status);

    const std::string wrongPasswordCode = Totp::code(activation.secret, std::max(Totp::currentStep(), activation.enableStep + 1));
    const auto        wrongPassword     = AsynGyanis::Base::ConfigValue::object({{"password", "not-the-password"}, {"code", wrongPasswordCode}});
    EXPECT_EQ(401, m_server->post("/api/auth/mfa/disable", wrongPassword.dump(), "application/json", bearer(token)).status);
    EXPECT_TRUE(listMfaEnabled(token, "admin"));
    // 关闭前的口令复核不计入登录失败次数，也不会触发锁定
    const LoginFailureState failureState = readLoginFailureState(*m_context, userIdByUsername(*m_context, "admin"));
    EXPECT_EQ(0, failureState.count);
    EXPECT_TRUE(failureState.lockedUntil.empty());

    // 口令错误时不会消费步号；这里重新按当前时间取码，避免等待期间的步长边界
    const std::int64_t disableStep = std::max(Totp::currentStep(), activation.enableStep + 1);
    const std::string  freshCode   = Totp::code(activation.secret, disableStep);
    const auto         disableBody = AsynGyanis::Base::ConfigValue::object({{"password", "admin123"}, {"code", freshCode}});
    const auto         disabled    = m_server->post("/api/auth/mfa/disable", disableBody.dump(), "application/json", bearer(token));
    ASSERT_EQ(200, disabled.status) << disabled.body;
    EXPECT_EQ(std::string{"no-store"}, disabled.header("Cache-Control").value_or(""));

    const std::int64_t              adminId    = userIdByUsername(*m_context, "admin");
    const std::vector<OperAuditRow> operAudits = readOperAudits(*m_context);
    const auto disableAudit = std::find_if(operAudits.begin(), operAudits.end(),
                                           [adminId](const OperAuditRow &row) { return row.url == "/api/auth/mfa/disable" && row.operId == adminId && row.result == 1; });
    ASSERT_NE(operAudits.end(), disableAudit);
    EXPECT_EQ(std::string{"两步验证 · 关闭"}, disableAudit->title);
    EXPECT_FALSE(disableAudit->ip.empty());

    const HttpResult afterDisable = attemptLogin("admin", "admin123");
    ASSERT_EQ(200, afterDisable.status) << afterDisable.body;
    const auto afterData = responseData(afterDisable);
    ASSERT_TRUE(afterData.has_value());
    EXPECT_FALSE(afterData->value("mfaRequired", false));
    EXPECT_FALSE(afterData->value("token", std::string{}).empty());
}

// 钉住：管理员重置需要 system:user:edit，跨租户目标按 404 处理，列表回显 mfaEnabled，审计不含秘密
TEST_F(MfaEndpointsTest, AdminResetScopesPermissionAndAudit)
{
    const std::string adminToken = loginToken("admin", "admin123");
    ASSERT_FALSE(adminToken.empty());

    const std::int64_t roleId = createRole(adminToken, "MFA 目标", "mfa_target_role");
    ASSERT_GT(roleId, 0);
    const std::int64_t targetId = createUser(adminToken, "mfa-target", "两步验证目标", roleId);
    ASSERT_GT(targetId, 0);
    EXPECT_FALSE(listMfaEnabled(adminToken, "mfa-target"));

    const std::string targetToken = loginToken("mfa-target", "userpass1");
    ASSERT_FALSE(targetToken.empty());
    const MfaActivation activation = activateMfa(targetToken, "userpass1");
    ASSERT_FALSE(activation.secret.empty());
    EXPECT_TRUE(listMfaEnabled(adminToken, "mfa-target"));

    // 低权限账号不能重置他人的两步验证
    const auto denied = m_server->post("/api/system/users/" + std::to_string(targetId) + "/mfa/reset", "{}", "application/json", bearer(targetToken));
    EXPECT_EQ(403, denied.status) << denied.body;

    // 租户管理员看不到其他租户的目标用户
    const auto createdTenant = m_server->post(
            "/api/system/tenants",
            AsynGyanis::Base::ConfigValue::object({{"code", "mfa_acme"}, {"name", "两步验证租户"}, {"adminUsername", "mfa-acme-admin"}, {"adminPassword", "acme-pass-1"}}).dump(),
            "application/json", bearer(adminToken));
    ASSERT_EQ(200, createdTenant.status) << createdTenant.body;
    const std::string tenantToken = loginToken("mfa-acme-admin", "acme-pass-1", "mfa_acme");
    ASSERT_FALSE(tenantToken.empty());
    const auto crossTenant = m_server->post("/api/system/users/" + std::to_string(targetId) + "/mfa/reset", "{}", "application/json", bearer(tenantToken));
    EXPECT_EQ(404, crossTenant.status) << crossTenant.body;

    const auto reset = m_server->post("/api/system/users/" + std::to_string(targetId) + "/mfa/reset", "{}", "application/json", bearer(adminToken));
    ASSERT_EQ(200, reset.status) << reset.body;
    EXPECT_FALSE(listMfaEnabled(adminToken, "mfa-target"));

    const HttpResult afterReset = attemptLogin("mfa-target", "userpass1");
    ASSERT_EQ(200, afterReset.status) << afterReset.body;
    const auto afterData = responseData(afterReset);
    ASSERT_TRUE(afterData.has_value());
    EXPECT_FALSE(afterData->value("mfaRequired", false));
    EXPECT_FALSE(afterData->value("token", std::string{}).empty());

    const std::vector<OperAuditRow> operAudits = readOperAudits(*m_context);
    const std::string               resetUrl   = "/api/system/users/" + std::to_string(targetId) + "/mfa/reset";
    const std::int64_t              adminId    = userIdByUsername(*m_context, "admin");
    const auto resetAudit = std::find_if(operAudits.begin(), operAudits.end(),
                                         [&resetUrl, adminId](const OperAuditRow &row) { return row.url == resetUrl && row.operId == adminId && row.result == 1; });
    ASSERT_NE(operAudits.end(), resetAudit);
    EXPECT_EQ(std::string{"用户管理 · 重置两步验证"}, resetAudit->title);
    EXPECT_EQ(adminId, resetAudit->operId);
    EXPECT_FALSE(resetAudit->ip.empty());

    const std::vector<LoginAuditRow> loginAudits = readLoginAudits(*m_context);
    for (const OperAuditRow &row: operAudits)
    {
        EXPECT_FALSE(containsAnyAuditSecret(row.params, activation)) << row.params;
        EXPECT_FALSE(containsAnyAuditSecret(row.title, activation)) << row.title;
        EXPECT_FALSE(containsAnyAuditSecret(row.errorMessage, activation)) << row.errorMessage;
    }
    for (const LoginAuditRow &row: loginAudits)
    {
        EXPECT_FALSE(containsAnyAuditSecret(row.message, activation)) << row.message;
    }
}

// 钉住：用户可用未消费的 TOTP 自助换发恢复码，旧恢复码随即失效
TEST_F(MfaEndpointsTest, RegeneratesRecoveryCodes)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());
    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_FALSE(activation.secret.empty());

    const std::int64_t codeStep = std::max(Totp::currentStep(), activation.enableStep + 1);
    const auto         body     = AsynGyanis::Base::ConfigValue::object({{"code", Totp::code(activation.secret, codeStep)}});
    const auto         response = m_server->post("/api/auth/mfa/recovery-codes", body.dump(), "application/json", bearer(token));
    ASSERT_EQ(200, response.status) << response.body;
    EXPECT_EQ(std::string{"no-store"}, response.header("Cache-Control").value_or(""));
    const auto data = responseData(response);
    ASSERT_TRUE(data.has_value());
    std::vector<std::string> freshCodes;
    const auto               iterator = data->find("recoveryCodes");
    ASSERT_NE(data->end(), iterator);
    for (const AsynGyanis::Base::ConfigValue &value: *iterator)
    {
        freshCodes.push_back(value.get<std::string>());
    }
    EXPECT_EQ(10U, freshCodes.size());

    const std::string challenge = startChallenge("admin", "admin123");
    ASSERT_FALSE(challenge.empty());
    EXPECT_EQ(401, verifyChallenge(challenge, activation.recoveryCodes.front()).status);
    const std::string freshToken = verifyChallengeToken(challenge, freshCodes.front());
    EXPECT_FALSE(freshToken.empty());
}

// 钉住：设置、启用与换发恢复码都留下带用户与 IP 的操作审计，且不含秘密或验证码
TEST_F(MfaEndpointsTest, ManagementActionsAreAuditedWithoutSecrets)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());
    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_FALSE(activation.secret.empty());
    const std::int64_t adminId = userIdByUsername(*m_context, "admin");
    ASSERT_GT(adminId, 0);

    const std::int64_t regenerateStep = std::max(Totp::currentStep(), activation.enableStep + 1);
    const std::string  regenerateCode = Totp::code(activation.secret, regenerateStep);
    const auto         regenerateBody = AsynGyanis::Base::ConfigValue::object({{"code", regenerateCode}});
    ASSERT_EQ(200, m_server->post("/api/auth/mfa/recovery-codes", regenerateBody.dump(), "application/json", bearer(token)).status);

    const std::vector<OperAuditRow> rows = readOperAudits(*m_context);
    const auto findByUrl = [&rows](const std::string &url) { return std::find_if(rows.begin(), rows.end(), [&url](const OperAuditRow &row) { return row.url == url; }); };

    const auto setupRow = findByUrl("/api/auth/mfa/setup");
    ASSERT_NE(rows.end(), setupRow);
    EXPECT_EQ(std::string{"两步验证 · 开始设置"}, setupRow->title);
    EXPECT_EQ(adminId, setupRow->operId);
    EXPECT_FALSE(setupRow->ip.empty());

    const auto enableRow = findByUrl("/api/auth/mfa/enable");
    ASSERT_NE(rows.end(), enableRow);
    EXPECT_EQ(std::string{"两步验证 · 启用"}, enableRow->title);
    EXPECT_EQ(adminId, enableRow->operId);

    const auto recoveryRow = findByUrl("/api/auth/mfa/recovery-codes");
    ASSERT_NE(rows.end(), recoveryRow);
    EXPECT_EQ(std::string{"两步验证 · 重置恢复码"}, recoveryRow->title);
    EXPECT_EQ(adminId, recoveryRow->operId);

    const std::vector<std::string> oneTimeCodes{regenerateCode, Totp::code(activation.secret, activation.enableStep)};
    for (const OperAuditRow &row: rows)
    {
        EXPECT_FALSE(containsAnyAuditSecret(row.params, activation)) << row.params;
        EXPECT_FALSE(containsAnyAuditSecret(row.title, activation)) << row.title;
        EXPECT_FALSE(containsAnyAuditSecret(row.errorMessage, activation)) << row.errorMessage;
        for (const std::string &code: oneTimeCodes)
        {
            EXPECT_EQ(std::string::npos, row.params.find(code)) << row.params;
            EXPECT_EQ(std::string::npos, row.errorMessage.find(code)) << row.errorMessage;
        }
    }
}

// 钉住：OIDC 一次性 code 兑换时，已启用 MFA 的账号返回挑战形状而不是会话
TEST_F(MfaEndpointsTest, OidcExchangeReturnsMfaChallengeShape)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());
    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_FALSE(activation.secret.empty());
    const std::int64_t adminId = userIdByUsername(*m_context, "admin");
    ASSERT_GT(adminId, 0);

    EventLoopRunner   runner;
    const std::string code     = issueOidcCode(runner, adminId, "/nodes");
    const auto        exchange = AsynGyanis::Base::ConfigValue::object({{"code", code}});
    const HttpResult  response = m_server->post("/api/auth/oidc/exchange", exchange.dump(), "application/json");
    ASSERT_EQ(200, response.status) << response.body;
    EXPECT_EQ(std::string{"no-store"}, response.header("Cache-Control").value_or(""));
    const auto data = responseData(response);
    ASSERT_TRUE(data.has_value());
    EXPECT_TRUE(data->value("mfaRequired", false));
    EXPECT_FALSE(data->contains("token"));
    EXPECT_EQ(std::string{"/nodes"}, data->value("returnPath", std::string{}));
    EXPECT_EQ(static_cast<std::int64_t>(MfaService::kChallengeTtlSeconds), data->value("expiresIn", std::int64_t{0}));
    const std::string challenge = data->value("challengeToken", std::string{});
    ASSERT_FALSE(challenge.empty());

    const std::int64_t verifyStep = std::max(Totp::currentStep(), activation.enableStep + 1);
    EXPECT_FALSE(verifyChallengeToken(challenge, Totp::code(activation.secret, verifyStep)).empty());
}

// 钉住：两步验证挑战签发失败时兑换接口保留 503，而不是折成 401
TEST_F(MfaEndpointsTest, OidcExchangePreservesMfaUnavailableStatus)
{
    const std::string token = loginToken("admin", "admin123");
    ASSERT_FALSE(token.empty());
    const MfaActivation activation = activateMfa(token, "admin123");
    ASSERT_FALSE(activation.secret.empty());
    const std::int64_t adminId = userIdByUsername(*m_context, "admin");
    ASSERT_GT(adminId, 0);

    auto failingStore = std::make_shared<FailingChallengeStore>(m_context->cache);
    auto brokenMfa    = std::make_shared<MfaService>(m_context->database, failingStore, encodedMfaKey());
    m_context->auth->setMfaService(brokenMfa);

    EventLoopRunner   runner;
    const std::string code     = issueOidcCode(runner, adminId, "/nodes");
    const auto        exchange = AsynGyanis::Base::ConfigValue::object({{"code", code}});
    const HttpResult  response = m_server->post("/api/auth/oidc/exchange", exchange.dump(), "application/json");
    EXPECT_EQ(503, response.status) << response.body;
    EXPECT_EQ(std::string::npos, response.body.find("\"code\":401")) << response.body;
}

// 钉住：OIDC 等外部身份签发路径对已启用 MFA 的用户返回挑战而不是会话
TEST(MfaIssuance, ExternalIssuanceReturnsChallenge)
{
    AuthTestContext context(EdgePulse::Auth::AuthOptions{6, false, 900, 100, 900});
    EventLoopRunner runner;
    auto            mfa = std::make_shared<MfaService>(context.database, context.cache, encodedMfaKey());
    context.auth->setMfaService(mfa);

    auto       users = std::make_shared<EdgePulse::System::UserRepository>(context.database, EdgePulse::System::TenantScope{1, false});
    const auto admin = runner.run(users->findByUsername(runner.loop(), "admin"));
    ASSERT_TRUE(admin.has_value());

    const auto setup = runner.run(mfa->beginSetup(runner.loop(), sessionFor(*admin)));
    ASSERT_TRUE(setup.has_value()) << setup.error().message;
    const std::int64_t enableStep = Totp::currentStep();
    const auto         codes      = runner.run(mfa->enable(runner.loop(), sessionFor(*admin), Totp::code(setup->secret, enableStep)));
    ASSERT_TRUE(codes.has_value()) << codes.error().message;

    const auto issued = runner.run(context.auth->issueSessionOrChallengeForUser(runner.loop(), admin->id, "127.0.0.1", "trace-oidc", "OIDC 登录成功"));
    ASSERT_TRUE(issued.has_value()) << issued.error().message;
    EXPECT_TRUE(issued->mfaRequired);
    EXPECT_TRUE(issued->token.empty());
    EXPECT_EQ(MfaService::kChallengeTtlSeconds, issued->challengeExpiresIn);
    ASSERT_FALSE(issued->challengeToken.empty());

    const std::int64_t verifyStep = std::max(Totp::currentStep(), enableStep + 1);
    const auto         verified   = runner.run(context.auth->verifyMfa(runner.loop(), issued->challengeToken, Totp::code(setup->secret, verifyStep), "127.0.0.1", "trace-oidc"));
    ASSERT_TRUE(verified.has_value()) << verified.error().message;
    EXPECT_FALSE(verified->mfaRequired);
    EXPECT_FALSE(verified->token.empty());
}
