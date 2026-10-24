/**
 * @file SessionStore.h
 * @brief 不透明会话令牌与登录身份存储
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Cache/KeyValueStore.h"
#include "Core/System/TenantScope.h"

#include <Core/Coroutine/Task.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Auth
{
    /**
     * @brief 一次登录的身份快照
     * @note 权限与角色在登录时缓存；角色变更后需重新登录才生效
     */
    struct SessionRecord
    {
        std::string              token;                 ///< 会话令牌
        std::int64_t             userId{0};             ///< 用户标识
        std::int64_t             tenantId{1};           ///< 归属租户
        std::string              tenantCode{"default"}; ///< 租户编码
        std::string              tenantName;            ///< 租户名称
        bool                     platformAdmin{false};  ///< 是否平台管理员
        bool                     apiToken{false};       ///< 是否由 API 令牌换来的身份；此类会话不写入会话表
        std::string              username;              ///< 登录名
        std::string              nickname;              ///< 显示名
        std::int64_t             departmentId{0};       ///< 部门标识
        std::string              departmentName;        ///< 部门名
        std::vector<std::string> roles;                 ///< 角色码
        std::vector<std::string> permissions;           ///< 权限码

        /// 本次会话的租户边界
        [[nodiscard]] System::TenantScope tenantScope() const noexcept
        {
            return System::TenantScope{tenantId, platformAdmin};
        }
    };

    /**
     * @brief 会话表：内存或 Redis 由注入的键值存储决定
     * @note 滑动过期；键值存储实例必须比本对象活得久
     */
    class SessionStore
    {
    public:
        /**
         * @brief 构造
         * @param store 键值存储
         * @param ttlSeconds 会话有效期，单位秒
         */
        SessionStore(std::shared_ptr<Cache::KeyValueStore> store, int ttlSeconds);

        /**
         * @brief 创建会话
         * @param loop 恢复协程用的事件循环
         * @param record 登录身份；其中的 token 会被忽略并重新生成
         * @return SessionRecord 带有新 token 的快照
         * @throws Base::Exception 随机源不可用或存储失败
         */
        [[nodiscard]] AsynGyanis::Core::Task<SessionRecord> create(AsynGyanis::Core::EventLoop &loop, SessionRecord record);

        /**
         * @brief 校验令牌并把有效期推到 now + TTL
         * @param loop 恢复协程用的事件循环
         * @param token 会话令牌
         * @return std::optional<SessionRecord> 命中且未过期时返回身份快照
         */
        [[nodiscard]] AsynGyanis::Core::Task<std::optional<SessionRecord>> validateAndTouch(AsynGyanis::Core::EventLoop &loop, std::string_view token);

        /**
         * @brief 删除会话
         * @param loop 恢复协程用的事件循环
         * @param token 会话令牌
         * @return bool 是否成功
         */
        AsynGyanis::Core::Task<bool> remove(AsynGyanis::Core::EventLoop &loop, std::string_view token);

        /**
         * @brief 会话有效期
         * @return int 秒数
         */
        [[nodiscard]] int ttlSeconds() const noexcept;

        /**
         * @brief 运行期更新会话 TTL
         * @param ttlSeconds 新有效期；非正数被忽略
         */
        void setTtlSeconds(int ttlSeconds) noexcept;

    private:
        std::shared_ptr<Cache::KeyValueStore> m_store;      ///< 键值存储
        std::atomic<int>                      m_ttlSeconds; ///< 有效期
    };
} // namespace EdgePulse::Auth
