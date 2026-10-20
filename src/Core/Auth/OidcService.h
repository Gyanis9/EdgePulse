/**
 * @file OidcService.h
 * @brief OIDC 授权码登录：发现、PKCE、ID Token 校验与会话签发
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Auth/Jwt.h"

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}
namespace EdgePulse::Cache
{
    class KeyValueStore;
}
namespace EdgePulse::System
{
    class UserRepository;
    class TenantRepository;
} // namespace EdgePulse::System

namespace EdgePulse::Auth
{
    class AuthService;

    /**
     * @brief OIDC 配置
     */
    struct OidcConfiguration
    {
        bool         enabled{false};                 ///< 是否启用
        std::string  issuer;                         ///< 签发者基地址
        std::string  clientId;                       ///< 客户端标识
        std::string  clientSecret;                   ///< 客户端密钥，只从环境变量读取；空表示公共客户端
        std::string  redirectUri;                    ///< 回调地址
        std::string  frontendRedirect{"/login"};     ///< 回调后回到的前端路径
        std::string  scopes{"openid profile email"}; ///< 申请的 scope
        bool         autoProvision{false};           ///< 首次登录是否自动建号
        std::int64_t defaultRoleId{0};               ///< 自动建号时分配的角色
        std::int64_t defaultDepartmentId{101};       ///< 自动建号时的部门
        std::string  allowedEmailDomains;            ///< 允许的邮箱域，逗号分隔；空表示不限制
        std::string  tenantCode{"default"};          ///< 登录与自动建号归属的租户编码
    };

    /**
     * @brief 发起登录的结果
     */
    struct OidcStartResult
    {
        bool        ok{false};   ///< 是否拿到授权地址
        std::string redirectUrl; ///< 跳转到 IdP 的地址
        std::string error;       ///< 中文失败原因
    };

    /**
     * @brief 回调处理结果
     */
    struct OidcCallbackResult
    {
        bool        ok{false};   ///< 是否完成身份校验
        std::string redirectUrl; ///< 回到前端的地址，带一次性 code 或错误
        std::string error;       ///< 中文失败原因
    };

    /**
     * @brief 一次性 code 换会话的结果
     */
    struct OidcExchangeResult
    {
        bool        ok{false};    ///< 是否签发成功
        std::string token;        ///< 会话令牌
        int         expiresIn{0}; ///< 有效期
        std::string returnPath;   ///< 登录前想去的路径
        std::string error;        ///< 中文失败原因
    };

    /**
     * @brief OIDC 服务
     * @note 只在服务器所属事件循环上调用
     */
    class OidcService
    {
    public:
        /**
         * @brief 构造
         * @param configuration 配置
         * @param users 用户仓储
         * @param tenants 租户仓储，用于把外部身份绑定到配置的租户
         * @param cache 状态与一次性 code 的存储
         * @param auth 认证服务，用于签发会话
         */
        OidcService(OidcConfiguration configuration, System::UserRepository &users, System::TenantRepository &tenants, Cache::KeyValueStore &cache, AuthService &auth);

        /**
         * @brief 生成 state、nonce 与 PKCE，返回 IdP 授权地址
         * @param loop 恢复协程用的事件循环
         * @param returnPath 登录后回到的前端路径
         * @return OidcStartResult 结果
         */
        AsynGyanis::Core::Task<OidcStartResult> start(AsynGyanis::Core::EventLoop &loop, std::string returnPath);

        /**
         * @brief 处理 IdP 回调：换 token、校验 ID Token、登记一次性 code
         * @param loop 恢复协程用的事件循环
         * @param code 授权码
         * @param state 状态串
         * @return OidcCallbackResult 结果；redirectUrl 指回前端
         */
        AsynGyanis::Core::Task<OidcCallbackResult> handleCallback(AsynGyanis::Core::EventLoop &loop, std::string code, std::string state);

        /**
         * @brief 用一次性 code 签发 EdgePulse 会话
         * @param loop 恢复协程用的事件循环
         * @param code 一次性 code
         * @param clientIp 来源 IP
         * @param traceId 请求标识
         * @return OidcExchangeResult 结果
         */
        AsynGyanis::Core::Task<OidcExchangeResult> exchange(AsynGyanis::Core::EventLoop &loop, std::string code, std::string clientIp, std::string traceId);

        /// 是否启用
        [[nodiscard]] bool enabled() const noexcept
        {
            return m_configuration.enabled;
        }

    private:
        struct Discovery
        {
            std::string issuer;                ///< 实际签发者
            std::string authorizationEndpoint; ///< 授权端点
            std::string tokenEndpoint;         ///< 令牌端点
            std::string jwksUri;               ///< JWKS 地址
        };

        /**
         * @brief 读取并缓存 OIDC 发现文档
         */
        AsynGyanis::Core::Task<std::optional<Discovery>> discovery(AsynGyanis::Core::EventLoop &loop, std::string &error);

        /**
         * @brief 拉取 JWKS
         */
        AsynGyanis::Core::Task<std::vector<JwtRsaKey>> fetchJwks(AsynGyanis::Core::EventLoop &loop, const std::string &jwksUri, std::string &error);

        /**
         * @brief 用授权码换 ID Token
         */
        AsynGyanis::Core::Task<std::optional<std::string>> exchangeCode(AsynGyanis::Core::EventLoop &loop, const Discovery &discovery, std::string_view code,
                                                                        std::string_view verifier, std::string &error);

        /**
         * @brief 按邮箱查用户，必要时自动建号
         */
        AsynGyanis::Core::Task<std::optional<std::int64_t>> resolveUser(AsynGyanis::Core::EventLoop &loop, const JwtClaims &claims, std::string &error);

        [[nodiscard]] bool emailDomainAllowed(std::string_view email) const;

        OidcConfiguration         m_configuration;
        System::UserRepository   &m_users;
        System::TenantRepository &m_tenants;
        Cache::KeyValueStore     &m_cache;
        AuthService              &m_auth;
        std::optional<Discovery>  m_discovery;
    };
} // namespace EdgePulse::Auth
