#include "Base/Database/Database.h"

#include <filesystem>
#include <system_error>
#include <utility>

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
            // 相对文件名或 :memory: 没有父目录，无需创建
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
         * @brief 把应用配置翻译成驱动连接参数
         * @param options 应用连接参数
         * @return AsynGyanis::Database::ConnectionConfig 驱动参数
         */
        [[nodiscard]] AsynGyanis::Database::ConnectionConfig connectionConfigOf(const DatabaseOptions &options)
        {
            if (options.driver == "sqlite")
            {
                ensureParentDirectory(options.path);
                return AsynGyanis::Database::ConnectionConfig::sqliteDefault(options.path);
            }
            if (options.driver == "mysql")
            {
                AsynGyanis::Database::ConnectionConfig configuration = AsynGyanis::Database::ConnectionConfig::mySqlDefault();
                configuration.host                                   = options.host;
                configuration.port                                   = options.port;
                configuration.userName                               = options.userName;
                configuration.password                               = options.password;
                configuration.database                               = options.database;
                return configuration;
            }
            throw DatabaseException("不支持的数据库驱动：" + options.driver + "；可选 sqlite 或 mysql");
        }

        /**
         * @brief 创建并连接一条驱动实例
         * @param configuration 驱动参数
         * @return 已连接的驱动
         * @throws DatabaseException 驱动创建或连接失败
         */
        [[nodiscard]] std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connectDatabase(const AsynGyanis::Database::ConnectionConfig &configuration,
                                                                                                const DatabaseOptions                        &options)
        {
            std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = options.driver == "mysql" ? AsynGyanis::Database::DatabaseFactory::createMySql(configuration)
                                                                                                             : AsynGyanis::Database::DatabaseFactory::createSqlite(configuration);
            if (!connection)
            {
                throw DatabaseException("创建数据库驱动失败：" + options.driver);
            }
            if (!connection->connect())
            {
                throw DatabaseException("连接数据库失败：" + options.driver + "：" + connection->lastError());
            }
            return connection;
        }
    } // namespace

    Database::Database(std::string databasePath, const std::size_t maximumPoolSize) : m_databasePath(std::move(databasePath)), m_maximumPoolSize(maximumPoolSize)
    {
        DatabaseOptions options;
        options.driver          = "sqlite";
        options.path            = m_databasePath;
        options.maximumPoolSize = maximumPoolSize;
        initialize(options);
    }

    Database::Database(DatabaseOptions options) : m_databasePath(options.path), m_maximumPoolSize(options.maximumPoolSize)
    {
        initialize(options);
    }

    void Database::initialize(const DatabaseOptions &options)
    {
        const AsynGyanis::Database::ConnectionConfig configuration = connectionConfigOf(options);

        // 启动期先用一条独立连接验证可用；失败时给出干净原因，而不是等第一个请求 500
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> probe = connectDatabase(configuration, options);
        probe->disconnect();

        AsynGyanis::Database::PoolConfig poolConfiguration;
        poolConfiguration.maximumPoolSize            = m_maximumPoolSize;
        poolConfiguration.acquireTimeoutMilliseconds = 5000;
        poolConfiguration.probeBeforeAcquire         = options.driver == "mysql";
        m_pool = std::make_unique<AsynGyanis::Database::ConnectionPool>([configuration, options]() { return connectDatabase(configuration, options); }, poolConfiguration);
    }

    std::shared_ptr<Database> Database::open(const DatabaseOptions &options)
    {
        return std::make_shared<Database>(options);
    }

    std::unique_ptr<AsynGyanis::Database::DatabaseConnection> Database::openDirect(const DatabaseOptions &options)
    {
        return connectDatabase(connectionConfigOf(options), options);
    }

    std::shared_ptr<Database> Database::openSqlite(std::string databasePath, const std::size_t maximumPoolSize)
    {
        return std::make_shared<Database>(std::move(databasePath), maximumPoolSize);
    }

    std::unique_ptr<AsynGyanis::Database::DatabaseConnection> Database::openDirectSqlite(const std::string &databasePath)
    {
        DatabaseOptions options;
        options.driver = "sqlite";
        options.path   = databasePath;
        return openDirect(options);
    }

    std::size_t Database::maximumPoolSize() const noexcept
    {
        return m_maximumPoolSize;
    }

    bool Database::probe()
    {
        auto lease = m_pool->acquire();
        return lease && lease->probeAlive();
    }
} // namespace EdgePulse::Common
