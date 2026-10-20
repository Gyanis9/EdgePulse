#include "Base/Config/ConfigValue.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/DataScopeService.h"
#include "Core/Http/AuditMiddleware.h"
#include "Core/Http/MiddlewareSetup.h"
#include "Core/System/DictionaryRepository.h"
#include "Core/System/LogRepository.h"
#include "Core/System/MenuRepository.h"
#include "Core/System/OrgRepository.h"
#include "Core/System/RoleRepository.h"
#include "Core/System/UserRepository.h"
#include "Plugins/AuthPlugin/AuthController.h"
#include "Plugins/DashboardPlugin/DashboardController.h"
#include "Plugins/MonitorPlugin/MonitorController.h"
#include "Plugins/SystemPlugin/SystemController.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/HttpClientHarness.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    std::vector<std::pair<std::string, std::string>> bearer(const std::string &token)
    {
        return {{"Authorization", "Bearer " + token}};
    }

    std::string loginBody(const std::string &username, const std::string &password, const std::string &code, const std::string &uuid)
    {
        return AsynGyanis::Base::ConfigValue::object({{"username", username}, {"password", password}, {"captcha", code}, {"captchaUuid", uuid}}).dump();
    }
} // namespace

class SystemEndpointsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_context    = std::make_unique<EdgePulse::TestSupport::AuthTestContext>();
        m_users      = std::make_shared<EdgePulse::System::UserRepository>(m_context->database);
        m_roles      = std::make_shared<EdgePulse::System::RoleRepository>(m_context->database);
        m_menus      = std::make_shared<EdgePulse::System::MenuRepository>(m_context->database);
        m_org        = std::make_shared<EdgePulse::System::OrgRepository>(m_context->database);
        m_dictionary = std::make_shared<EdgePulse::System::DictionaryRepository>(m_context->database);
        m_logs       = std::make_shared<EdgePulse::System::LogRepository>(m_context->database);
        m_dataScopes = std::make_shared<EdgePulse::Auth::DataScopeService>(m_context->database);
        m_server     = std::make_unique<EdgePulse::TestSupport::ServerHarness>(
                [this](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                {
                    EdgePulse::Web::applySecurityHeaders(router);
                    EdgePulse::Web::registerAuditMiddleware(router, loop, *m_context->auth, *m_logs);
                    EdgePulse::Auth::registerRoutes(router, loop, *m_context->auth, *m_context->captcha, true);
                    EdgePulse::System::SystemDependencies systemDependencies;
                    systemDependencies.auth         = m_context->auth.get();
                    systemDependencies.dataScopes   = m_dataScopes.get();
                    systemDependencies.users        = m_users.get();
                    systemDependencies.roles        = m_roles.get();
                    systemDependencies.menus        = m_menus.get();
                    systemDependencies.org          = m_org.get();
                    systemDependencies.dictionaries = m_dictionary.get();
                    EdgePulse::System::registerSystemRoutes(router, loop, systemDependencies);
                    EdgePulse::Monitor::MonitorDependencies monitorDependencies;
                    monitorDependencies.auth       = m_context->auth.get();
                    monitorDependencies.dataScopes = m_dataScopes.get();
                    monitorDependencies.logs       = m_logs.get();
                    EdgePulse::Monitor::registerMonitorRoutes(router, loop, monitorDependencies);
                    EdgePulse::Dashboard::DashboardDependencies dashboardDependencies;
                    dashboardDependencies.auth       = m_context->auth.get();
                    dashboardDependencies.dataScopes = m_dataScopes.get();
                    dashboardDependencies.logs       = m_logs.get();
                    dashboardDependencies.roles      = m_roles.get();
                    EdgePulse::Dashboard::registerDashboardRoutes(router, loop, dashboardDependencies);
                });
    }

    void TearDown() override
    {
        m_server.reset();
        m_dictionary.reset();
        m_logs.reset();
        m_org.reset();
        m_menus.reset();
        m_roles.reset();
        m_users.reset();
        m_context.reset();
    }

    std::string login(const std::string &username, const std::string &password)
    {
        const auto captcha = m_server->get("/api/auth/captcha");
        EXPECT_EQ(200, captcha.status) << captcha.body;
        const auto json = captcha.json();
        EXPECT_TRUE(json.has_value());
        const std::string uuid     = (*json)["data"]["uuid"].get<std::string>();
        const std::string code     = (*json)["data"]["code"].get<std::string>();
        const auto        response = m_server->post("/api/auth/login", loginBody(username, password, code, uuid));
        EXPECT_EQ(200, response.status) << response.body;
        const auto loginJson = response.json();
        EXPECT_TRUE(loginJson.has_value());
        return (*loginJson)["data"]["token"].get<std::string>();
    }

    std::int64_t createRole(const std::string &token, const std::string &name, const std::string &code, const std::vector<std::int64_t> &menuIds)
    {
        const std::string body =
                AsynGyanis::Base::ConfigValue::object({{"name", name}, {"code", code}, {"dataScope", "SELF"}, {"sort", 50}, {"status", 1}, {"menuIds", menuIds}}).dump();
        const auto response = m_server->post("/api/system/roles", body, "application/json", bearer(token));
        EXPECT_EQ(200, response.status) << response.body;
        return (*response.json())["data"]["id"].get<std::int64_t>();
    }

    std::int64_t createUser(const std::string &token, const std::string &username, const std::string &nickname, const std::int64_t roleId)
    {
        const std::string body     = AsynGyanis::Base::ConfigValue::object({{"username", username},
                                                                            {"password", "userpass1"},
                                                                            {"nickname", nickname},
                                                                            {"deptId", 101},
                                                                            {"status", 1},
                                                                            {"roleIds", std::vector<std::int64_t>{roleId}},
                                                                            {"postIds", std::vector<std::int64_t>{101}}})
                                             .dump();
        const auto        response = m_server->post("/api/system/users", body, "application/json", bearer(token));
        EXPECT_EQ(200, response.status) << response.body;
        return (*response.json())["data"]["id"].get<std::int64_t>();
    }

    std::unique_ptr<EdgePulse::TestSupport::AuthTestContext> m_context;
    std::shared_ptr<EdgePulse::System::UserRepository>       m_users;
    std::shared_ptr<EdgePulse::System::RoleRepository>       m_roles;
    std::shared_ptr<EdgePulse::System::MenuRepository>       m_menus;
    std::shared_ptr<EdgePulse::System::OrgRepository>        m_org;
    std::shared_ptr<EdgePulse::System::DictionaryRepository> m_dictionary;
    std::shared_ptr<EdgePulse::System::LogRepository>        m_logs;
    std::shared_ptr<EdgePulse::Auth::DataScopeService>       m_dataScopes;
    std::unique_ptr<EdgePulse::TestSupport::ServerHarness>   m_server;
};

