#include "Base/Runtime/TimeFormat.h"
#include "Base/Security/PasswordHash.h"
#include "Core/Alarm/AlarmRepository.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/DataScopeService.h"
#include "Core/Auth/SessionStore.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/NodeGroupRepository.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Firmware/FirmwareRepository.h"
#include "Core/Http/MiddlewareSetup.h"
#include "Core/System/DictionaryRepository.h"
#include "Core/System/LogRepository.h"
#include "Core/System/MenuRepository.h"
#include "Core/System/OrgRepository.h"
#include "Core/System/RoleRepository.h"
#include "Core/System/TenantRepository.h"
#include "Core/System/UserRepository.h"
#include "Core/Telemetry/TelemetryRepository.h"
#include "Plugins/SystemPlugin/SystemController.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"
#include "TestSupport/HttpClientHarness.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

namespace
{
    using EdgePulse::System::TenantScope;

    [[nodiscard]] std::int64_t provisionTenantB(EdgePulse::TestSupport::AuthTestContext &context, EdgePulse::TestSupport::EventLoopRunner &runner)
    {
        EdgePulse::System::TenantProvisionRequest request;
        request.code              = "acme";
        request.name              = "示范租户";
        request.remark            = "隔离用例";
        request.adminUsername     = "acme-admin";
        request.adminNickname     = "示范管理员";
        request.adminPasswordHash = EdgePulse::Common::hashPassword("acme-pass-1");
        const auto created        = runner.run(context.tenants->provision(runner.loop(), std::move(request), 1));
        if (!created.has_value())
        {
            return 0;
        }
        return created->tenantId;
    }

    [[nodiscard]] EdgePulse::Device::NodeRecord createNode(EdgePulse::TestSupport::EventLoopRunner &runner, const std::shared_ptr<EdgePulse::Device::NodeRepository> &repository,
                                                           const std::string &name)
    {
        return runner.run(repository->create(runner.loop(), name, "{}"));
    }
} // namespace

// 钉住：租户编码参与登录定位，会话携带归属；默认租户看不到别家账号，停用租户拒绝登录
TEST(TenantIsolation, LoginBindsTenantAndRejectsDisabledTenant)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const std::int64_t                      tenantId = provisionTenantB(context, runner);
    ASSERT_GT(tenantId, 0);

    const auto challenge = runner.run(context.captcha->issue(runner.loop(), "127.0.0.1"));
    ASSERT_TRUE(challenge.has_value());
    EdgePulse::Auth::LoginCredentials credentials{"acme-admin", "acme-pass-1", challenge->code, challenge->uuid, "127.0.0.1", "tenant-login"};
    credentials.tenantCode = "acme";
    const auto loggedIn    = runner.run(context.auth->login(runner.loop(), credentials));
    ASSERT_TRUE(loggedIn.has_value()) << loggedIn.error().message;
    const auto session = runner.run(context.sessions->validateAndTouch(runner.loop(), loggedIn->token));
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(tenantId, session->tenantId);
    EXPECT_EQ("acme", session->tenantCode);
    EXPECT_FALSE(session->platformAdmin);

    // 同名账号在默认租户不存在：用户名全局唯一不等于跨租户可见
    const auto secondChallenge = runner.run(context.captcha->issue(runner.loop(), "127.0.0.1"));
    ASSERT_TRUE(secondChallenge.has_value());
    EdgePulse::Auth::LoginCredentials wrongTenant{"acme-admin", "acme-pass-1", secondChallenge->code, secondChallenge->uuid, "127.0.0.1", "tenant-login"};
    EXPECT_FALSE(runner.run(context.auth->login(runner.loop(), wrongTenant)).has_value());

    // 停用租户后即使口令正确也拒绝
    ASSERT_TRUE(runner.run(context.tenants->update(runner.loop(), tenantId, std::string{}, 0, std::string{})));
    const auto thirdChallenge = runner.run(context.captcha->issue(runner.loop(), "127.0.0.1"));
    ASSERT_TRUE(thirdChallenge.has_value());
    EdgePulse::Auth::LoginCredentials disabled{"acme-admin", "acme-pass-1", thirdChallenge->code, thirdChallenge->uuid, "127.0.0.1", "tenant-login"};
    disabled.tenantCode = "acme";
    const auto rejected = runner.run(context.auth->login(runner.loop(), disabled));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(401, rejected.error().httpStatus);
    // 已签发会话也不能继续用
    EXPECT_FALSE(runner.run(context.auth->authenticate(runner.loop(), loggedIn->token)).has_value());
}

