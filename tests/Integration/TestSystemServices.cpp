#include "Base/Security/PasswordHash.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Auth/DataScopeService.h"
#include "Core/Auth/LoginAudit.h"
#include "Core/System/DictionaryRepository.h"
#include "Core/System/LogRepository.h"
#include "Core/System/MenuRepository.h"
#include "Core/System/OrgRepository.h"
#include "Core/System/RoleRepository.h"
#include "Core/System/UserRepository.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using EdgePulse::System::UserWrite;

    [[nodiscard]] UserWrite newUser(const std::string &username, const std::string &nickname, const std::int64_t deptId)
    {
        UserWrite write;
        write.username = username;
        write.nickname = nickname;
        write.deptId   = deptId;
        write.status   = 1;
        return write;
    }
} // namespace

// 钉住：用户 CRUD、角色/岗位关联、状态与重置口令都在同一条仓储链上成立
TEST(SystemServices, UserCrudKeepsRelations)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::System::UserRepository       users(context.database);

    UserWrite write           = newUser("alice", "爱丽丝", 101);
    write.phone               = "13800000000";
    write.roleIds             = std::vector<std::int64_t>{1};
    write.postIds             = std::vector<std::int64_t>{101};
    const std::int64_t userId = runner.run(users.create(runner.loop(), write, EdgePulse::Common::hashPassword("alice123"), 1));

    const auto found = runner.run(users.findById(runner.loop(), userId));
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ("alice", found->username);
    EXPECT_EQ("研发中心", found->departmentName);
    EXPECT_EQ("超级管理员", found->roleNames);
    EXPECT_EQ("运维工程师", found->postNames);

    UserWrite update;
    update.nickname = "爱丽丝2";
    update.status   = 0;
    ASSERT_TRUE(runner.run(users.update(runner.loop(), userId, update, 1)));
    const auto updated = runner.run(users.findById(runner.loop(), userId));
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ("爱丽丝2", updated->nickname);
    EXPECT_EQ(0, updated->status);

    const auto page = runner.run(users.list(runner.loop(), EdgePulse::System::UserQuery{"alice", "", 0, {}, false, 0}, EdgePulse::Common::PageParameters{1, 10}));
    EXPECT_EQ(1, page.total);
    ASSERT_EQ(1U, page.rows.size());

    ASSERT_TRUE(runner.run(users.resetPassword(runner.loop(), userId, EdgePulse::Common::hashPassword("newpass1"), 1)));
    const auto reread = runner.run(users.findById(runner.loop(), userId));
    ASSERT_TRUE(reread.has_value());
    EXPECT_TRUE(EdgePulse::Common::verifyPassword(reread->passwordHash, "newpass1"));

    ASSERT_TRUE(runner.run(users.remove(runner.loop(), userId)));
    EXPECT_FALSE(runner.run(users.findById(runner.loop(), userId)).has_value());
}

// 钉住：DEPT 角色只可见本部门；SELF 角色只看本人；管理员 ALL 不受限
TEST(SystemServices, DataScopeFollowsRoleConfiguration)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::System::UserRepository       users(context.database);
    EdgePulse::System::RoleRepository       roles(context.database);
    EdgePulse::Auth::DataScopeService       scopes(context.database);

    EdgePulse::System::RoleWrite roleWrite;
    roleWrite.name            = "运维人员";
    roleWrite.code            = "ops_scope";
    roleWrite.dataScope       = "DEPT";
    roleWrite.status          = 1;
    const std::int64_t roleId = runner.run(roles.create(runner.loop(), roleWrite, 1));

    UserWrite write           = newUser("bob", "鲍勃", 101);
    write.roleIds             = std::vector<std::int64_t>{roleId};
    const std::int64_t userId = runner.run(users.create(runner.loop(), write, EdgePulse::Common::hashPassword("bob12345"), 1));

    const auto scope = runner.run(scopes.resolve(runner.loop(), userId));
    EXPECT_FALSE(scope.all);
    EXPECT_FALSE(scope.selfOnly);
    EXPECT_NE(scope.deptIds.end(), std::find(scope.deptIds.begin(), scope.deptIds.end(), 101));
    EXPECT_EQ(scope.deptIds.end(), std::find(scope.deptIds.begin(), scope.deptIds.end(), 102));

    EdgePulse::System::UserQuery query;
    query.allowedDeptIds = scope.deptIds;
    query.userId         = userId;
    const auto page      = runner.run(users.list(runner.loop(), query, EdgePulse::Common::PageParameters{1, 100}));
    ASSERT_EQ(1U, page.rows.size());
    EXPECT_EQ("bob", page.rows[0].username);

    EdgePulse::System::RoleWrite selfWrite;
    selfWrite.name               = "自助";
    selfWrite.code               = "self_scope";
    selfWrite.dataScope          = "SELF";
    const std::int64_t selfRole  = runner.run(roles.create(runner.loop(), selfWrite, 1));
    UserWrite          selfUser  = newUser("carol", "卡罗尔", 102);
    selfUser.roleIds             = std::vector<std::int64_t>{selfRole};
    const std::int64_t selfId    = runner.run(users.create(runner.loop(), selfUser, EdgePulse::Common::hashPassword("carol123"), 1));
    const auto         selfScope = runner.run(scopes.resolve(runner.loop(), selfId));
    EXPECT_TRUE(selfScope.selfOnly);

    const auto adminScope = runner.run(scopes.resolve(runner.loop(), 1));
    EXPECT_TRUE(adminScope.all);
}

