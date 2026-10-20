/**
 * @file SessionStore.h
 * @brief 不透明会话令牌与登录身份的进程内存储
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace EdgePulse::Auth
{
    /**
     * @brief 一次登录的身份快照
     * @note 权限与角色在登录时缓存；P1 接入角色变更失效前，改动权限需重新登录
     */
    struct SessionRecord
    {
        std::string              token;           ///< 会话令牌
        std::int64_t             userId{0};       ///< 用户标识
        std::string              username;        ///< 登录名
        std::string              nickname;        ///< 显示名
        std::int64_t             departmentId{0}; ///< 部门标识
        std::string              departmentName;  ///< 部门名，P0 为空
        std::vector<std::string> roles;           ///< 角色码
        std::vector<std::string> permissions;     ///< 权限码
    };

    /**
     * @brief 进程内会话表
     * @note 滑动过期；所有公开方法线程安全
     */
    class SessionStore
    {
    public:
        using Clock = std::function<std::chrono::steady_clock::time_point()>;

        /**
         * @brief 构造
         * @param ttlSeconds 会话有效期，单位秒
         * @param clock 时钟注入；留空使用 steady_clock
         */
        explicit SessionStore(int ttlSeconds, Clock clock = {});

        /**
         * @brief 创建会话
         * @param record 登录身份；其中的 token 会被忽略并重新生成
         * @return 带有新 token 的快照
         * @throws Base::Exception 随机源不可用
         */
        [[nodiscard]] SessionRecord create(SessionRecord record);

        /**
         * @brief 校验令牌并把有效期推到 now + TTL
         * @param token 会话令牌
         * @return 命中且未过期时返回身份快照
         */
        [[nodiscard]] std::optional<SessionRecord> validateAndTouch(std::string_view token);

        /**
         * @brief 删除会话
         * @param token 会话令牌
         */
        void remove(std::string_view token);

        /**
         * @brief 会话有效期
         */
        [[nodiscard]] int ttlSeconds() const noexcept;

        /**
         * @brief 当前会话条数，主要用于测试与诊断
         */
        [[nodiscard]] std::size_t size() const;

    private:
        struct Entry
        {
            SessionRecord                         record;    ///< 身份快照
            std::chrono::steady_clock::time_point expiresAt; ///< 过期时刻
        };

        [[nodiscard]] std::chrono::steady_clock::time_point now() const;

        int                                    m_ttlSeconds; ///< 有效期
        Clock                                  m_clock;      ///< 时钟
        mutable std::mutex                     m_mutex;      ///< 保护会话表
        std::unordered_map<std::string, Entry> m_sessions;   ///< token -> 会话
    };
} // namespace EdgePulse::Auth