// 钉住：用户仓储按租户过滤，平台视图可跨租户
TEST(TenantIsolation, UsersAreScopedAndPlatformViewSeesAll)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const std::int64_t                      tenantId = provisionTenantB(context, runner);
    ASSERT_GT(tenantId, 0);

    EdgePulse::System::UserRepository users(context.database);
    EdgePulse::System::UserQuery      query;
    query.keyword      = "acme";
    const auto ownPage = runner.run(users.list(runner.loop(), query, {1, 10}));
    const auto allPage = runner.run(users.scoped(TenantScope{0, true})->list(runner.loop(), query, {1, 10}));
    EXPECT_EQ(0, ownPage.total); // 默认租户视图看不到 acme 管理员
    EXPECT_EQ(1, allPage.total);
    EXPECT_EQ(1, runner.run(users.scoped(TenantScope{tenantId, false})->list(runner.loop(), query, {1, 10})).total);
    const auto acmeUser = runner.run(users.scoped(TenantScope{tenantId, false})->findByUsername(runner.loop(), "acme-admin"));
    ASSERT_TRUE(acmeUser.has_value());
    EXPECT_EQ(tenantId, acmeUser->tenantId);
    EXPECT_EQ("acme", acmeUser->tenantCode);
    EXPECT_FALSE(acmeUser->platformAdmin);

    // 跨租户按标识查不到
    EXPECT_FALSE(runner.run(users.findById(runner.loop(), acmeUser->id)).has_value());
    EXPECT_FALSE(runner.run(users.scoped(TenantScope{1, false})->findByUsername(runner.loop(), "acme-admin")).has_value());
}

