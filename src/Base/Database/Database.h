/**
 * @file Database.h
 * @brief SQLite 连接池与异步执行入口
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/EdgePulseException.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/Coroutine/Task.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Common
{
    /**
     * @brief 进程级数据库封装：持有连接池，并把阻塞 SQL 放到 AsyncExecutor 上执行
     * @note 对象必须活到所有借用它的协程完成之后；P0 由 Application 持有
     */
    class Database
    {
    public:
        /**
         * @brief 打开 SQLite 并建立连接池
         * @param databasePath 数据库文件路径；父目录不存在时自动创建
         * @param maximumPoolSize 连接数上限，P0 默认 4
         * @return 已就绪的数据库封装
         * @throws DatabaseException 文件路径不可写或首次连接失败
         * @note 应用应通过本函数创建实例，不要直接构造
         */
        [[nodiscard]] static std::shared_ptr<Database> openSqlite(std::string databasePath, std::size_t maximumPoolSize = 4);

        /**
         * @brief 打开一条不走连接池的 SQLite 连接，供启动期迁移使用
         * @param databasePath 数据库文件路径；父目录不存在时自动创建
         * @return 已连接的驱动实例
         * @throws DatabaseException 创建或连接失败
         */
        [[nodiscard]] static std::unique_ptr<AsynGyanis::Database::DatabaseConnection> openDirectSqlite(const std::string &databasePath);

        /**
         * @brief 在阻塞工作线程上借一条连接执行 work，完成后回到事件循环
         * @tparam ResultType work 的返回值，不能是 void
         * @param loop 恢复协程用的事件循环
         * @param work 在连接上执行的阻塞工作；入参连接只在本次调用内有效
         * @return 协程结果；连接池取不到连接或 work 抛出时，异常在等待处重新抛出
         * @note work 在工作线程上执行，不要在其中碰事件循环对象或协程帧
         */
        template<typename ResultType>
        AsynGyanis::Core::Task<ResultType> withConnection(AsynGyanis::Core::EventLoop &loop, std::function<ResultType(AsynGyanis::Database::DatabaseConnection &)> work)
        {
            co_return co_await AsynGyanis::Core::AsyncExecutor::shared().submit<ResultType>(
                    loop, std::function<ResultType()>(
                                  [this, work = std::move(work)]() -> ResultType
                                  {
                                      // 连接池为空或已停摆时立刻给出可操作原因，不让调用方等满借用超时
                                      auto lease = m_pool->acquire();
                                      if (!lease)
                                      {
                                          throw DatabaseException("数据库连接池取不到连接：请检查 SQLite 路径、连接上限与借用超时配置");
                                      }
                                      return work(*lease);
                                  }));
        }

        /**
         * @brief 连接池上限
         * @return std::size_t maximumPoolSize
         */
        [[nodiscard]] std::size_t maximumPoolSize() const noexcept;

        /**
         * @brief 建立一条探针连接验证路径可用，再创建连接池
         * @param databasePath 数据库文件路径
         * @param maximumPoolSize 连接数上限
         * @note 应用应通过 openSqlite() 创建共享实例；本构造公开是为了让 make_shared 可用
         */
        Database(std::string databasePath, std::size_t maximumPoolSize);

    private:
        std::string                                           m_databasePath;    ///< SQLite 文件路径
        std::size_t                                           m_maximumPoolSize; ///< 连接数上限
        std::unique_ptr<AsynGyanis::Database::ConnectionPool> m_pool;            ///< 懒惰建连的连接池
    };
} // namespace EdgePulse::Common