// 钉住：角色菜单授权整体替换；菜单删除前必须先清空子节点
TEST(SystemServices, RoleMenusAndMenuRemovalGuard)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::System::RoleRepository       roles(context.database);
    EdgePulse::System::MenuRepository       menus(context.database);

    EdgePulse::System::RoleWrite write;
    write.name                = "只读";
    write.code                = "read_only";
    write.menuIds             = std::vector<std::int64_t>{1300, 1000, 1001};
    const std::int64_t roleId = runner.run(roles.create(runner.loop(), write, 1));
    auto               ids    = runner.run(roles.menuIdsFor(runner.loop(), roleId));
    EXPECT_EQ(3U, ids.size());

    ASSERT_TRUE(runner.run(roles.updateMenus(runner.loop(), roleId, std::vector<std::int64_t>{1300})));
    ids = runner.run(roles.menuIdsFor(runner.loop(), roleId));
    ASSERT_EQ(1U, ids.size());
    EXPECT_EQ(1300, ids[0]);

    EXPECT_TRUE(runner.run(menus.hasChildren(runner.loop(), 1000)));
    EXPECT_FALSE(runner.run(menus.hasChildren(runner.loop(), 1002)));
}

// 钉住：字典与参数 CRUD 的可移植 SQL 路径
TEST(SystemServices, DictionaryAndConfigCrud)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::System::DictionaryRepository dictionary(context.database);

    EdgePulse::System::DictTypeWrite typeWrite;
    typeWrite.name            = "机房";
    typeWrite.type            = "site_zone";
    const std::int64_t typeId = runner.run(dictionary.createDictType(runner.loop(), typeWrite, 1));
    EXPECT_TRUE(runner.run(dictionary.dictTypeExists(runner.loop(), "site_zone")));

    EdgePulse::System::DictDataWrite dataWrite;
    dataWrite.type            = "site_zone";
    dataWrite.label           = "华东";
    dataWrite.value           = "east";
    const std::int64_t dataId = runner.run(dictionary.createDictData(runner.loop(), dataWrite, 1));
    auto               data   = runner.run(dictionary.dictData(runner.loop(), "site_zone"));
    ASSERT_EQ(1U, data.size());
    EXPECT_EQ("华东", data[0].label);

    ASSERT_TRUE(runner.run(dictionary.removeDictType(runner.loop(), typeId)));
    data = runner.run(dictionary.dictData(runner.loop(), "site_zone"));
    EXPECT_TRUE(data.empty());
    static_cast<void>(dataId);

    EdgePulse::System::ConfigWrite configWrite;
    configWrite.name            = "测试参数";
    configWrite.key             = "test.option";
    configWrite.value           = "on";
    const std::int64_t configId = runner.run(dictionary.createConfig(runner.loop(), configWrite, 1));
    EXPECT_TRUE(runner.run(dictionary.configKeyExists(runner.loop(), "test.option")));
    ASSERT_TRUE(runner.run(dictionary.removeConfig(runner.loop(), configId)));
}

// 钉住：操作日志与登录日志按部门/本人范围过滤，分页总数与行一致
TEST(SystemServices, LogScopeFiltersOperAndLoginLogs)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    EdgePulse::System::LogRepository        logs(context.database);
    EdgePulse::Auth::LoginAudit             loginAudit(context.database);

    for (const auto &[dept, oper, name]: {std::tuple<std::int64_t, std::int64_t, const char *>{101, 7, "alice"}, {102, 8, "bob"}})
    {
        EdgePulse::System::OperLogRecord record;
        record.module   = "验收";
        record.action   = "查询";
        record.operName = name;
        record.operId   = oper;
        record.deptId   = dept;
        record.operTime = EdgePulse::Common::nowDateTimeUtc();
        ASSERT_TRUE(runner.run(logs.insertOperLog(runner.loop(), record)));
        ASSERT_TRUE(runner.run(loginAudit.record(runner.loop(), EdgePulse::Auth::LoginAuditEntry{"user", "127.0.0.1", true, "ok", "t", oper, dept})));
    }

    EdgePulse::System::LogScope deptScope;
    deptScope.all       = false;
    deptScope.deptIds   = {101};
    const auto operPage = runner.run(logs.listOperLogs(runner.loop(), "", "", EdgePulse::Common::PageParameters{1, 10}, deptScope));
    EXPECT_EQ(1, operPage.total);
    ASSERT_EQ(1U, operPage.rows.size());
    EXPECT_EQ("alice", operPage.rows[0].operName);
    const auto loginPage = runner.run(logs.listLoginLogs(runner.loop(), "", "", EdgePulse::Common::PageParameters{1, 10}, deptScope));
    EXPECT_EQ(1, loginPage.total);

    EdgePulse::System::LogScope selfScope;
    selfScope.all       = false;
    selfScope.selfOnly  = true;
    selfScope.userId    = 8;
    const auto selfOper = runner.run(logs.listOperLogs(runner.loop(), "", "", EdgePulse::Common::PageParameters{1, 10}, selfScope));
    ASSERT_EQ(1U, selfOper.rows.size());
    EXPECT_EQ("bob", selfOper.rows[0].operName);
    const auto allLogin = runner.run(logs.listLoginLogs(runner.loop(), "", "", EdgePulse::Common::PageParameters{1, 10}));
    EXPECT_EQ(2, allLogin.total);
}