// 钉住：节点、节点分组、遥测、命令、固件按租户隔离；子记录继承目标节点/固件的租户
TEST(TenantIsolation, FleetDataIsIsolatedByTenant)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const std::int64_t                      tenantId = provisionTenantB(context, runner);
    ASSERT_GT(tenantId, 0);

    const auto defaultNodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    const auto tenantNodes  = defaultNodes->scoped(TenantScope{tenantId, false});
    const auto nodeA        = createNode(runner, defaultNodes, "default-node");
    const auto nodeB        = createNode(runner, tenantNodes, "acme-node");
    EXPECT_EQ(tenantId, nodeB.tenantId);
    EXPECT_EQ(1, runner.run(defaultNodes->list(runner.loop(), {}, "", "", 0)).total);
    EXPECT_EQ(1, runner.run(tenantNodes->list(runner.loop(), {}, "", "", 0)).total);
    EXPECT_EQ(2, runner.run(defaultNodes->scoped(TenantScope{0, true})->list(runner.loop(), {}, "", "", 0)).total);
    EXPECT_FALSE(runner.run(defaultNodes->findById(runner.loop(), nodeB.id)).has_value());

    const auto groups       = std::make_shared<EdgePulse::Device::NodeGroupRepository>(context.database);
    const auto tenantGroups = groups->scoped(TenantScope{tenantId, false});
    const auto groupB       = runner.run(tenantGroups->create(runner.loop(), 0, "租户分组", 0, std::string{}, 1));
    EXPECT_EQ(1U, runner.run(groups->tree(runner.loop())).size());
    EXPECT_EQ(1U, runner.run(tenantGroups->tree(runner.loop())).size());
    EXPECT_FALSE(runner.run(groups->exists(runner.loop(), groupB)));
    EXPECT_TRUE(runner.run(tenantGroups->exists(runner.loop(), groupB)));
    EXPECT_EQ(2U, runner.run(groups->scoped(TenantScope{0, true})->tree(runner.loop())).size());

    EdgePulse::Telemetry::TelemetrySample cpuSample;
    cpuSample.metric = "cpu.usage";
    cpuSample.value  = 42;
    cpuSample.unit   = "%";
    auto telemetry   = std::make_shared<EdgePulse::Telemetry::TelemetryRepository>(context.database);
    ASSERT_TRUE(runner.run(telemetry->insertBatch(runner.loop(), nodeA.id, {cpuSample})));
    ASSERT_TRUE(runner.run(telemetry->scoped(TenantScope{tenantId, false})->insertBatch(runner.loop(), nodeB.id, {cpuSample})));
    EXPECT_EQ(1U, runner.run(telemetry->latest(runner.loop(), nodeA.id)).size());
    EXPECT_TRUE(runner.run(telemetry->latest(runner.loop(), nodeB.id)).empty());
    EXPECT_FALSE(runner.run(telemetry->scoped(TenantScope{tenantId, false})->insertBatch(runner.loop(), nodeA.id, {cpuSample})));

    auto commands      = std::make_shared<EdgePulse::Command::CommandRepository>(context.database);
    auto tenantCommand = commands->scoped(TenantScope{tenantId, false});
    EXPECT_FALSE(runner.run(commands->create(runner.loop(), nodeB.id, "reboot", "{}", 1, 60)).has_value());
    const auto commandB = runner.run(tenantCommand->create(runner.loop(), nodeB.id, "reboot", "{}", 1, 60));
    ASSERT_TRUE(commandB.has_value());
    EXPECT_EQ(tenantId, commandB->tenantId);
    EXPECT_EQ(0, runner.run(commands->list(runner.loop(), "", 0, {})).total);
    EXPECT_EQ(1, runner.run(tenantCommand->list(runner.loop(), "", 0, {})).total);
    EXPECT_FALSE(runner.run(commands->find(runner.loop(), commandB->id)).has_value());

    auto                                firmware = std::make_shared<EdgePulse::Firmware::FirmwareRepository>(context.database);
    EdgePulse::Firmware::FirmwareRecord record;
    record.name                   = "EdgePulse OS";
    record.version                = "9.0.1";
    record.fileName               = "edge-os.bin";
    record.contentType            = "application/octet-stream";
    record.sha256                 = std::string(64, 'a');
    record.storagePath            = "unused";
    const std::int64_t firmwareId = runner.run(firmware->scoped(TenantScope{tenantId, false})->create(runner.loop(), record));
    ASSERT_GT(firmwareId, 0);
    EXPECT_EQ(0, runner.run(firmware->list(runner.loop(), "", {})).total);
    EXPECT_EQ(1, runner.run(firmware->scoped(TenantScope{tenantId, false})->list(runner.loop(), "", {})).total);
    EXPECT_FALSE(runner.run(firmware->find(runner.loop(), firmwareId)).has_value());
    EXPECT_EQ(0, runner.run(firmware->createTask(runner.loop(), firmwareId, nodeB.id)));
    const std::int64_t taskId = runner.run(firmware->scoped(TenantScope{tenantId, false})->createTask(runner.loop(), firmwareId, nodeB.id));
    ASSERT_GT(taskId, 0);
    EXPECT_EQ(1U, runner.run(firmware->scoped(TenantScope{tenantId, false})->tasks(runner.loop(), firmwareId)).size());
}

