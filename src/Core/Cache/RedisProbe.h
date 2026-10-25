/**
 * @file RedisProbe.h
 * @brief 可恢复的 Redis 探针：PING 失败时只重连一次
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <string_view>

namespace AsynGyanis::Database
{
    class RedisConnection;
}

namespace EdgePulse::Cache
{
    /**
     * @brief 对一个 Redis 连接执行可恢复的 PING 探针
     * @details 连接未建立时先建连；已建立但 PING 失败时断开并只重连一次，再 PING。
     *          这是缓存的恢复路径：常规缓存操作不会静默重连。
     * @param connection 连接；调用方负责串行化，本函数不持锁
     * @param endpoint 日志端点 host:port，不得带口令
     * @return bool 首次或重连后的 PING 成功为 true；建连或两次 PING 都失败为 false
     * @note 失败原因写日志，不含口令；本函数不抛异常
     */
    [[nodiscard]] bool probeRedisConnection(AsynGyanis::Database::RedisConnection &connection, std::string_view endpoint);
} // namespace EdgePulse::Cache
