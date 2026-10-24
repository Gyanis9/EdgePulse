/**
 * @file AuthService.h
 * @brief 登录、会话、路由生成与改密的应用服务
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Auth/CaptchaService.h"
#include "Core/Auth/LoginAudit.h"
#include "Core/Auth/SessionStore.h"
#include "Core/Cache/KeyValueStore.h"
#include "Base/Database/Database.h"
#include "Base/Security/PasswordPolicy.h"
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

namespace EdgePulse::System
{
    class TenantRepository;
}

namespace EdgePulse::Auth
{
    class ApiTokenService;

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
        std::string username;              ///< 登录名
        std::string password;              ///< 明文口令
        std::string captcha;               ///< 验证码答案
        std::string captchaUuid;           ///< 验证码标识
        std::string clientIp;              ///< 来源 IP
        std::string traceId;               ///< 请求标识
        std::string tenantCode{"default"}; ///< 租户编码；空表示默认租户
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
     * @brief 认证行为的可配置策略
     * @note 默认值即生产档：建号与改密至少 10 位且要求四类字符，账号连续失败 5 次锁定 15 分钟
     */
    struct AuthOptions
    {
        int  passwordMinimumLength{10};       ///< 口令最短长度
        bool passwordRequireComplexity{true}; ///< 是否要求大写、小写、数字与符号
        int  loginFailureWindowSeconds{900};  ///< 账号失败计数的滑动窗口，单位秒
        int  loginMaximumFailures{5};         ///< 触发锁定的连续失败次数
        int  loginLockSeconds{900};           ///< 锁定时长，单位秒
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
         * @param cache 登录失败计数使用的键值存储
         * @param tenants 租户注册表
         * @param options 口令策略与账号锁定参数
         */
        AuthService(std::shared_ptr<Common::Database> database, std::shared_ptr<SessionStore> sessions, std::shared_ptr<CaptchaService> captcha, std::shared_ptr<LoginAudit> audit,
                    std::shared_ptr<Cache::KeyValueStore> cache, std::shared_ptr<System::TenantRepository> tenants, AuthOptions options = {});

        /**
         * @brief 挂接 API 令牌服务，使会话未命中时继续按令牌认证
         * @param apiTokens 令牌服务；空表示只接受会话令牌
         * @note 只在启动装配阶段调用一次，不做运行期替换；登录装配在用户仓储之后完成，故用迟绑定而不是调整构造参数顺序
         */
        void setApiTokenService(std::shared_ptr<ApiTokenService> apiTokens);

        /**
         * @brief 验证码 + 口令登录
         * @param loop 恢复协程用的事件循环
         * @param credentials 登录输入
         * @return 成功返回 token 与有效期；失败返回可恢复错误
         */
        AsynGyanis::Core::Task<std::expected<LoginResult, AuthError>> login(AsynGyanis::Core::EventLoop &loop, LoginCredentials credentials);

        /**
         * @brief 为已确认的外部身份签发会话
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @param clientIp 来源 IP
         * @param traceId 请求标识
         * @param auditMessage 登录审计文案
         * @return 成功返回 token 与有效期；账号不存在或停用时返回 401
         * @note 调用方必须已完成外部身份校验（如 OIDC）
         */
        AsynGyanis::Core::Task<std::expected<LoginResult, AuthError>> issueSessionForUser(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, std::string clientIp,
                                                                                          std::string traceId, std::string auditMessage);

        /**
         * @brief 校验令牌并滑动续期
         * @param loop 恢复协程用的事件循环
         * @param token 会话令牌
         * @return 命中且未过期时返回身份快照
         */
        [[nodiscard]] AsynGyanis::Core::Task<std::optional<SessionRecord>> authenticate(AsynGyanis::Core::EventLoop &loop, std::string_view token);

        /**
         * @brief 删除会话
         * @param loop 恢复协程用的事件循环
         * @param token 会话令牌
         * @return bool 是否删除成功
         */
        AsynGyanis::Core::Task<bool> logout(AsynGyanis::Core::EventLoop &loop, std::string_view token);

        /**
         * @brief 清掉租户状态缓存，使停用在下一次鉴权时生效
         * @param loop 恢复协程用的事件循环
         * @param tenantId 租户标识
         */
        AsynGyanis::Core::Task<void> invalidateTenant(AsynGyanis::Core::EventLoop &loop, std::int64_t tenantId);

        /**
         * @brief 取用户可见的菜单树
         * @param loop 恢复协程用的事件循环
         * @param session 已认证身份
         * @return 根菜单列表
         */
        AsynGyanis::Core::Task<std::vector<System::MenuNode>> routes(AsynGyanis::Core::EventLoop &loop, const SessionRecord &session);

        /**
         * @brief 按当前策略校验一条新口令
         * @param password 待校验明文
         * @return 成功为空；失败为中文可操作说明
         * @note 建号、重置与改密共用同一策略，调用方负责把错误文案交给用户
         */
        [[nodiscard]] std::expected<void, std::string> validatePassword(std::string_view password) const;

        /**
         * @brief 修改口令
         * @param loop 恢复协程用的事件循环
         * @param session 已认证身份
         * @param oldPassword 原口令
         * @param newPassword 新口令，长度与字符类别由策略决定
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
         * @brief 为通过认证的用户建立会话、更新登录时间并写审计
         * @param loop 恢复协程用的事件循环
         * @param user 用户行
         * @param clientIp 来源 IP
         * @param traceId 请求标识
         * @param auditMessage 登录审计文案
         * @return 会话令牌与有效期
         */
        AsynGyanis::Core::Task<std::expected<LoginResult, AuthError>> createSessionForUser(AsynGyanis::Core::EventLoop &loop, const System::UserRecord &user,
                                                                                           const std::string &clientIp, const std::string &traceId,
                                                                                           const std::string &auditMessage);

        /**
         * @brief 写审计；失败只记日志，不改变登录结果
         */
        AsynGyanis::Core::Task<> recordAudit(AsynGyanis::Core::EventLoop &loop, LoginAuditEntry entry);

        /**
         * @brief 判断租户是否启用，状态经短 TTL 缓存
         * @param loop 恢复协程用的事件循环
         * @param tenantId 租户标识
         * @return bool 租户存在且启用
         */
        AsynGyanis::Core::Task<bool> tenantEnabled(AsynGyanis::Core::EventLoop &loop, std::int64_t tenantId);

        std::shared_ptr<SessionStore>             m_sessions;       ///< 会话表
        std::shared_ptr<CaptchaService>           m_captcha;        ///< 验证码
        std::shared_ptr<LoginAudit>               m_loginAudit;     ///< 登录审计
        std::shared_ptr<Cache::KeyValueStore>     m_cache;          ///< 登录失败计数
        std::shared_ptr<System::TenantRepository> m_tenants;        ///< 租户注册表
        std::shared_ptr<ApiTokenService>          m_apiTokens;      ///< API 令牌；空表示未启用
        System::UserRepository                    m_users;          ///< 用户仓储
        System::MenuRepository                    m_menus;          ///< 菜单仓储
        AuthOptions                               m_options;        ///< 口令与锁定策略
        Common::PasswordPolicy                    m_passwordPolicy; ///< 由 m_options 构造的校验器
    };
} // namespace EdgePulse::Auth