// 钉住：告警规则与告警按租户隔离，平台视图可汇总
TEST(TenantIsolation, AlarmsAreIsolatedByTenant)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const std::int64_t                      tenantId = provisionTenantB(context, runner);
    ASSERT_GT(tenantId, 0);

    const auto                        tenantAlarms = std::make_shared<EdgePulse::Alarm::AlarmRepository>(context.database)->scoped(TenantScope{tenantId, false});
    EdgePulse::Alarm::AlarmRuleRecord rule;
    rule.name                 = "高负载";
    rule.metric               = "cpu.usage";
    rule.comparison           = ">";
    rule.threshold            = 90;
    rule.level                = "critical";
    const std::int64_t ruleId = runner.run(tenantAlarms->createRule(runner.loop(), rule, 1));
    ASSERT_GT(ruleId, 0);

    const auto defaultAlarms = std::make_shared<EdgePulse::Alarm::AlarmRepository>(context.database);
    EXPECT_EQ(3, runner.run(defaultAlarms->listRules(runner.loop(), "", {})).total); // 只有默认租户的内置规则
    EXPECT_EQ(1, runner.run(tenantAlarms->listRules(runner.loop(), "", {})).total);
    EXPECT_EQ(4, runner.run(defaultAlarms->scoped(TenantScope{0, true})->listRules(runner.loop(), "", {})).total);
    EXPECT_FALSE(runner.run(defaultAlarms->findRule(runner.loop(), ruleId)).has_value());
    EXPECT_TRUE(runner.run(tenantAlarms->findRule(runner.loop(), ruleId)).has_value());

    const auto storedRule = runner.run(tenantAlarms->findRule(runner.loop(), ruleId));
    ASSERT_TRUE(storedRule.has_value());
    const auto alarm = runner.run(tenantAlarms->insertAlarm(runner.loop(), *storedRule, 1, 95.0, "cpu.usage 超过阈值"));
    ASSERT_TRUE(alarm.has_value());
    EXPECT_EQ(tenantId, alarm->tenantId);
    EXPECT_EQ(1, runner.run(tenantAlarms->openCount(runner.loop())));
    EXPECT_EQ(0, runner.run(defaultAlarms->openCount(runner.loop())));
    EXPECT_EQ(1, runner.run(defaultAlarms->scoped(TenantScope{0, true})->openCount(runner.loop())));
    EXPECT_FALSE(runner.run(defaultAlarms->findAlarm(runner.loop(), alarm->id)).has_value());
}

// 钉住：权限图按会话租户返回用户与用户角色关系，平台管理员仍能跨租户读取
TEST(TenantIsolation, PermissionsGraphIsTenantScoped)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const std::int64_t                      tenantId = provisionTenantB(context, runner);
    ASSERT_GT(tenantId, 0);

    auto users        = std::make_shared<EdgePulse::System::UserRepository>(context.database);
    auto roles        = std::make_shared<EdgePulse::System::RoleRepository>(context.database);
    auto menus        = std::make_shared<EdgePulse::System::MenuRepository>(context.database);
    auto org          = std::make_shared<EdgePulse::System::OrgRepository>(context.database);
    auto dictionaries = std::make_shared<EdgePulse::System::DictionaryRepository>(context.database);
    auto dataScopes   = std::make_shared<EdgePulse::Auth::DataScopeService>(context.database);

    EdgePulse::TestSupport::ServerHarness server(
            [&context, &users, &roles, &menus, &org, &dictionaries, &dataScopes](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
            {
                EdgePulse::Web::applySecurityHeaders(router);
                EdgePulse::System::SystemDependencies dependencies;
                dependencies.auth         = context.auth.get();
                dependencies.dataScopes   = dataScopes.get();
                dependencies.users        = users.get();
                dependencies.roles        = roles.get();
                dependencies.menus        = menus.get();
                dependencies.org          = org.get();
                dependencies.dictionaries = dictionaries.get();
                dependencies.tenants      = context.tenants.get();
                EdgePulse::System::registerSystemRoutes(router, loop, dependencies);
            });

    const auto tenantChallenge = runner.run(context.captcha->issue(runner.loop(), "127.0.0.1"));
    ASSERT_TRUE(tenantChallenge.has_value());
    EdgePulse::Auth::LoginCredentials tenantAdmin{"acme-admin", "acme-pass-1", tenantChallenge->code, tenantChallenge->uuid, "127.0.0.1", "tenant-graph"};
    tenantAdmin.tenantCode = "acme";
    const auto tenantLogin = runner.run(context.auth->login(runner.loop(), tenantAdmin));
    ASSERT_TRUE(tenantLogin.has_value()) << tenantLogin.error().message;

    const auto tenantGraph = server.get("/api/system/permissions/graph", {{"Authorization", "Bearer " + tenantLogin->token}});
    ASSERT_EQ(200, tenantGraph.status) << tenantGraph.body;
    const auto tenantJson = tenantGraph.json();
    ASSERT_TRUE(tenantJson.has_value());
    const auto &tenantUsers = (*tenantJson)["data"]["users"];
    ASSERT_EQ(1U, tenantUsers.size()) << tenantGraph.body;
    EXPECT_EQ("acme-admin", tenantUsers[0]["username"].get<std::string>());
    // 用户角色关系与用户列表同一范围，不能带出默认租户的 user_id
    ASSERT_FALSE((*tenantJson)["data"]["userRoles"].empty()) << tenantGraph.body;
    for (const auto &pair: (*tenantJson)["data"]["userRoles"])
    {
        EXPECT_NE(1, pair[0].get<std::int64_t>()) << tenantGraph.body;
    }

    // 平台管理员按全租户视图读取
    const auto platformChallenge = runner.run(context.captcha->issue(runner.loop(), "127.0.0.1"));
    ASSERT_TRUE(platformChallenge.has_value());
    EdgePulse::Auth::LoginCredentials platformAdmin{"admin", "admin123", platformChallenge->code, platformChallenge->uuid, "127.0.0.1", "tenant-graph"};
    const auto platformLogin = runner.run(context.auth->login(runner.loop(), platformAdmin));
    ASSERT_TRUE(platformLogin.has_value()) << platformLogin.error().message;
    const auto platformGraph = server.get("/api/system/permissions/graph", {{"Authorization", "Bearer " + platformLogin->token}});
    ASSERT_EQ(200, platformGraph.status) << platformGraph.body;
    const auto platformJson = platformGraph.json();
    ASSERT_TRUE(platformJson.has_value());
    EXPECT_GE((*platformJson)["data"]["users"].size(), 2U) << platformGraph.body;
}

