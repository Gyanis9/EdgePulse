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
        EXPECT_EQ(8, querySingleInteger(*connection, "SELECT COUNT(*) FROM schema_migrations"));
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
