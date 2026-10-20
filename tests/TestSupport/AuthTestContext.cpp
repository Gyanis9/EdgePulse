#include "TestSupport/AuthTestContext.h"

#include "Auth/AuthService.h"
#include "Auth/CaptchaService.h"
#include "Auth/LoginAudit.h"
#include "Auth/SessionStore.h"
#include "Common/Database.h"
#include "System/MigrationRunner.h"

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

    AuthTestContext::AuthTestContext() : m_databasePath(makeTemporaryDatabasePath())
    {
        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = Common::Database::openDirectSqlite(m_databasePath.string());
            System::MigrationRunner::apply(*connection, std::filesystem::path(EDGEPULSE_SOURCE_DIR) / "migrations");
        }
        database = Common::Database::openSqlite(m_databasePath.string());
        sessions = std::make_shared<Auth::SessionStore>(7200);
        captcha  = std::make_shared<Auth::CaptchaService>(Auth::CaptchaOptions{120, 30});
        audit    = std::make_shared<Auth::LoginAudit>(database);
        auth     = std::make_shared<Auth::AuthService>(database, sessions, captcha, audit);
    }

    AuthTestContext::~AuthTestContext()
    {
        // 先放掉服务与连接池，Windows 才允许删除仍被打开的文件
        auth.reset();
        audit.reset();
        captcha.reset();
        sessions.reset();
        database.reset();
        std::error_code error;
        std::filesystem::remove(m_databasePath, error);
    }
} // namespace EdgePulse::TestSupport