// 钉住：日志删除与清空也受租户边界约束，租户管理员不能借删除接口抹掉别家日志
TEST(TenantIsolation, LogDeletionRespectsTenantScope)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const std::int64_t                      tenantId = provisionTenantB(context, runner);
    ASSERT_GT(tenantId, 0);

    EdgePulse::System::LogRepository logs(context.database);
    const auto                       writeLog = [&runner, &logs, tenantId](const std::string &title, const std::int64_t ownerTenant)
    {
        EdgePulse::System::OperLogRecord record;
        record.title    = title;
        record.module   = title;
        record.action   = "create";
        record.operName = "tester";
        record.tenantId = ownerTenant;
        record.operTime = EdgePulse::Common::nowDateTimeUtc();
        return runner.run(logs.insertOperLog(runner.loop(), record));
    };
    ASSERT_TRUE(writeLog("default-log", 1));
    ASSERT_TRUE(writeLog("tenant-log", tenantId));

    EdgePulse::System::LogScope tenantScope;
    tenantScope.allTenants = false;
    tenantScope.tenantId   = tenantId;
    const auto tenantLogs  = runner.run(logs.listOperLogs(runner.loop(), "tenant-log", "", {1, 10}, tenantScope));
    ASSERT_EQ(1, tenantLogs.total);
    const std::int64_t tenantLogId = tenantLogs.rows[0].id;
    const auto         defaultLogs = runner.run(logs.listOperLogs(runner.loop(), "default-log", "", {1, 10}));
    ASSERT_EQ(1, defaultLogs.total);

    // 跨租户删除失败；同租户删除成功
    EXPECT_FALSE(runner.run(logs.removeOperLog(runner.loop(), defaultLogs.rows[0].id, tenantScope)));
    EXPECT_TRUE(runner.run(logs.removeOperLog(runner.loop(), tenantLogId, tenantScope)));

    // 清空只影响本租户
    ASSERT_TRUE(writeLog("default-log-2", 1));
    ASSERT_TRUE(writeLog("tenant-log-2", tenantId));
    EXPECT_TRUE(runner.run(logs.clearOperLogs(runner.loop(), tenantScope)));
    EXPECT_EQ(2, runner.run(logs.listOperLogs(runner.loop(), "default-log", "", {1, 10})).total);
    EXPECT_EQ(0, runner.run(logs.listOperLogs(runner.loop(), "", "", {1, 10}, tenantScope)).total);
}
