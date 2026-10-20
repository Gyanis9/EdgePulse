#include "Auth/AuthService.h"

#include "Base/Log/LogMacros.h"
#include "Common/ErrorCatalog.h"
#include "Common/PasswordHash.h"
#include "Common/TimeFormat.h"
#include "Core/Coroutine/AsyncExecutor.h"

#include <algorithm>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Auth
{
    namespace
    {
        constexpr std::size_t kMinimumPasswordLength = 6;  ///< 新口令最短长度
        constexpr std::size_t kMaximumPasswordLength = 64; ///< 新口令最长长度
    } // namespace

    AuthService::AuthService(std::shared_ptr<Common::Database> database, std::shared_ptr<SessionStore> sessions, std::shared_ptr<CaptchaService> captcha,
                             std::shared_ptr<LoginAudit> audit) :
        m_sessions(std::move(sessions)), m_captcha(std::move(captcha)), m_loginAudit(std::move(audit)), m_users(database), m_menus(std::move(database))
    {
    }

    AsynGyanis::Core::Task<> AuthService::recordAudit(AsynGyanis::Core::EventLoop &loop, LoginAuditEntry entry)
    {
        try
        {
            if (!co_await m_loginAudit->record(loop, std::move(entry)))
            {
                LOG_ERROR("登录审计未写入：数据库返回失败");
            }
        } catch (const std::exception &failure)
        {
            // 审计失败不推翻已经成立的登录；原因只进日志，不回给客户端
            LOG_ERROR_FMT("登录审计写入异常：{}", failure.what());
        }
        co_return;
    }

    AsynGyanis::Core::Task<std::expected<LoginResult, AuthError>> AuthService::login(AsynGyanis::Core::EventLoop &loop, LoginCredentials credentials)
    {
        if (credentials.username.empty() || credentials.password.empty())
        {
            co_return std::unexpected(AuthError{Common::kCodeBadRequest, Common::kCodeBadRequest, "用户名和密码不能为空"});
        }

        // 验证码一次性消费：失败时也写审计，便于排查撞库与过期页面
        if (!m_captcha->consume(credentials.captchaUuid, credentials.captcha))
        {
            co_await recordAudit(loop, LoginAuditEntry{credentials.username, credentials.clientIp, false, "验证码错误或已过期", credentials.traceId});
            co_return std::unexpected(AuthError{Common::kCodeBadRequest, Common::kCodeBadRequest, "验证码错误或已过期"});
        }

        const std::optional<System::UserRecord> user = co_await m_users.findByUsername(loop, credentials.username);
        // 用户不存在与口令错误对客户端使用同一条文案，避免账号枚举
        if (!user.has_value() || user->status != 1)
        {
            co_await recordAudit(loop, LoginAuditEntry{credentials.username, credentials.clientIp, false, "用户不存在或已停用", credentials.traceId});
            co_return std::unexpected(AuthError{Common::kCodeUnauthorized, Common::kCodeUnauthorized, "用户名或密码错误"});
        }

        // PBKDF2 是 CPU 密集操作，放到工作线程，避免 210000 次迭代占住事件循环
        const std::string storedHash = user->passwordHash;
        const std::string password   = credentials.password;
        const bool isMatch = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<bool>(loop, [storedHash, password] { return Common::verifyPassword(storedHash, password); });
        if (!isMatch)
        {
            co_await recordAudit(loop, LoginAuditEntry{credentials.username, credentials.clientIp, false, "口令不匹配", credentials.traceId});
            co_return std::unexpected(AuthError{Common::kCodeUnauthorized, Common::kCodeUnauthorized, "用户名或密码错误"});
        }

        const std::vector<std::string> roles       = co_await m_users.rolesFor(loop, user->id);
        const std::vector<std::string> permissions = co_await m_users.permissionsFor(loop, user->id);

        SessionRecord record;
        record.userId               = user->id;
        record.username             = user->username;
        record.nickname             = user->nickname;
        record.departmentId         = user->departmentId;
        record.departmentName       = "";
        record.roles                = roles;
        record.permissions          = permissions;
        const SessionRecord session = m_sessions->create(std::move(record));

        // 登录时间更新失败不影响本次登录，但必须在日志里留下原因
        if (!co_await m_users.touchLastLogin(loop, user->id, Common::nowIso8601Utc()))
        {
            LOG_WARN_FMT("登录时间未更新：user={}", user->username);
        }
        co_await recordAudit(loop, LoginAuditEntry{user->username, credentials.clientIp, true, "登录成功", credentials.traceId});

        co_return LoginResult{session.token, m_sessions->ttlSeconds()};
    }

    std::optional<SessionRecord> AuthService::authenticate(const std::string_view token)
    {
        if (token.empty())
        {
            return std::nullopt;
        }
        return m_sessions->validateAndTouch(token);
    }

    void AuthService::logout(const std::string_view token)
    {
        m_sessions->remove(token);
    }

    AsynGyanis::Core::Task<std::vector<System::MenuNode>> AuthService::routes(AsynGyanis::Core::EventLoop &loop, const SessionRecord &session)
    {
        co_return co_await m_menus.treeForUser(loop, session.userId);
    }

    AsynGyanis::Core::Task<std::expected<void, AuthError>> AuthService::changePassword(AsynGyanis::Core::EventLoop &loop, const SessionRecord &session, std::string oldPassword,
                                                                                       std::string newPassword)
    {
        if (newPassword.size() < kMinimumPasswordLength || newPassword.size() > kMaximumPasswordLength)
        {
            co_return std::unexpected(AuthError{Common::kCodeBadRequest, Common::kCodeBadRequest, "新密码长度必须是 6 到 64 个字符"});
        }
        const std::optional<System::UserRecord> user = co_await m_users.findById(loop, session.userId);
        if (!user.has_value())
        {
            co_return std::unexpected(AuthError{Common::kCodeUnauthorized, Common::kCodeUnauthorized, "会话对应的用户已不存在"});
        }

        const std::string storedHash = user->passwordHash;
        const bool        isMatch =
                co_await AsynGyanis::Core::AsyncExecutor::shared().submit<bool>(loop, [storedHash, oldPassword] { return Common::verifyPassword(storedHash, oldPassword); });
        if (!isMatch)
        {
            co_return std::unexpected(AuthError{Common::kCodeBadRequest, Common::kCodeBadRequest, "原密码不正确"});
        }

        const std::string newHash = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::string>(loop, [newPassword] { return Common::hashPassword(newPassword); });
        if (!co_await m_users.updatePassword(loop, session.userId, newHash))
        {
            co_return std::unexpected(AuthError{Common::kCodeInternalServerError, Common::kCodeInternalServerError, "修改密码失败：用户不存在或已被删除"});
        }
        co_return std::expected<void, AuthError>{};
    }

    bool AuthService::hasPermission(const SessionRecord &session, const std::string_view permission) const
    {
        return std::find(session.permissions.begin(), session.permissions.end(), permission) != session.permissions.end();
    }

    int AuthService::sessionTtlSeconds() const noexcept
    {
        return m_sessions->ttlSeconds();
    }
} // namespace EdgePulse::Auth
