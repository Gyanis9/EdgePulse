/**
 * @file AuthTestContext.h
 * @brief 认证测试共用的临时数据库与服务集合
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <filesystem>
#include <memory>

namespace EdgePulse::Common
{
    class Database;
}
namespace EdgePulse::Cache
{
    class MemoryKeyValueStore;
}
namespace EdgePulse::System
{
    class TenantRepository;
}
namespace EdgePulse::Auth
{
    class AuthService;
    class CaptchaService;
    class LoginAudit;
    class SessionStore;
} // namespace EdgePulse::Auth

namespace EdgePulse::TestSupport
{
    /**
     * @brief 每个测试实例独占一份 SQLite 与一套认证服务
     * @note 成员按声明逆序销毁；数据库与服务于本对象析构时清理临时文件
     */
    class AuthTestContext
    {
    public:
        AuthTestContext();
        ~AuthTestContext();

        AuthTestContext(const AuthTestContext &)            = delete;
        AuthTestContext &operator=(const AuthTestContext &) = delete;

        std::shared_ptr<Common::Database>           database; ///< 已应用迁移的数据库
        std::shared_ptr<Cache::MemoryKeyValueStore> cache;    ///< 会话与验证码存储
        std::shared_ptr<Auth::SessionStore>         sessions; ///< 会话表
        std::shared_ptr<Auth::CaptchaService>       captcha;  ///< 验证码
        std::shared_ptr<Auth::LoginAudit>           audit;    ///< 登录审计
        std::shared_ptr<Auth::AuthService>          auth;     ///< 认证服务
        std::shared_ptr<System::TenantRepository>   tenants;  ///< 租户

        /// 临时数据库文件路径
        [[nodiscard]] const std::filesystem::path &databasePath() const noexcept
        {
            return m_databasePath;
        }

    private:
        std::filesystem::path m_databasePath; ///< 临时数据库文件
    };
} // namespace EdgePulse::TestSupport
