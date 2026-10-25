#include "Core/System/MigrationLock.h"

#include "Base/Database/Database.h"
#include "Base/Log/LogMacros.h"

#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseValue.h"

#include <array>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <variant>

namespace EdgePulse::System
{
    namespace
    {
        constexpr const char *kLockName = "edgepulse:migrations"; ///< MySQL 命名锁，全项目共用一把
    } // namespace

    MigrationLock::MigrationLock(std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection) noexcept : m_connection(std::move(connection))
    {
        m_held = m_connection != nullptr;
    }

    MigrationLock::~MigrationLock()
    {
        release();
    }

    std::expected<std::unique_ptr<MigrationLock>, std::string> MigrationLock::acquire(const Common::DatabaseOptions &options, const int timeoutSeconds)
    {
        if (timeoutSeconds < 0)
        {
            return std::unexpected(std::string{"迁移锁等待秒数不能为负数"});
        }
        if (options.driver == "sqlite")
        {
            // SQLite 是单写者，迁移由进程启动顺序保证串行，不需要也不应假装有跨实例锁
            LOG_INFO("SQLite 是单写者，跳过迁移锁");
            return std::unique_ptr<MigrationLock>(new MigrationLock(nullptr));
        }
        if (options.driver != "mysql")
        {
            return std::unexpected("不支持的数据库驱动：" + options.driver + "；迁移锁只支持 sqlite 或 mysql");
        }

        try
        {
            std::unique_ptr<AsynGyanis::Database::DatabaseConnection>   connection = Common::Database::openDirect(options);
            const std::array<AsynGyanis::Database::DatabaseValue, 2>    parameters{std::string{kLockName}, static_cast<std::int64_t>(timeoutSeconds)};
            const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection->execute("SELECT GET_LOCK(?, ?)", parameters);
            if (!result)
            {
                const std::string reason = connection->lastError();
                connection->disconnect();
                return std::unexpected("执行 MySQL GET_LOCK 失败：" + reason);
            }
            if (!result->next())
            {
                connection->disconnect();
                return std::unexpected("执行 MySQL GET_LOCK 失败：结果集为空");
            }
            const AsynGyanis::Database::DatabaseValue value = result->getValue(0);
            const auto                               *code  = std::get_if<std::int64_t>(&value);
            if (code == nullptr)
            {
                connection->disconnect();
                return std::unexpected("执行 MySQL GET_LOCK 失败：返回值不是整数");
            }
            if (*code != 1)
            {
                connection->disconnect();
                if (*code == 0)
                {
                    return std::unexpected("等待迁移锁超时（" + std::to_string(timeoutSeconds) + " 秒）：另一实例正在执行数据库迁移");
                }
                return std::unexpected("MySQL GET_LOCK 返回失败状态：" + std::to_string(*code));
            }
            return std::unique_ptr<MigrationLock>(new MigrationLock(std::move(connection)));
        } catch (const std::exception &failure)
        {
            return std::unexpected("获取迁移锁失败：" + std::string{failure.what()});
        }
    }

    void MigrationLock::release() noexcept
    {
        if (!m_connection)
        {
            m_held = false;
            return;
        }
        if (!m_held)
        {
            return;
        }
        m_held = false;
        try
        {
            const std::array<AsynGyanis::Database::DatabaseValue, 1>    parameters{std::string{kLockName}};
            const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = m_connection->execute("SELECT RELEASE_LOCK(?)", parameters);
            if (!result || !result->next())
            {
                LOG_WARN_FMT("释放迁移锁失败：{}", m_connection->lastError());
            }
            m_connection->disconnect();
        } catch (const std::exception &failure)
        {
            LOG_WARN_FMT("释放迁移锁失败：{}", failure.what());
        }
        m_connection.reset();
    }
} // namespace EdgePulse::System
