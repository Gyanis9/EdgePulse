/**
 * @file RedisKeyValueStore.h
 * @brief Redis 后端的会话、验证码与限流计数存储
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Cache/KeyValueStore.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    class DatabaseResult;
    class RedisConnection;
} // namespace AsynGyanis::Database

namespace EdgePulse::Cache
{
    /**
     * @brief Redis 键值存储
     * @details 单连接加互斥锁，所有网络往返经 AsyncExecutor 移出事件循环；
     *          会话读写是低频操作，这里优先保证顺序与可诊断性，不做连接池。
     */
    class RedisKeyValueStore : public KeyValueStore
    {
    public:
        /**
         * @brief 建立并验证 Redis 连接
         * @param host 主机
         * @param port 端口
         * @param password 口令；空表示不认证
         * @param database 键空间编号
         * @throws Common::DatabaseException 连接失败
         */
        RedisKeyValueStore(std::string host, std::uint16_t port, std::string password, int database);

        ~RedisKeyValueStore() override;

        /**
         * @brief 读取键值
         * @param loop 恢复协程用的事件循环
         * @param key 键
         * @return std::optional<std::string> 值；不存在或已过期时为空
         */
        [[nodiscard]] AsynGyanis::Core::Task<std::optional<std::string>> get(AsynGyanis::Core::EventLoop &loop, std::string key) override;

        /**
         * @brief 读取并删除键
         * @details 重写 KeyValueStore::take()：单条 GETDEL 由服务端原子完成读取与删除，
         *          不存在或已过期的键回 nil。GETDEL 要求 Redis 6.2 以上，部署档为 7。
         * @param loop 恢复协程用的事件循环
         * @param key 键
         * @return std::optional<std::string> 删除前的值；不存在或已过期时为空
         */
        [[nodiscard]] AsynGyanis::Core::Task<std::optional<std::string>> take(AsynGyanis::Core::EventLoop &loop, std::string key) override;

        /**
         * @brief 写入并设置过期时间
         * @param loop 恢复协程用的事件循环
         * @param key 键
         * @param value 值
         * @param ttl 有效期
         * @return bool 是否成功
         */
        AsynGyanis::Core::Task<bool> set(AsynGyanis::Core::EventLoop &loop, std::string key, std::string value, std::chrono::seconds ttl) override;

        /**
         * @brief 删除键
         * @param loop 恢复协程用的事件循环
         * @param key 键
         * @return bool 是否成功
         */
        AsynGyanis::Core::Task<bool> erase(AsynGyanis::Core::EventLoop &loop, std::string key) override;

        /**
         * @brief 原子自增；首次创建时设置过期时间
         * @param loop 恢复协程用的事件循环
         * @param key 键
         * @param ttl 首次创建时的有效期
         * @return std::int64_t 自增后的值
         */
        AsynGyanis::Core::Task<std::int64_t> increment(AsynGyanis::Core::EventLoop &loop, std::string key, std::chrono::seconds ttl) override;

        /**
         * @brief 后端名称
         * @return std::string_view redis
         */
        [[nodiscard]] std::string_view backendName() const noexcept override;

    private:
        /**
         * @brief 在已持锁的工作线程上执行一条命令
         * @param command 命令名
         * @param arguments 参数
         * @return std::unique_ptr<AsynGyanis::Database::DatabaseResult> 结果
         * @throws Common::DatabaseException 连接或命令失败
         */
        [[nodiscard]] std::unique_ptr<AsynGyanis::Database::DatabaseResult> executeLocked(const std::string_view command, const std::vector<std::string> &arguments);

        std::string   m_host;     ///< 主机
        std::uint16_t m_port;     ///< 端口
        std::string   m_password; ///< 口令
        int           m_database; ///< 键空间

        std::mutex                                             m_mutex;      ///< 串行化单连接
        std::unique_ptr<AsynGyanis::Database::RedisConnection> m_connection; ///< 当前连接
    };
} // namespace EdgePulse::Cache
