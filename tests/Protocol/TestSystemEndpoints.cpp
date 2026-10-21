#include "Base/Config/ConfigValue.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/DataScopeService.h"
#include "Core/Http/AuditMiddleware.h"
#include "Core/Http/MiddlewareSetup.h"
#include "Core/System/DictionaryRepository.h"
#include "Core/System/LogRepository.h"
#include "Core/System/MenuRepository.h"
#include "Core/System/OrgRepository.h"
#include "Core/System/TenantRepository.h"
#include "Core/System/RoleRepository.h"
#include "Core/System/UserRepository.h"
#include "Plugins/AuthPlugin/AuthController.h"
#include "Plugins/DashboardPlugin/DashboardController.h"
#include "Plugins/MonitorPlugin/MonitorController.h"
#include "Plugins/SystemPlugin/SystemController.h"
#include "Plugins/SystemPlugin/TenantController.h"
#include "Plugins/SystemPlugin/NotificationController.h"
#include "Core/Notification/NotificationRepository.h"
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
} // namespace

class SystemEndpointsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_context       = std::make_unique<EdgePulse::TestSupport::AuthTestContext>();
        m_users         = std::make_shared<EdgePulse::System::UserRepository>(m_context->database);
        m_roles         = std::make_shared<EdgePulse::System::RoleRepository>(m_context->database);
        m_menus         = std::make_shared<EdgePulse::System::MenuRepository>(m_context->database);
        m_org           = std::make_shared<EdgePulse::System::OrgRepository>(m_context->database);
        m_dictionary    = std::make_shared<EdgePulse::System::DictionaryRepository>(m_context->database);
        m_logs          = std::make_shared<EdgePulse::System::LogRepository>(m_context->database);
        m_dataScopes    = std::make_shared<EdgePulse::Auth::DataScopeService>(m_context->database);
        m_tenants       = std::make_shared<EdgePulse::System::TenantRepository>(m_context->database);
        m_notifications = std::make_shared<EdgePulse::Notification::NotificationRepository>(m_context->database);
        m_server        = std::make_unique<EdgePulse::TestSupport::ServerHarness>(
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
                    systemDependencies.tenants      = m_tenants.get();
                    EdgePulse::System::registerSystemRoutes(router, loop, systemDependencies);
                    EdgePulse::System::TenantDependencies tenantDependencies;
                    tenantDependencies.auth    = m_context->auth.get();
                    tenantDependencies.tenants = m_tenants.get();
                    EdgePulse::System::registerTenantRoutes(router, loop, tenantDependencies);
                    EdgePulse::Notification::NotificationDependencies notificationDependencies;
                    notificationDependencies.auth     = m_context->auth.get();
                    notificationDependencies.channels = m_notifications.get();
                    notificationDependencies.tenants  = m_tenants.get();
                    EdgePulse::Notification::registerNotificationRoutes(router, loop, notificationDependencies);
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
        return loginInTenant(username, password, "");
    }

    std::string loginInTenant(const std::string &username, const std::string &password, const std::string &tenantCode)
    {
        const auto captcha = m_server->get("/api/auth/captcha");
        EXPECT_EQ(200, captcha.status) << captcha.body;
        const auto json = captcha.json();
        EXPECT_TRUE(json.has_value());
        const std::string             uuid      = (*json)["data"]["uuid"].get<std::string>();
        const std::string             code      = (*json)["data"]["code"].get<std::string>();
        AsynGyanis::Base::ConfigValue bodyValue = AsynGyanis::Base::ConfigValue::object({{"username", username}, {"password", password}, {"captcha", code}, {"captchaUuid", uuid}});
        if (!tenantCode.empty())
        {
            bodyValue["tenantCode"] = tenantCode;
        }
        const auto response = m_server->post("/api/auth/login", bodyValue.dump());
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

    std::unique_ptr<EdgePulse::TestSupport::AuthTestContext>         m_context;
    std::shared_ptr<EdgePulse::System::UserRepository>               m_users;
    std::shared_ptr<EdgePulse::System::RoleRepository>               m_roles;
    std::shared_ptr<EdgePulse::System::MenuRepository>               m_menus;
    std::shared_ptr<EdgePulse::System::OrgRepository>                m_org;
    std::shared_ptr<EdgePulse::System::DictionaryRepository>         m_dictionary;
    std::shared_ptr<EdgePulse::System::LogRepository>                m_logs;
    std::shared_ptr<EdgePulse::Auth::DataScopeService>               m_dataScopes;
    std::shared_ptr<EdgePulse::System::TenantRepository>             m_tenants;
    std::shared_ptr<EdgePulse::Notification::NotificationRepository> m_notifications;
    std::unique_ptr<EdgePulse::TestSupport::ServerHarness>           m_server;
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

// 钉住：CUSTOM 数据范围的部门可写可读，空部门集合被拒绝
TEST_F(SystemEndpointsTest, RoleCustomScopePersistsDepartments)
{
    const std::string token      = login("admin", "admin123");
    const std::string createBody = AsynGyanis::Base::ConfigValue::object({{"name", "自定义部门"},
                                                                          {"code", "custom_depts"},
                                                                          {"dataScope", "CUSTOM"},
                                                                          {"deptIds", std::vector<std::int64_t>{101, 102}},
                                                                          {"menuIds", std::vector<std::int64_t>{1300}}})
                                           .dump();
    const auto        created    = m_server->post("/api/system/roles", createBody, "application/json", bearer(token));
    ASSERT_EQ(200, created.status) << created.body;
    const std::int64_t roleId = (*created.json())["data"]["id"].get<std::int64_t>();

    const std::string path   = "/api/system/roles/" + std::to_string(roleId) + "/depts";
    const auto        before = m_server->get(path, bearer(token));
    ASSERT_EQ(200, before.status) << before.body;
    ASSERT_EQ(2, (*before.json())["data"]["deptIds"].size());

    const std::string updateBody = AsynGyanis::Base::ConfigValue::object({{"dataScope", "CUSTOM"}, {"deptIds", std::vector<std::int64_t>{102}}}).dump();
    ASSERT_EQ(200, m_server->send("PUT", "/api/system/roles/" + std::to_string(roleId), updateBody, "application/json", bearer(token)).status);
    const auto after = m_server->get(path, bearer(token));
    ASSERT_EQ(200, after.status) << after.body;
    ASSERT_EQ(1, (*after.json())["data"]["deptIds"].size());
    EXPECT_EQ(102, (*after.json())["data"]["deptIds"][0].get<std::int64_t>());

    const std::string emptyBody =
            AsynGyanis::Base::ConfigValue::object({{"name", "空范围"}, {"code", "empty_custom"}, {"dataScope", "CUSTOM"}, {"deptIds", std::vector<std::int64_t>{}}}).dump();
    EXPECT_EQ(400, m_server->post("/api/system/roles", emptyBody, "application/json", bearer(token)).status);
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

// 钉住：平台管理员建租户后，租户管理员只看本租户账号；租户管理只对平台管理员开放；停用租户阻断登录
TEST_F(SystemEndpointsTest, TenantAdminIsIsolatedAndTenantLifecycle)
{
    const std::string platformToken    = login("admin", "admin123");
    const std::string createTenantBody = AsynGyanis::Base::ConfigValue::object({{"code", "acme"},
                                                                                {"name", "示范租户"},
                                                                                {"remark", "协议用例"},
                                                                                {"adminUsername", "acme-admin"},
                                                                                {"adminNickname", "示范管理员"},
                                                                                {"adminPassword", "acme-pass-1"}})
                                                 .dump();
    const auto        createdTenant    = m_server->post("/api/system/tenants", createTenantBody, "application/json", bearer(platformToken));
    ASSERT_EQ(200, createdTenant.status) << createdTenant.body;
    const auto createdJson = createdTenant.json();
    ASSERT_TRUE(createdJson.has_value());
    const std::int64_t tenantId = (*createdJson)["data"]["tenantId"].get<std::int64_t>();
    EXPECT_GT(tenantId, 1);

    const auto tenantList = m_server->get("/api/system/tenants?pageNum=1&pageSize=10&keyword=acme", bearer(platformToken));
    ASSERT_EQ(200, tenantList.status) << tenantList.body;
    EXPECT_EQ(1, (*tenantList.json())["data"]["total"].get<std::int64_t>());

    const std::string tenantToken = loginInTenant("acme-admin", "acme-pass-1", "acme");
    const auto        me          = m_server->get("/api/auth/me", bearer(tenantToken));
    ASSERT_EQ(200, me.status) << me.body;
    EXPECT_EQ(tenantId, (*me.json())["data"]["tenantId"].get<std::int64_t>());
    EXPECT_EQ("acme", (*me.json())["data"]["tenantCode"].get<std::string>());
    EXPECT_FALSE((*me.json())["data"]["platformAdmin"].get<bool>());

    // 租户管理员只看到自己；平台管理员既能跨租户看到，也能看到默认租户
    const auto ownUsers = m_server->get("/api/system/users?pageNum=1&pageSize=10&keyword=acme", bearer(tenantToken));
    ASSERT_EQ(200, ownUsers.status) << ownUsers.body;
    EXPECT_EQ(1, (*ownUsers.json())["data"]["total"].get<std::int64_t>());
    const auto defaultAdmin = m_server->get("/api/system/users?pageNum=1&pageSize=10&keyword=admin", bearer(tenantToken));
    ASSERT_EQ(200, defaultAdmin.status) << defaultAdmin.body;
    // 关键词会命中租户自己的 acme-admin，但平台内置 admin 不应出现在结果里
    EXPECT_EQ(1, (*defaultAdmin.json())["data"]["total"].get<std::int64_t>());
    EXPECT_EQ("acme", (*defaultAdmin.json())["data"]["rows"][0]["tenantCode"].get<std::string>());
    const auto platformUsers = m_server->get("/api/system/users?pageNum=1&pageSize=10&keyword=acme", bearer(platformToken));
    ASSERT_EQ(200, platformUsers.status) << platformUsers.body;
    EXPECT_EQ(1, (*platformUsers.json())["data"]["total"].get<std::int64_t>());
    EXPECT_EQ("acme", (*platformUsers.json())["data"]["rows"][0]["tenantCode"].get<std::string>());

    // 租户管理员不得管理租户
    EXPECT_EQ(403, m_server->post("/api/system/tenants", createTenantBody, "application/json", bearer(tenantToken)).status);

    // 租户管理员建的用户落在本租户；平台管理员按关键字能看到，默认租户管理员看不到
    const auto tenantRoles = m_server->get("/api/system/roles?pageNum=1&pageSize=10", bearer(tenantToken));
    ASSERT_EQ(200, tenantRoles.status) << tenantRoles.body;
    const auto tenantRoleRows = (*tenantRoles.json())["data"]["rows"];
    ASSERT_FALSE(tenantRoleRows.empty());
    const std::int64_t tenantRoleId = tenantRoleRows[0]["id"].get<std::int64_t>();
    const auto         tenantDepts  = m_server->get("/api/system/depts/tree", bearer(tenantToken));
    ASSERT_EQ(200, tenantDepts.status) << tenantDepts.body;
    const auto tenantDeptRows = (*tenantDepts.json())["data"];
    ASSERT_FALSE(tenantDeptRows.empty());
    const std::int64_t tenantDeptId = tenantDeptRows[0]["id"].get<std::int64_t>();
    const auto         tenantUser   = m_server->post("/api/system/users",
                                                     AsynGyanis::Base::ConfigValue::object({{"username", "acme-ops"},
                                                                                            {"password", "userpass1"},
                                                                                            {"nickname", "租户运维"},
                                                                                            {"deptId", tenantDeptId},
                                                                                            {"status", 1},
                                                                                            {"roleIds", std::vector<std::int64_t>{tenantRoleId}},
                                                                                            {"postIds", std::vector<std::int64_t>{}}})
                                                             .dump(),
                                                     "application/json", bearer(tenantToken));
    ASSERT_EQ(200, tenantUser.status) << tenantUser.body;
    const std::int64_t tenantUserId = (*tenantUser.json())["data"]["id"].get<std::int64_t>();
    EXPECT_GT(tenantUserId, 0);
    const std::int64_t defaultAdminRole = 3;
    static_cast<void>(createUser(platformToken, "default-ops", "默认运维", defaultAdminRole));
    const std::string defaultToken = login("default-ops", "userpass1");
    const auto        leaked       = m_server->get("/api/system/users?pageNum=1&pageSize=10&keyword=acme-ops", bearer(defaultToken));
    ASSERT_EQ(200, leaked.status) << leaked.body;
    EXPECT_EQ(0, (*leaked.json())["data"]["total"].get<std::int64_t>());
    const auto visible = m_server->get("/api/system/users?pageNum=1&pageSize=10&keyword=acme-ops", bearer(platformToken));
    ASSERT_EQ(200, visible.status) << visible.body;
    EXPECT_EQ(1, (*visible.json())["data"]["total"].get<std::int64_t>());
    EXPECT_EQ("acme", (*visible.json())["data"]["rows"][0]["tenantCode"].get<std::string>());

    // 停用租户后新登录被拒绝
    const std::string disableBody = R"({"status":0})";
    ASSERT_EQ(200, m_server->send("PUT", "/api/system/tenants/" + std::to_string(tenantId), disableBody, "application/json", bearer(platformToken)).status);
    const auto captcha = m_server->get("/api/auth/captcha");
    ASSERT_EQ(200, captcha.status);
    const auto        captchaJson       = captcha.json();
    const std::string disabledLoginBody = AsynGyanis::Base::ConfigValue::object({{"username", "acme-admin"},
                                                                                 {"password", "acme-pass-1"},
                                                                                 {"captcha", (*captchaJson)["data"]["code"].get<std::string>()},
                                                                                 {"captchaUuid", (*captchaJson)["data"]["uuid"].get<std::string>()},
                                                                                 {"tenantCode", "acme"}})
                                                  .dump();
    EXPECT_EQ(401, m_server->post("/api/auth/login", disabledLoginBody).status);
}

// 钉住：通知渠道可增删改查，密钥只回报是否已配置，编辑不传密钥时保留原值
TEST_F(SystemEndpointsTest, NotificationChannelCrudMasksSecret)
{
    const std::string token = login("admin", "admin123");
    const std::string createBody =
            AsynGyanis::Base::ConfigValue::object(
                    {{"name", "值班群"}, {"type", "webhook"}, {"endpoint", "https://example.com/hook"}, {"secret", "hook-secret"}, {"minLevel", "warning"}, {"enabled", true}})
                    .dump();
    const auto created = m_server->post("/api/system/notifications", createBody, "application/json", bearer(token));
    ASSERT_EQ(200, created.status) << created.body;
    const std::int64_t id = (*created.json())["data"]["id"].get<std::int64_t>();
    EXPECT_GT(id, 0);

    const auto list = m_server->get("/api/system/notifications?pageNum=1&pageSize=10&keyword=值班群", bearer(token));
    ASSERT_EQ(200, list.status) << list.body;
    const auto listJson = list.json();
    ASSERT_TRUE(listJson.has_value());
    ASSERT_EQ(1, (*listJson)["data"]["total"].get<std::int64_t>());
    const auto &row = (*listJson)["data"]["rows"][0];
    EXPECT_FALSE(row.contains("secret"));
    EXPECT_TRUE(row["secretConfigured"].get<bool>());
    EXPECT_EQ("warning", row["minLevel"].get<std::string>());

    const auto injected = m_server->post("/api/system/notifications",
                                         R"({"name":"换行注入","type":"email","endpoint":"smtp://127.0.0.1:25","recipients":"ops@example.com
Bcc:evil@example.com","sender":"edgepulse@example.com","minLevel":"info","enabled":true})",
                                         "application/json", bearer(token));
    EXPECT_EQ(400, injected.status) << injected.body;

    const std::string updateBody = R"({"name":"值班群-改","type":"webhook","endpoint":"https://example.com/hook","minLevel":"critical","enabled":true})";
    EXPECT_EQ(200, m_server->send("PUT", "/api/system/notifications/" + std::to_string(id), updateBody, "application/json", bearer(token)).status);
    const auto after = m_server->get("/api/system/notifications?pageNum=1&pageSize=10&keyword=值班群-改", bearer(token));
    ASSERT_EQ(200, after.status) << after.body;
    EXPECT_TRUE((*after.json())["data"]["rows"][0]["secretConfigured"].get<bool>());

    EXPECT_EQ(200, m_server->send("DELETE", "/api/system/notifications/" + std::to_string(id), "", "", bearer(token)).status);
    const auto empty = m_server->get("/api/system/notifications?pageNum=1&pageSize=10", bearer(token));
    ASSERT_EQ(200, empty.status);
    EXPECT_EQ(0, (*empty.json())["data"]["total"].get<std::int64_t>());
}

// 钉住：租户可自建角色、部门、岗位与字典；跨租户不可见，且不能授予自己没有的菜单权限
TEST_F(SystemEndpointsTest, TenantScopedRbacAndOrganization)
{
    const std::string platformToken = login("admin", "admin123");
    const auto        createdTenant = m_server->post(
            "/api/system/tenants",
            AsynGyanis::Base::ConfigValue::object({{"code", "rbac_b"}, {"name", "权限租户"}, {"adminUsername", "rbac-admin"}, {"adminPassword", "rbac-pass-1"}}).dump(),
            "application/json", bearer(platformToken));
    ASSERT_EQ(200, createdTenant.status) << createdTenant.body;
    const std::string tenantToken = loginInTenant("rbac-admin", "rbac-pass-1", "rbac_b");

    const auto rolesBefore = m_server->get("/api/system/roles?pageNum=1&pageSize=10", bearer(tenantToken));
    ASSERT_EQ(200, rolesBefore.status) << rolesBefore.body;
    ASSERT_EQ(1, (*rolesBefore.json())["data"]["total"].get<std::int64_t>());
    const std::int64_t ownRoleId = (*rolesBefore.json())["data"]["rows"][0]["id"].get<std::int64_t>();
    EXPECT_EQ(200, m_server->get("/api/system/roles/" + std::to_string(ownRoleId) + "/menus", bearer(tenantToken)).status);
    EXPECT_EQ(404, m_server->get("/api/system/roles/2/menus", bearer(tenantToken)).status);

    const auto role = m_server->post(
            "/api/system/roles",
            AsynGyanis::Base::ConfigValue::object(
                    {{"name", "租户只读"}, {"code", "rbac_readonly"}, {"dataScope", "SELF"}, {"sort", 10}, {"status", 1}, {"menuIds", std::vector<std::int64_t>{1300}}})
                    .dump(),
            "application/json", bearer(tenantToken));
    ASSERT_EQ(200, role.status) << role.body;
    const auto escalated = m_server->post(
            "/api/system/roles",
            AsynGyanis::Base::ConfigValue::object({{"name", "越权"}, {"code", "rbac_escalate"}, {"dataScope", "SELF"}, {"status", 1}, {"menuIds", std::vector<std::int64_t>{1103}}})
                    .dump(),
            "application/json", bearer(tenantToken));
    EXPECT_EQ(403, escalated.status) << escalated.body;

    // 平台默认租户的部门、岗位与字典不出现在租户视图里
    const auto tenantDepts = m_server->get("/api/system/depts/tree?keyword=研发中心", bearer(tenantToken));
    ASSERT_EQ(200, tenantDepts.status) << tenantDepts.body;
    EXPECT_EQ(0U, (*tenantDepts.json())["data"].size());
    const auto dept = m_server->post("/api/system/depts", R"({"parentId":0,"name":"租户研发","sort":1,"status":1})", "application/json", bearer(tenantToken));
    ASSERT_EQ(200, dept.status) << dept.body;

    const auto platformPost =
            m_server->post("/api/system/posts", R"({"postCode":"platform_post","postName":"平台岗位","sort":10,"status":1})", "application/json", bearer(platformToken));
    ASSERT_EQ(200, platformPost.status) << platformPost.body;
    const auto hiddenPost = m_server->get("/api/system/posts?pageNum=1&pageSize=10&keyword=platform_post", bearer(tenantToken));
    ASSERT_EQ(200, hiddenPost.status) << hiddenPost.body;
    EXPECT_EQ(0, (*hiddenPost.json())["data"]["total"].get<std::int64_t>());
    const auto post = m_server->post("/api/system/posts", R"({"postCode":"tenant_ops","postName":"租户运维","sort":10,"status":1})", "application/json", bearer(tenantToken));
    ASSERT_EQ(200, post.status) << post.body;

    const auto platformDict =
            m_server->post("/api/system/dict/types", R"({"dictName":"平台字典","dictType":"platform_custom","status":1})", "application/json", bearer(platformToken));
    ASSERT_EQ(200, platformDict.status) << platformDict.body;
    const auto hiddenDict = m_server->get("/api/system/dict/types?pageNum=1&pageSize=10&keyword=platform_custom", bearer(tenantToken));
    ASSERT_EQ(200, hiddenDict.status) << hiddenDict.body;
    EXPECT_EQ(0, (*hiddenDict.json())["data"]["total"].get<std::int64_t>());
    const auto dictType = m_server->post("/api/system/dict/types", R"({"dictName":"租户字典","dictType":"tenant_custom","status":1})", "application/json", bearer(tenantToken));
    ASSERT_EQ(200, dictType.status) << dictType.body;
    const auto tenantDict = m_server->get("/api/system/dict/types?pageNum=1&pageSize=10&keyword=tenant_custom", bearer(tenantToken));
    ASSERT_EQ(200, tenantDict.status) << tenantDict.body;
    EXPECT_EQ(1, (*tenantDict.json())["data"]["total"].get<std::int64_t>());
}
