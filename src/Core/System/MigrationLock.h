/**
 * @file MigrationLock.h
 * @brief 跨实例串行化迁移的数据库锁
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <expected>
#include <memory>
#include <string>

namespace AsynGyanis::Database
{
    class DatabaseConnection;
}

namespace EdgePulse::Common
{
    struct DatabaseOptions;
}

namespace EdgePulse::System
{
    /**
     * @brief 迁移期的互斥锁：MySQL 用命名锁，SQLite 按单写者直接放行
     * @details MySQL 通过独立直连执行 GET_LOCK，连接存活期间持锁，析构或 release 时
     *          执行 RELEASE_LOCK 并断开；SQLite 不承诺多实例，不在这里假装互斥。
     */
    class MigrationLock
    {
    public:
        /**
         * @brief 获取迁移锁
         * @param options 数据库连接参数，决定 MySQL 直连还是 SQLite 直通
         * @param timeoutSeconds 等待秒数；0 表示立即返回
         * @return 成功返回持有锁的对象；失败返回中文原因
         * @note SQLite 总是成功，并记录一条跳过锁的日志
         */
        [[nodiscard]] static std::expected<std::unique_ptr<MigrationLock>, std::string> acquire(const Common::DatabaseOptions &options, int timeoutSeconds = 30);

        /**
         * @brief 释放锁并断开直连
         * @note 幂等且不抛异常；未持有锁时什么也不做
         */
        ~MigrationLock();

        MigrationLock(const MigrationLock &)            = delete;
        MigrationLock &operator=(const MigrationLock &) = delete;
        MigrationLock(MigrationLock &&)                 = delete;
        MigrationLock &operator=(MigrationLock &&)      = delete;

        /**
         * @brief 显式释放锁
         * @note 幂等；析构会再调用一次
         */
        void release() noexcept;

    private:
        /**
         * @brief 接管一条已拿到 GET_LOCK 的连接
         * @param connection 直连；SQLite 路径为空
         */
        explicit MigrationLock(std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection) noexcept;

        std::unique_ptr<AsynGyanis::Database::DatabaseConnection> m_connection;  ///< 持锁连接；空表示 SQLite 直通
        bool                                                      m_held{false}; ///< 是否仍持有 MySQL 命名锁
    };
} // namespace EdgePulse::System
