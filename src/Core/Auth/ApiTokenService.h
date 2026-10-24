/**
 * @file ApiTokenService.h
 * @brief API 令牌的签发、轮换、吊销与鉴权
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Database/Database.h"
#include "Core/Auth/ApiTokenRepository.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/SessionStore.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace EdgePulse::System
{
    class TenantRepository;
    class UserRepository;
} // namespace EdgePulse::System

namespace EdgePulse::Auth
{
    /**
     * @brief 对外可见的令牌摘要；不含哈希与重叠窗口等内部列
     */
    struct ApiTokenInfo
    {
        std::int64_t             id{0};      ///< 主键
        std::string              name;       ///< 管理名
        std::string              prefix;     ///< 明文前 12 个字符，供列表辨识
        std::vector<std::string> scopes;     ///< 权限范围
        std::string              expiresAt;  ///< 到期时间，UTC
        std::string              lastUsedAt; ///< 最近一次成功鉴权时间，UTC
        std::string              revokedAt;  ///< 吊销时间，UTC
        std::string              createdAt;  ///< 创建时间，UTC
    };

    /**
     * @brief 创建或轮换的产物；plaintext 只在这一次返回里出现
     */
    struct ApiTokenCreated
    {
        ApiTokenInfo token;              ///< 已落库的对外摘要
        std::string  plaintext;          ///< 完整明文令牌，调用方不得持久化
        std::string  previousValidUntil; ///< 轮换后旧明文仍可用的截止时间，创建时为空
    };

    /**
     * @brief API 令牌服务
     * @note 权限按 owner 当前角色实时计算；令牌只在其归属租户内生效
     */
    class ApiTokenService
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         * @param users 用户仓储，用于实时读取 owner 与权限
         * @param tenants 租户注册表，用于判断租户启停
         */
        ApiTokenService(std::shared_ptr<Common::Database> database, std::shared_ptr<System::UserRepository> users, std::shared_ptr<System::TenantRepository> tenants);

        /**
         * @brief 为当前身份签发一枚令牌
         * @param loop 恢复协程用的事件循环
         * @param session 当前身份，令牌归属该用户与其租户
         * @param name 管理名，长度 1 到 64 个字符
         * @param scopes 权限范围，必须非空、去重且不超出调用者的可授予范围
         * @param expiresInSeconds 有效期，0 表示默认 30 天；其余取值限 300 到 31536000
         * @return 成功返回对外摘要与一次性明文；校验失败返回可恢复错误
         * @note 浏览器会话按 owner 当前权限校验；令牌会话只能下放自身 permissions 内的 scope
         */
        AsynGyanis::Core::Task<std::expected<ApiTokenCreated, AuthError>> create(AsynGyanis::Core::EventLoop &loop, const SessionRecord &session, std::string name,
                                                                                 std::vector<std::string> scopes, int expiresInSeconds = 0);

        /**
         * @brief 列出当前租户的全部令牌
         * @param loop 恢复协程用的事件循环
         * @param session 当前身份；租户取 session 的租户，不接受客户端覆盖
         * @return std::vector<ApiTokenInfo> 按创建时间倒序的对外摘要
         */
        AsynGyanis::Core::Task<std::vector<ApiTokenInfo>> list(AsynGyanis::Core::EventLoop &loop, const SessionRecord &session);

        /**
         * @brief 吊销一条令牌，不物理删除
         * @param loop 恢复协程用的事件循环
         * @param session 当前身份；租户取 session 的租户
         * @param id 令牌标识
         * @return bool 是否命中一条未吊销且调用者有权管理的记录
         * @note 令牌会话只能吊销本人且 scope 不宽于自身的令牌；浏览器会话可管理本租户令牌
         */
        AsynGyanis::Core::Task<bool> revoke(AsynGyanis::Core::EventLoop &loop, const SessionRecord &session, std::int64_t id);

        /**
         * @brief 轮换一条令牌，保留原到期时间并开启一小时旧哈希重叠窗口
         * @param loop 恢复协程用的事件循环
         * @param session 当前身份；只能轮换本租户内自己有权管理的令牌
         * @param id 令牌标识
         * @return 成功返回对外摘要与新明文；令牌不存在、已吊销、已过期或并发冲突返回可恢复错误
         */
        AsynGyanis::Core::Task<std::expected<ApiTokenCreated, AuthError>> rotate(AsynGyanis::Core::EventLoop &loop, const SessionRecord &session, std::int64_t id);

        /**
         * @brief 按明文令牌重建身份快照
         * @param loop 恢复协程用的事件循环
         * @param token 完整明文令牌
         * @return std::optional<SessionRecord> 令牌可用时返回身份；否则为空
         * @note 依次检查令牌状态与重叠窗口、owner 状态、租户状态、owner 当前权限
         */
        AsynGyanis::Core::Task<std::optional<SessionRecord>> authenticate(AsynGyanis::Core::EventLoop &loop, std::string_view token);

    private:
        std::shared_ptr<System::UserRepository>   m_users;   ///< 用户仓储
        std::shared_ptr<System::TenantRepository> m_tenants; ///< 租户注册表
        ApiTokenRepository                        m_tokens;  ///< 令牌仓储
    };
} // namespace EdgePulse::Auth
