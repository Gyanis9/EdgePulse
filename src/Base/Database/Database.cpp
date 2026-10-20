#include "Base/Database/Database.h"

#include <filesystem>
#include <system_error>

namespace EdgePulse::Common
{
    namespace
    {
        /**
         * @brief 确保 SQLite 文件的父目录存在
         * @param databasePath 数据库文件路径
         * @throws DatabaseException 目录创建失败
         */
        void ensureParentDirectory(const std::string &databasePath)
        {
            const std::filesystem::path parent = std::filesystem::path(databasePath).parent_path();
            // 相对文件名（例如 ":memory:" 或 "edgepulse.db"）没有父目录，无需创建
            if (parent.empty())
            {
                return;
            }
            std::error_code error;
            std::filesystem::create_directories(parent, error);
            if (error)
            {
                throw DatabaseException("创建 SQLite 目录失败：" + parent.string() + "：" + error.message());
            }
        }

        /**
         * @brief 创建并连接一条 SQLite 驱动实例
         * @param configuration 连接配置
         * @return 已连接的驱动
         * @throws DatabaseException 驱动创建或连接失败
         */
        [[nodiscard]] std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connectSqlite(const AsynGyanis::Database::ConnectionConfig &configuration)
        {
            std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = AsynGyanis::Database::DatabaseFactory::createSqlite(configuration);
            if (!connection)
            {
                throw DatabaseException("创建 SQLite 驱动失败：" + configuration.database);
            }
            if (!connection->connect())
            {
                throw DatabaseException("连接 SQLite 失败：" + configuration.database + "：" + connection->lastError());
            }
            return connection;
        }
    } // namespace

    Database::Database(std::string databasePath, const std::size_t maximumPoolSize) : m_databasePath(std::move(databasePath)), m_maximumPoolSize(maximumPoolSize)
    {
        ensureParentDirectory(m_databasePath);
        const AsynGyanis::Database::ConnectionConfig configuration = AsynGyanis::Database::ConnectionConfig::sqliteDefault(m_databasePath);

        // 启动期先用一条独立连接验证路径可写可读；失败时给出干净原因，而不是等第一个请求 500
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> probe = connectSqlite(configuration);
        probe->disconnect();

        AsynGyanis::Database::PoolConfig poolConfiguration;
        poolConfiguration.maximumPoolSize            = maximumPoolSize;
        poolConfiguration.acquireTimeoutMilliseconds = 5000;
        m_pool = std::make_unique<AsynGyanis::Database::ConnectionPool>([configuration]() { return connectSqlite(configuration); }, poolConfiguration);
    }

    std::shared_ptr<Database> Database::openSqlite(std::string databasePath, const std::size_t maximumPoolSize)
    {
        return std::make_shared<Database>(std::move(databasePath), maximumPoolSize);
    }

    std::unique_ptr<AsynGyanis::Database::DatabaseConnection> Database::openDirectSqlite(const std::string &databasePath)
    {
        ensureParentDirectory(databasePath);
        return connectSqlite(AsynGyanis::Database::ConnectionConfig::sqliteDefault(databasePath));
    }

    std::size_t Database::maximumPoolSize() const noexcept
    {
        return m_maximumPoolSize;
    }
} // namespace EdgePulse::Common
