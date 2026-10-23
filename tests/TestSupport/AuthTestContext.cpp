#include "TestSupport/AuthTestContext.h"

#include "Core/Auth/AuthService.h"
#include "Core/System/TenantRepository.h"
#include "Core/Auth/CaptchaService.h"
#include "Core/Auth/LoginAudit.h"
#include "Core/Auth/SessionStore.h"
#include "Core/Cache/MemoryKeyValueStore.h"
#include "Base/Database/Database.h"
#include "Core/System/MigrationRunner.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace EdgePulse::TestSupport
{
    namespace
    {
        [[nodiscard]] std::filesystem::path makeTemporaryDatabasePath()
        {
            const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            return std::filesystem::temp_directory_path() / ("edgepulse-auth-" + suffix + ".db");
        }
    } // namespace

    AuthTestContext::AuthTestContext(const Auth::AuthOptions options) : m_databasePath(makeTemporaryDatabasePath())
    {
        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = Common::Database::openDirectSqlite(m_databasePath.string());
            System::MigrationRunner::apply(*connection, std::filesystem::path(EDGEPULSE_SOURCE_DIR) / "migrations");
        }
        database = Common::Database::openSqlite(m_databasePath.string());
        cache    = std::make_shared<Cache::MemoryKeyValueStore>();
        sessions = std::make_shared<Auth::SessionStore>(cache, 7200);
        captcha  = std::make_shared<Auth::CaptchaService>(cache, Auth::CaptchaOptions{120, 30});
        audit    = std::make_shared<Auth::LoginAudit>(database);
        tenants  = std::make_shared<System::TenantRepository>(database);
        auth     = std::make_shared<Auth::AuthService>(database, sessions, captcha, audit, cache, tenants, options);
    }

    AuthTestContext::~AuthTestContext()
    {
        // 先放掉服务与连接池，Windows 才允许删除仍被打开的文件
        auth.reset();
        audit.reset();
        captcha.reset();
        sessions.reset();
        cache.reset();
        database.reset();
        std::error_code error;
        std::filesystem::remove(m_databasePath, error);
    }
} // namespace EdgePulse::TestSupport