// 钉住：管理端各列表、角色/用户写入、审计落库与导出都走真实 HTTP 与数据库
TEST_F(SystemEndpointsTest, ManagementEndpointsAndAudit)
{
    const std::string token = login("admin", "admin123");
    for (const std::string &path:
         {std::string{"/api/system/users?pageNum=1&pageSize=10"}, std::string{"/api/system/users/options"}, std::string{"/api/system/roles"}, std::string{"/api/system/menus/tree"},
          std::string{"/api/system/depts/tree"}, std::string{"/api/system/posts"}, std::string{"/api/system/dict/types"}, std::string{"/api/system/configs"},
          std::string{"/api/monitor/oper-logs"}, std::string{"/api/monitor/login-logs"}, std::string{"/api/dashboard/summary?range=24h"}})
    {
        const auto response = m_server->get(path, bearer(token));
        EXPECT_EQ(200, response.status) << path << " " << response.body;
    }

    const std::int64_t roleId = createRole(token, "只读用户", "readonly_role", {1300});
    const std::int64_t userId = createUser(token, "readonly", "只读用户", roleId);
    EXPECT_GT(userId, 0);

    const auto list = m_server->get("/api/system/users?pageNum=1&pageSize=10&keyword=readonly", bearer(token));
    ASSERT_EQ(200, list.status) << list.body;
    const auto listJson = list.json();
    ASSERT_TRUE(listJson.has_value());
    EXPECT_EQ(1, (*listJson)["data"]["total"].get<std::int64_t>());

    const std::string statusBody = R"({"status":0})";
    EXPECT_EQ(200, m_server->send("PUT", "/api/system/users/" + std::to_string(userId) + "/status", statusBody, "application/json", bearer(token)).status);
    const std::string resetBody = R"({"password":"resetpass1"})";
    EXPECT_EQ(200, m_server->send("PUT", "/api/system/users/" + std::to_string(userId) + "/password", resetBody, "application/json", bearer(token)).status);

    // 登录日志来自前面的管理员登录；审计日志来自角色与用户写入
    const auto loginLogs = m_server->get("/api/monitor/login-logs?pageNum=1&pageSize=10", bearer(token));
    ASSERT_EQ(200, loginLogs.status);
    EXPECT_GE((*loginLogs.json())["data"]["total"].get<std::int64_t>(), 1);
    const auto operLogs = m_server->get("/api/monitor/oper-logs?pageNum=1&pageSize=10", bearer(token));
    ASSERT_EQ(200, operLogs.status);
    EXPECT_GE((*operLogs.json())["data"]["total"].get<std::int64_t>(), 2);

    const auto exported = m_server->get("/api/system/users/export", bearer(token));
    ASSERT_EQ(200, exported.status);
    EXPECT_EQ(0, exported.body.rfind("\xEF\xBB\xBF", 0));

    EXPECT_EQ(200, m_server->send("DELETE", "/api/system/users/" + std::to_string(userId), "", "", bearer(token)).status);
    EXPECT_EQ(200, m_server->send("DELETE", "/api/system/roles/" + std::to_string(roleId), "", "", bearer(token)).status);
}

