#include "Base/Database/Database.h"
#include "Base/Exception/EdgePulseException.h"
#include "Core/System/MenuRepository.h"
#include "Core/System/MigrationRunner.h"
#include "Core/System/UserRepository.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace
{
    constexpr const char *kSeedPasswordHash = "pbkdf2_sha256$210000$nUwvGntuPFCBorPE1eb3CA==$cxCcCezVVbPzfukzjfcpWoTZpVyQ3AR1H9Z2V1QWObk=";

    /**
     * @brief 每个用例独立的临时数据库路径，避免并行时互相覆盖
     */
    std::filesystem::path makeTemporaryDatabasePath()
    {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        return std::filesystem::temp_directory_path() / ("edgepulse-p0-" + suffix + ".db");
    }

    /**
     * @brief 在独立连接上应用仓库内的迁移目录
     */
    void applyMigrations(const std::filesystem::path &databasePath)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
        EdgePulse::System::MigrationRunner::apply(*connection, std::filesystem::path(EDGEPULSE_SOURCE_DIR) / "migrations");
    }

    /**
     * @brief 读一条单列整数
     */
    std::int64_t querySingleInteger(AsynGyanis::Database::DatabaseConnection &connection, const std::string &sql)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(sql);
        EXPECT_NE(nullptr, result) << connection.lastError();
        if (!result)
        {
            return -1;
        }
        EXPECT_TRUE(result->next());
        const auto value = result->getValue(0);
        if (const auto *number = std::get_if<std::int64_t>(&value); number != nullptr)
        {
            return *number;
        }
        return -1;
    }
} // namespace

