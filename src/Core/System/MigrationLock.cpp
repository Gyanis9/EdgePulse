#include "Core/System/MigrationLock.h"

#include "Base/Database/Database.h"
#include "Base/Log/LogMacros.h"

#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseValue.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace EdgePulse::System
{
    namespace
    {
        constexpr std::string_view kLockNamePrefix        = "edgepulse:migrations"; ///< MySQL 命名锁的固定前缀
        constexpr std::size_t      kMaximumLockNameLength = 64;                     ///< MySQL GET_LOCK 的名称上限

        /**
         * @brief 32 位 FNV-1a 摘要，只用于给超长库名生成稳定的短后缀
         * @param text 待摘要文本
         * @return std::uint32_t 摘要值
         */
        [[nodiscard]] std::uint32_t fnv1a32(const std::string_view text) noexcept
        {
            std::uint32_t hash = 2166136261U;
            for (const char character: text)
            {
                hash ^= static_cast<unsigned char>(character);
                hash *= 16777619U;
            }
            return hash;
        }

        /**
         * @brief 把 32 位整数写成 8 位小写十六进制
         * @param value 待格式化的值
         * @return std::string 定长 8 字符
         */
        [[nodiscard]] std::string hex32(const std::uint32_t value)
        {
            constexpr char digits[] = "0123456789abcdef";
            std::string    text(8, '0');
            for (std::size_t index = 0; index < text.size(); ++index)
            {
                const std::size_t shift = (text.size() - 1 - index) * 4;
                text[index]             = digits[(value >> shift) & 0xFU];
            }
            return text;
        }

        /**
         * @brief 生成带库名限定的迁移锁名，同一 MySQL 服务器上的不同库互不阻塞
         * @param database MySQL 库名
         * @return std::string 不超过 64 字符；库名过长时保留前缀与库名前段，再接 8 位摘要防撞名
         */
        [[nodiscard]] std::string migrationLockName(const std::string &database)
        {
            std::string name{kLockNamePrefix};
            if (!database.empty())
            {
                name += ':';
                name += database;
            }
            if (name.size() <= kMaximumLockNameLength)
            {
                return name;
            }
            const std::string suffix = ":" + hex32(fnv1a32(database));
            const std::size_t keep   = kMaximumLockNameLength - kLockNamePrefix.size() - 1 - suffix.size();
            name.assign(kLockNamePrefix);
            name += ':';
            name.append(database, 0, keep);
            name += suffix;
            return name;
        }
    } // namespace

    MigrationLock::MigrationLock(std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection, std::string lockName) noexcept :
        m_connection(std::move(connection)), m_lockName(std::move(lockName))
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
            return std::unique_ptr<MigrationLock>(new MigrationLock(nullptr, {}));
        }
        if (options.driver != "mysql")
        {
            return std::unexpected("不支持的数据库驱动：" + options.driver + "；迁移锁只支持 sqlite 或 mysql");
        }

        try
        {
            const std::string                                           lockName   = migrationLockName(options.database);
            std::unique_ptr<AsynGyanis::Database::DatabaseConnection>   connection = Common::Database::openDirect(options);
            const std::array<AsynGyanis::Database::DatabaseValue, 2>    parameters{lockName, static_cast<std::int64_t>(timeoutSeconds)};
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
                return std::unexpected(std::string{"MySQL GET_LOCK 未返回结果行"} + "（锁名 " + lockName + "）");
            }
            const AsynGyanis::Database::DatabaseValue value = result->getValue(0);
            if (std::holds_alternative<std::monostate>(value))
            {
                const std::string reason = connection->lastError();
                connection->disconnect();
                // GET_LOCK 返回 NULL 表示语句执行出错，而不是「等锁超时」（后者返回 0）
                return std::unexpected(std::string{"MySQL GET_LOCK 返回 NULL（锁名 "} + lockName + "）：命名锁请求执行出错，请检查锁名长度上限 64 与 MySQL 错误日志；连接错误：" +
                                       reason);
            }
            const auto *code = std::get_if<std::int64_t>(&value);
            if (code == nullptr)
            {
                connection->disconnect();
                return std::unexpected(std::string{"MySQL GET_LOCK 返回值不是整数（锁名 "} + lockName + "，实际类型：" + AsynGyanis::Database::databaseValueTypeName(value) + "）");
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
            return std::unique_ptr<MigrationLock>(new MigrationLock(std::move(connection), lockName));
        } catch (const std::exception &failure)
        {
            return std::unexpected("迁移锁请求抛出异常：" + std::string{failure.what()});
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
            const std::array<AsynGyanis::Database::DatabaseValue, 1>    parameters{m_lockName};
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
        m_lockName.clear();
    }
} // namespace EdgePulse::System
