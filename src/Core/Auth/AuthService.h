/**
 * @file AuthService.h
 * @brief 登录、会话、路由生成与改密的应用服务
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Auth/CaptchaService.h"
#include "Core/Auth/LoginAudit.h"
#include "Core/Auth/SessionStore.h"
#include "Base/Database/Database.h"
#include "Core/System/MenuRepository.h"
#include "Core/System/UserRepository.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace EdgePulse::Auth
{
    /**
     * @brief 认证失败的可恢复错误
     */
    struct AuthError
    {
        int         httpStatus{400}; ///< HTTP 状态码
        int         code{400};       ///< 业务码
        std::string message;         ///< 中文可操作说明
    };

    /**
     * @brief 一次登录的输入
     */
    struct LoginCredentials
    {
        std::string username;    ///< 登录名
        std::string password;    ///< 明文口令
        std::string captcha;     ///< 验证码答案
        std::string captchaUuid; ///< 验证码标识
        std::string clientIp;    ///< 来源 IP
        std::string traceId;     ///< 请求标识
    };

    /**
     * @brief 登录成功的输出
     */
    struct LoginResult
    {
        std::string token;        ///< 不透明会话令牌
        int         expiresIn{0}; ///< 有效期，单位秒
    };

    /**
     * @brief 认证服务
     * @note 依赖的会话、验证码与审计对象都必须比本对象活得久
     */
    class AuthService
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         * @param sessions 会话表
         * @param captcha 验证码服务
         * @param audit 登录审计
         */
        AuthService(std::shared_ptr<Common::Database> database, std::shared_ptr<SessionStore> sessions, std::shared_ptr<CaptchaService> captcha, std::shared_ptr<LoginAudit> audit);

        /**
         * @brief 验证码 + 口令登录
         * @param loop 恢复协程用的事件循环
         * @param credentials 登录输入
         * @return 成功返回 token 与有效期；失败返回可恢复错误
         */
        AsynGyanis::Core::Task<std::expected<LoginResult, AuthError>> login(AsynGyanis::Core::EventLoop &loop, LoginCredentials credentials);

        /**
         * @brief 校验令牌并滑动续期
         * @param token 会话令牌
         * @return 命中且未过期时返回身份快照
         */
        [[nodiscard]] std::optional<SessionRecord> authenticate(std::string_view token);

        /**
         * @brief 删除会话
         * @param token 会话令牌
         */
        void logout(std::string_view token);

        /**
         * @brief 取用户可见的菜单树
         * @param loop 恢复协程用的事件循环
         * @param session 已认证身份
         * @return 根菜单列表
         */
        AsynGyanis::Core::Task<std::vector<System::MenuNode>> routes(AsynGyanis::Core::EventLoop &loop, const SessionRecord &session);

        /**
         * @brief 修改口令
         * @param loop 恢复协程用的事件循环
         * @param session 已认证身份
         * @param oldPassword 原口令
         * @param newPassword 新口令，长度 6..64
         * @return 成功为空；失败为可恢复错误
         */
        AsynGyanis::Core::Task<std::expected<void, AuthError>> changePassword(AsynGyanis::Core::EventLoop &loop, const SessionRecord &session, std::string oldPassword,
                                                                              std::string newPassword);

        /**
         * @brief 判断身份是否拥有权限码
         * @param session 已认证身份
         * @param permission 权限码
         * @return true 命中
         */
        [[nodiscard]] bool hasPermission(const SessionRecord &session, std::string_view permission) const;

        /**
         * @brief 会话有效期
         */
        [[nodiscard]] int sessionTtlSeconds() const noexcept;

    private:
        /**
         * @brief 写审计；失败只记日志，不改变登录结果
         */
        AsynGyanis::Core::Task<> recordAudit(AsynGyanis::Core::EventLoop &loop, LoginAuditEntry entry);

        std::shared_ptr<SessionStore>   m_sessions;   ///< 会话表
        std::shared_ptr<CaptchaService> m_captcha;    ///< 验证码
        std::shared_ptr<LoginAudit>     m_loginAudit; ///< 登录审计
        System::UserRepository          m_users;      ///< 用户仓储
        System::MenuRepository          m_menus;      ///< 菜单仓储
    };
} // namespace EdgePulse::Auth
