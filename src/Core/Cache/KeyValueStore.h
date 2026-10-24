/**
 * @file KeyValueStore.h
 * @brief 会话、验证码与限流计数共用的异步键值存储契约
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <Core/Coroutine/Task.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Cache
{
    /**
     * @brief 带过期时间的键值存储
     * @details 所有方法都返回协程：内存实现立即完成，Redis 实现把阻塞网络调用交给
     *          AsyncExecutor，调用方不感知后端差异。
     */
    class KeyValueStore
    {
    public:
        virtual ~KeyValueStore() = default;

        KeyValueStore(const KeyValueStore &)            = delete;
        KeyValueStore &operator=(const KeyValueStore &) = delete;

        /**
         * @brief 读取键值
         * @param loop 恢复协程用的事件循环
         * @param key 键
         * @return std::optional<std::string> 值；不存在或已过期时为空
         */
        [[nodiscard]] virtual AsynGyanis::Core::Task<std::optional<std::string>> get(AsynGyanis::Core::EventLoop &loop, std::string key) = 0;

        /**
         * @brief 读取并删除键，取值与删除原子完成
         * @param loop 恢复协程用的事件循环
         * @param key 键
         * @return std::optional<std::string> 删除前的值；不存在或已过期时为空
         * @note 供一次性凭据消费使用：值只会交给一个调用方，调用方不必再补一次 erase
         */
        [[nodiscard]] virtual AsynGyanis::Core::Task<std::optional<std::string>> take(AsynGyanis::Core::EventLoop &loop, std::string key) = 0;

        /**
         * @brief 写入并设置过期时间
         * @param loop 恢复协程用的事件循环
         * @param key 键
         * @param value 值
         * @param ttl 有效期，必须为正
         * @return bool 是否成功
         */
        virtual AsynGyanis::Core::Task<bool> set(AsynGyanis::Core::EventLoop &loop, std::string key, std::string value, std::chrono::seconds ttl) = 0;

        /**
         * @brief 删除键
         * @param loop 恢复协程用的事件循环
         * @param key 键
         * @return bool 是否成功
         */
        virtual AsynGyanis::Core::Task<bool> erase(AsynGyanis::Core::EventLoop &loop, std::string key) = 0;

        /**
         * @brief 原子自增；键不存在时从 1 开始并设置过期时间
         * @param loop 恢复协程用的事件循环
         * @param key 键
         * @param ttl 首次创建时的有效期
         * @return std::int64_t 自增后的值
         */
        virtual AsynGyanis::Core::Task<std::int64_t> increment(AsynGyanis::Core::EventLoop &loop, std::string key, std::chrono::seconds ttl) = 0;

        /**
         * @brief 后端名称，用于日志与诊断
         * @return std::string_view memory 或 redis
         */
        [[nodiscard]] virtual std::string_view backendName() const noexcept = 0;

    protected:
        KeyValueStore() = default;
    };
} // namespace EdgePulse::Cache