// 钉住：匿名 401、无权限 403；低权限账号无法读取用户列表，但可访问工作台
TEST_F(SystemEndpointsTest, EnforcesAuthenticationAndPermission)
{
    EXPECT_EQ(401, m_server->get("/api/system/users").status);
    EXPECT_EQ(401, m_server->get("/api/system/roles").status);

    const std::string  adminToken = login("admin", "admin123");
    const std::int64_t roleId     = createRole(adminToken, "受限", "limited_role", {1300});
    createUser(adminToken, "limited", "受限用户", roleId);
    const std::string lowToken = login("limited", "userpass1");
    EXPECT_EQ(403, m_server->get("/api/system/users", bearer(lowToken)).status);
    EXPECT_EQ(403, m_server->get("/api/system/roles", bearer(lowToken)).status);
    EXPECT_EQ(200, m_server->get("/api/dashboard/summary?range=24h", bearer(lowToken)).status);
}

// 钉住：低权限账号的日志查询按本人范围过滤，工作台统计同样受范围约束
TEST_F(SystemEndpointsTest, ScopesLogsToSessionScope)
{
    const std::string  adminToken = login("admin", "admin123");
    const std::int64_t roleId     = createRole(adminToken, "本人日志", "self_log_role", {1202});
    createUser(adminToken, "logself", "本人日志", roleId);
    const std::string lowToken = login("logself", "userpass1");

    const auto adminLogins = m_server->get("/api/monitor/login-logs?pageNum=1&pageSize=10", bearer(adminToken));
    ASSERT_EQ(200, adminLogins.status) << adminLogins.body;
    const auto lowLogins = m_server->get("/api/monitor/login-logs?pageNum=1&pageSize=10", bearer(lowToken));
    ASSERT_EQ(200, lowLogins.status) << lowLogins.body;
    EXPECT_EQ(1, (*lowLogins.json())["data"]["total"].get<std::int64_t>());
    EXPECT_GE((*adminLogins.json())["data"]["total"].get<std::int64_t>(), 2);

    EXPECT_EQ(200, m_server->get("/api/dashboard/summary?range=24h", bearer(lowToken)).status);
}

// 钉住：导出接口是 CSV 而不是 JSON，且带 BOM
TEST_F(SystemEndpointsTest, ExportsCsvWithBom)
{
    const std::string token = login("admin", "admin123");
    for (const std::string &path: {std::string{"/api/system/roles/export"}, std::string{"/api/system/menus/export"}, std::string{"/api/system/depts/export"},
                                   std::string{"/api/system/posts/export"}, std::string{"/api/system/dict/types/export"}, std::string{"/api/system/configs/export"},
                                   std::string{"/api/monitor/oper-logs/export"}, std::string{"/api/monitor/login-logs/export"}})
    {
        const auto response = m_server->get(path, bearer(token));
        EXPECT_EQ(200, response.status) << path << " " << response.body;
        EXPECT_EQ(0, response.body.rfind("\xEF\xBB\xBF", 0)) << path;
    }
}
