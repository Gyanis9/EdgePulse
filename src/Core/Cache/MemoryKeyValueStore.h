/**
 * @file MemoryKeyValueStore.h
 * @brief 进程内键值存储
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Cache/KeyValueStore.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace EdgePulse::Cache
{
    /**
     * @brief 进程内键值存储，供单进程与测试使用
     * @note 所有方法线程安全；时钟可注入，用于钉住过期行为
     */
    class MemoryKeyValueStore : public KeyValueStore
    {
    public:
        using Clock = std::function<std::chrono::steady_clock::time_point()>;

        /**
         * @brief 构造
         * @param clock 时钟注入；留空使用 steady_clock
         */
        explicit MemoryKeyValueStore(Clock clock = {});

        /**
         * @brief 读取键值
         * @param loop 恢复协程用的事件循环
         * @param key 键
         * @return std::optional<std::string> 值；不存在或已过期时为空
         */
        [[nodiscard]] AsynGyanis::Core::Task<std::optional<std::string>> get(AsynGyanis::Core::EventLoop &loop, std::string key) override;

        /**
         * @brief 读取并删除键
         * @details 重写 KeyValueStore::take()：查找、过期判定与 erase 在同一把互斥锁内完成，
         *          并发调用不会把同一个值交给两个调用方。
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
         * @return bool 恒为 true
         */
        AsynGyanis::Core::Task<bool> set(AsynGyanis::Core::EventLoop &loop, std::string key, std::string value, std::chrono::seconds ttl) override;

        /**
         * @brief 删除键
         * @param loop 恢复协程用的事件循环
         * @param key 键
         * @return bool 是否命中
         */
        AsynGyanis::Core::Task<bool> erase(AsynGyanis::Core::EventLoop &loop, std::string key) override;

        /**
         * @brief 原子自增
         * @param loop 恢复协程用的事件循环
         * @param key 键
         * @param ttl 首次创建时的有效期
         * @return std::int64_t 自增后的值
         */
        AsynGyanis::Core::Task<std::int64_t> increment(AsynGyanis::Core::EventLoop &loop, std::string key, std::chrono::seconds ttl) override;

        /**
         * @brief 后端名称
         * @return std::string_view memory
         */
        [[nodiscard]] std::string_view backendName() const noexcept override;

        /**
         * @brief 当前有效键数量
         * @return std::size_t 键数
         */
        [[nodiscard]] std::size_t size() const;

    private:
        struct Entry
        {
            std::string                                          value;     ///< 值
            std::optional<std::chrono::steady_clock::time_point> expiresAt; ///< 过期时刻；空表示不过期
        };

        [[nodiscard]] std::chrono::steady_clock::time_point now() const;
        [[nodiscard]] bool                                  isExpired(const Entry &entry, std::chrono::steady_clock::time_point moment) const;

        Clock                                  m_clock;   ///< 时钟
        mutable std::mutex                     m_mutex;   ///< 保护键表
        std::unordered_map<std::string, Entry> m_entries; ///< 键表
    };
} // namespace EdgePulse::Cache