// 钉住：三版迁移能建表、落种子，迁移后再跑一次不会重复插入
TEST(Migrations, AppliesSchemaSeedAndIsIdempotent)
{
    const std::filesystem::path databasePath = makeTemporaryDatabasePath();
    {
        applyMigrations(databasePath);
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM sys_user"));
        EXPECT_GE(querySingleInteger(*connection, "SELECT COUNT(*) FROM sys_menu"), 30);
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM schema_migrations WHERE version = 1"));

        const std::unique_ptr<AsynGyanis::Database::DatabaseResult> hashResult = connection->execute("SELECT password FROM sys_user WHERE username = 'admin'");
        ASSERT_NE(nullptr, hashResult) << connection->lastError();
        ASSERT_TRUE(hashResult->next());
        const auto hashValue = hashResult->getValue(0);
        ASSERT_TRUE(std::holds_alternative<std::string>(hashValue));
        EXPECT_EQ(kSeedPasswordHash, std::get<std::string>(hashValue));
    }

    // 第二次应用应该全部走已应用分支，不抛异常也不产生第二份种子
    applyMigrations(databasePath);
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM sys_user"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM schema_migrations WHERE version = 15"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM schema_migrations WHERE version = 16"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM schema_migrations WHERE version = 17"));
        EXPECT_EQ(3, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_table_info('biz_firmware') "
                                                     "WHERE name IN ('signature', 'signing_key_id', 'signature_algorithm')"));
        // 只断存在与空表：表缺失时 COUNT(*) 会因查询失败返回 -1，两种情况都不会被放过
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'biz_agent_enrollment'"));
        EXPECT_EQ(0, querySingleInteger(*connection, "SELECT COUNT(*) FROM biz_agent_enrollment"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'biz_node_config'"));
        EXPECT_EQ(0, querySingleInteger(*connection, "SELECT COUNT(*) FROM biz_node_config"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'biz_node_config_revision'"));
        EXPECT_EQ(0, querySingleInteger(*connection, "SELECT COUNT(*) FROM biz_node_config_revision"));
        // 修订表必须同时有租户索引与节点/版本唯一约束，否则分页与并发兜底都会退化成全表扫描
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('biz_node_config_revision') WHERE name = 'idx_node_config_revision_tenant'"));
        EXPECT_EQ(2, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('biz_node_config_revision') AS il, pragma_index_info(il.name) AS ii "
                                                     "WHERE il.\"unique\" = 1 AND ii.name IN ('node_id', 'version')"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM schema_migrations WHERE version = 18"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM schema_migrations WHERE version = 19"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'sys_api_token'"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'sys_idempotency'"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'sys_user_mfa'"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('sys_api_token') WHERE name = 'idx_api_token_tenant'"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('sys_api_token') WHERE name = 'idx_api_token_previous'"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('sys_idempotency') WHERE name = 'idx_idempotency_expires'"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('sys_user_mfa') WHERE name = 'idx_user_mfa_tenant'"));
        // 租户列表与过期清理都走这条复合索引，列序反了会退化成回表扫描
        EXPECT_EQ(3, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_info('idx_api_token_tenant') "
                                                     "WHERE (name = 'tenant_id' AND seqno = 0) OR (name = 'revoked_at' AND seqno = 1) OR (name = 'expires_at' AND seqno = 2)"));
        // 令牌哈希全局唯一、幂等键按令牌成对唯一：并发写入的去重只靠这两条唯一索引兜底
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('sys_api_token') AS il, pragma_index_info(il.name) AS ii "
                                                     "WHERE il.\"unique\" = 1 AND ii.name = 'token_hash'"));
        EXPECT_EQ(2, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('sys_idempotency') AS il, pragma_index_info(il.name) AS ii "
                                                     "WHERE il.\"unique\" = 1 AND ii.name IN ('token_hash', 'key_hash')"));
        // 令牌表按令牌哈希查、幂等表按令牌与非空响应体回放：两处缺列都会让后续接口在运行期才发现
        EXPECT_EQ(2, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_table_info('sys_api_token') WHERE name IN ('token_hash', 'previous_hash')"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_table_info('sys_idempotency') WHERE name = 'response_body'"));
        EXPECT_EQ(1, querySingleInteger(*connection,
                                        "SELECT COUNT(*) FROM sys_menu WHERE id = 1210 AND parent_id = 1100 AND menu_type = 'menu' AND permission = 'system:token:list'"));
        EXPECT_EQ(3, querySingleInteger(*connection, "SELECT COUNT(*) FROM sys_menu WHERE id IN (1211, 1212, 1213) AND parent_id = 1210 AND menu_type = 'btn'"));
        EXPECT_EQ(4, querySingleInteger(*connection, "SELECT COUNT(*) FROM sys_role_menu WHERE menu_id IN (1210,1211,1212,1213) AND role_id = 1"));
        EXPECT_EQ(4, querySingleInteger(*connection, "SELECT COUNT(*) FROM sys_role_menu WHERE menu_id IN (1210,1211,1212,1213) AND role_id = 3"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM schema_migrations WHERE version = 20"));
        // 轮换要同时读到当前与上一枚证书，缺任一列都会让重叠窗口在运行期静默失效
        EXPECT_EQ(2, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_table_info('biz_node') "
                                                     "WHERE name IN ('previous_cert_fingerprint', 'previous_cert_not_after')"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'biz_agent_certificate'"));
        EXPECT_EQ(0, querySingleInteger(*connection, "SELECT COUNT(*) FROM biz_agent_certificate"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('biz_agent_certificate') WHERE name = 'idx_agent_certificate_node'"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('biz_agent_certificate') WHERE name = 'idx_agent_certificate_fingerprint'"));
        // 历史按节点与签发时间读取，列序反了会退化成回表扫描
        EXPECT_EQ(3, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_info('idx_agent_certificate_node') "
                                                     "WHERE (name = 'tenant_id' AND seqno = 0) OR (name = 'node_id' AND seqno = 1) OR (name = 'issued_at' AND seqno = 2)"));
        EXPECT_EQ(11, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_table_info('biz_agent_certificate')"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM schema_migrations WHERE version = 21"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'sys_lease'"));
        EXPECT_EQ(5, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_table_info('sys_lease') "
                                                     "WHERE name IN ('name', 'holder', 'fence', 'expires_at', 'updated_at')"));
        // 种子行必须可被接手：holder 为空、fence 为 0、expires_at 为 NULL，updated_at 用纪元作哨兵
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM sys_lease WHERE name = 'control-plane' AND holder = '' AND fence = 0 "
                                                     "AND expires_at IS NULL AND updated_at = '1970-01-01 00:00:00'"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM schema_migrations WHERE version = 22"));
        EXPECT_EQ(3, querySingleInteger(*connection, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name IN "
                                                     "('biz_firmware_rollout', 'biz_firmware_rollout_wave', 'biz_firmware_rollout_target')"));
        // 聚合列与状态机一一对应，缺列会让协调器在运行期才发现
        EXPECT_EQ(16, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_table_info('biz_firmware_rollout')"));
        EXPECT_EQ(11, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_table_info('biz_firmware_rollout_wave')"));
        EXPECT_EQ(11, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_table_info('biz_firmware_rollout_target')"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('biz_firmware_rollout') WHERE name = 'idx_fw_rollout_tenant'"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('biz_firmware_rollout_wave') WHERE name = 'idx_fw_rollout_wave'"));
        // 同一发布的波次下标唯一，协调器重复物化波次时由数据库兜底
        EXPECT_EQ(2, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('biz_firmware_rollout_wave') AS il, pragma_index_info(il.name) AS ii "
                                                     "WHERE il.\"unique\" = 1 AND ii.name IN ('rollout_id', 'wave_index')"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('biz_firmware_rollout_target') WHERE name = 'idx_fw_rollout_target_wave'"));
        // 同一节点在一个发布里只能出现一次，并发物化目标时靠这条唯一索引兜底
        EXPECT_EQ(2, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('biz_firmware_rollout_target') AS il, pragma_index_info(il.name) AS ii "
                                                     "WHERE il.\"unique\" = 1 AND ii.name IN ('rollout_id', 'node_id')"));
        EXPECT_EQ(3, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_table_info('biz_firmware_task') "
                                                     "WHERE name IN ('rollout_id', 'wave_id', 'command_id')"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('biz_firmware_task') WHERE name = 'idx_fw_task_rollout'"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_table_info('biz_node') WHERE name = 'installed_firmware_version'"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM schema_migrations WHERE version = 23"));
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM schema_migrations WHERE version = 24"));
        // 任务与命令的绑定必须唯一，重复派发才能被数据库兜底拒绝；未绑定行用负值占位后仍可建可移植索引
        EXPECT_EQ(1, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('biz_firmware_task') WHERE name = 'uq_fw_task_command'"));
        EXPECT_EQ(3, querySingleInteger(*connection, "SELECT COUNT(*) FROM pragma_index_list('biz_firmware_task') AS il, pragma_index_info(il.name) AS ii "
                                                     "WHERE il.\"unique\" = 1 AND ii.name IN ('firmware_id', 'node_id', 'command_id')"));
        ASSERT_NE(nullptr, connection->execute("INSERT INTO biz_firmware_task (id, tenant_id, firmware_id, node_id, status, command_id) VALUES (99001, 1, 1, 1, 'pending', 77)"));
        EXPECT_EQ(nullptr, connection->execute("INSERT INTO biz_firmware_task (id, tenant_id, firmware_id, node_id, status, command_id) VALUES (99002, 1, 1, 1, 'pending', 77)"))
                << "同一固件与节点不能绑定同一个命令";
        ASSERT_NE(nullptr, connection->execute("INSERT INTO biz_firmware_task (id, tenant_id, firmware_id, node_id, status, command_id) VALUES (99003, 1, 1, 1, 'pending', -99003)"));
        ASSERT_NE(nullptr, connection->execute("INSERT INTO biz_firmware_task (id, tenant_id, firmware_id, node_id, status, command_id) VALUES (99004, 1, 1, 1, 'pending', -99004)"))
                << "未绑定命令的任务用不同的负值占位，可以共存";
        EXPECT_EQ(24, querySingleInteger(*connection, "SELECT COUNT(*) FROM schema_migrations"));
    }
    std::filesystem::remove(databasePath);
}

// 钉住：已应用迁移的内容被改动时拒绝启动，而不是悄悄按新文件继续
TEST(Migrations, RejectsChecksumMismatch)
{
    const std::filesystem::path databasePath = makeTemporaryDatabasePath();
    applyMigrations(databasePath);
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
        ASSERT_NE(nullptr, connection->execute("UPDATE schema_migrations SET checksum = 'deadbeef' WHERE version = 1"));
    }
    EXPECT_THROW(applyMigrations(databasePath), EdgePulse::Common::MigrationException);
    std::filesystem::remove(databasePath);
}

// 钉住：仓储能从种子读到管理员、角色、权限码与菜单树
TEST(Migrations, RepositoriesReadSeedData)
{
    const std::filesystem::path databasePath = makeTemporaryDatabasePath();
    applyMigrations(databasePath);

    {
        const std::shared_ptr<EdgePulse::Common::Database> database = EdgePulse::Common::Database::openSqlite(databasePath.string());
        EdgePulse::TestSupport::EventLoopRunner            runner;
        EdgePulse::System::UserRepository                  users(database);
        EdgePulse::System::MenuRepository                  menus(database);

        const std::optional<EdgePulse::System::UserRecord> admin = runner.run(users.findByUsername(runner.loop(), "admin"));
        ASSERT_TRUE(admin.has_value());
        EXPECT_EQ("admin", admin->username);
        EXPECT_EQ("超级管理员", admin->nickname);
        EXPECT_EQ(1, admin->status);
        EXPECT_EQ(kSeedPasswordHash, admin->passwordHash);

        const std::vector<std::string> roles = runner.run(users.rolesFor(runner.loop(), admin->id));
        ASSERT_EQ(1U, roles.size());
        EXPECT_EQ("super_admin", roles[0]);

        const std::vector<std::string> permissions = runner.run(users.permissionsFor(runner.loop(), admin->id));
        EXPECT_NE(permissions.end(), std::find(permissions.begin(), permissions.end(), "device:node:list"));
        EXPECT_NE(permissions.end(), std::find(permissions.begin(), permissions.end(), "device:node:write"));

        const std::vector<EdgePulse::System::MenuNode> tree = runner.run(menus.treeForUser(runner.loop(), admin->id));
        const auto group = std::find_if(tree.begin(), tree.end(), [](const EdgePulse::System::MenuNode &node) { return node.name == "边缘运维"; });
        ASSERT_NE(tree.end(), group);
        const auto node = std::find_if(group->children.begin(), group->children.end(), [](const EdgePulse::System::MenuNode &child) { return child.name == "节点"; });
        ASSERT_NE(group->children.end(), node);
        EXPECT_EQ("device/Nodes", node->component);
        EXPECT_EQ("/device/nodes", node->path);
    }
    std::filesystem::remove(databasePath);
}
