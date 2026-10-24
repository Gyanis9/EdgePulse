#include "Core/Auth/AuthService.h"

#include "Base/Log/LogMacros.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Security/PasswordHash.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Auth/ApiTokenService.h"
#include "Core/Auth/MfaService.h"
#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/System/TenantRepository.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Auth
{
    namespace
    {
        constexpr int                  kMaximumLoginFailures = 10;                       ///< 单 IP 每分钟失败上限
        constexpr std::chrono::seconds kLoginFailureWindow{60};                          ///< 失败计数窗口
        constexpr std::string_view     kLoginFailurePrefix = "edgepulse:login:fail:";    ///< 失败计数键前缀
        constexpr std::string_view     kTenantStatusPrefix = "edgepulse:tenant:status:"; ///< 租户启停缓存前缀
        constexpr std::chrono::seconds kTenantStatusTtl{5};                              ///< 跨实例缓存的最长滞后

        [[nodiscard]] std::string tenantStatusKey(const std::int64_t tenantId)
        {
            return std::string{kTenantStatusPrefix} + std::to_string(tenantId);
        }

        [[nodiscard]] std::string ipFailureKey(const std::string &clientIp)
        {
            return std::string{kLoginFailurePrefix} + clientIp;
        }
    } // namespace

    AuthService::AuthService(std::shared_ptr<Common::Database> database, std::shared_ptr<SessionStore> sessions, std::shared_ptr<CaptchaService> captcha,
                             std::shared_ptr<LoginAudit> audit, std::shared_ptr<Cache::KeyValueStore> cache, std::shared_ptr<System::TenantRepository> tenants,
                             AuthOptions options) :
        m_sessions(std::move(sessions)), m_captcha(std::move(captcha)), m_loginAudit(std::move(audit)), m_cache(std::move(cache)), m_tenants(std::move(tenants)), m_users(database),
        m_menus(std::move(database)), m_options(options), m_passwordPolicy(options.passwordMinimumLength, options.passwordRequireComplexity)
    {
    }

    void AuthService::setApiTokenService(std::shared_ptr<ApiTokenService> apiTokens)
    {
        m_apiTokens = std::move(apiTokens);
    }

    void AuthService::setMfaService(std::shared_ptr<MfaService> mfa)
    {
        m_mfa = std::move(mfa);
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

        const std::string                         tenantCode = credentials.tenantCode.empty() ? std::string{"default"} : credentials.tenantCode;
        const std::optional<System::TenantRecord> tenant     = co_await m_tenants->findByCode(loop, tenantCode);
        if (!tenant.has_value() || tenant->status != 1)
        {
            co_await recordAudit(loop, LoginAuditEntry{credentials.username, credentials.clientIp, false, "租户不存在或已停用", credentials.traceId, 0, 0, 0});
            co_return std::unexpected(AuthError{Common::kCodeUnauthorized, Common::kCodeUnauthorized, "用户名或密码错误"});
        }
        const std::int64_t tenantId = tenant->id;

        // 失败计数放在最前：超过阈值时连验证码与口令校验都不做，避免把 PBKDF2 变成撞库加速器
        const std::string  failureKey = ipFailureKey(credentials.clientIp);
        const std::int64_t failures   = co_await m_cache->increment(loop, failureKey, kLoginFailureWindow);
        if (failures > kMaximumLoginFailures)
        {
            co_await recordAudit(loop, LoginAuditEntry{credentials.username, credentials.clientIp, false, "登录尝试过于频繁", credentials.traceId, 0, 0, tenantId});
            co_return std::unexpected(AuthError{Common::kCodeTooManyRequests, Common::kCodeTooManyRequests, "登录尝试过于频繁：请一分钟后再试"});
        }

        // 验证码一次性消费：失败时也写审计，便于排查撞库与过期页面
        if (!co_await m_captcha->consume(loop, credentials.captchaUuid, credentials.captcha))
        {
            co_await recordAudit(loop, LoginAuditEntry{credentials.username, credentials.clientIp, false, "验证码错误或已过期", credentials.traceId, 0, 0, tenantId});
            co_return std::unexpected(AuthError{Common::kCodeBadRequest, Common::kCodeBadRequest, "验证码错误或已过期"});
        }

        const std::optional<System::UserRecord> user = co_await m_users.scoped({tenantId, false})->findByUsername(loop, credentials.username);
        // 用户不存在与口令错误对客户端使用同一条文案，避免账号枚举
        if (!user.has_value() || user->status != 1)
        {
            co_await recordAudit(loop, LoginAuditEntry{credentials.username, credentials.clientIp, false, "用户不存在或已停用", credentials.traceId, 0, 0, tenantId});
            co_return std::unexpected(AuthError{Common::kCodeUnauthorized, Common::kCodeUnauthorized, "用户名或密码错误"});
        }

        // 锁定先于口令校验：锁定期内连 PBKDF2 都不跑，避免把锁定账号变成计算资源消耗点
        const std::string now = Common::nowDateTimeUtc();
        if (!user->lockedUntil.empty() && user->lockedUntil > now)
        {
            co_await recordAudit(loop,
                                 LoginAuditEntry{credentials.username, credentials.clientIp, false, "账号已锁定", credentials.traceId, user->id, user->departmentId, tenantId});
            // 分钟数是配置的锁定时长，不是剩余时间；不足一分钟也按一分钟提示
            co_return std::unexpected(AuthError{423, 423, "账号已锁定，请稍后再试（锁定时长 " + std::to_string((m_options.loginLockSeconds + 59) / 60) + " 分钟）"});
        }
        // 锁定已过期但失败窗口更短时，计数可能仍停在阈值上；先清零，避免下一次输错立刻把账号重新锁上
        if (!user->lockedUntil.empty())
        {
            // 数据库按匹配行数（found-rows）计影响行：即使三个字段本来已是 0/NULL 也照样命中；
            // 返回 false 只说明用户行已不匹配，例如期间被并发软删除
            if (!co_await m_users.scoped({tenantId, false})->clearLoginFailures(loop, user->id))
            {
                LOG_WARN_FMT("清零过期锁定未命中用户：user={} id={}", user->username, user->id);
            }
        }

        // PBKDF2 是 CPU 密集操作，放到工作线程，避免 210000 次迭代占住事件循环
        const std::string storedHash = user->passwordHash;
        const std::string password   = credentials.password;
        const bool isMatch = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<bool>(loop, [storedHash, password] { return Common::verifyPassword(storedHash, password); });
        if (!isMatch)
        {
            // 锁定的那次失败仍回统一 401：攻击者不能借状态码差异判断账号存在或已触发锁定
            const auto lockedUntil = co_await m_users.scoped({tenantId, false})
                                             ->recordLoginFailure(loop, user->id, m_options.loginFailureWindowSeconds, m_options.loginMaximumFailures, m_options.loginLockSeconds);
            co_await recordAudit(loop, LoginAuditEntry{credentials.username, credentials.clientIp, false, lockedUntil.has_value() ? "连续失败已锁定" : "口令不匹配",
                                                       credentials.traceId, user->id, user->departmentId, tenantId});
            co_return std::unexpected(AuthError{Common::kCodeUnauthorized, Common::kCodeUnauthorized, "用户名或密码错误"});
        }

        // 口令成立后先看第二因素：启用 MFA 的账号在完成挑战前不签发任何会话
        if (m_mfa != nullptr && co_await m_mfa->enabled(loop, user->id))
        {
            const std::optional<std::string> challenge = co_await m_mfa->createChallenge(loop, user->id, user->tenantId, credentials.clientIp);
            if (!challenge.has_value())
            {
                co_await recordAudit(loop, LoginAuditEntry{credentials.username, credentials.clientIp, false, "两步验证挑战签发失败", credentials.traceId, user->id,
                                                           user->departmentId, tenantId});
                co_return std::unexpected(AuthError{Common::kCodeServiceUnavailable, Common::kCodeServiceUnavailable, "两步验证服务暂不可用，请稍后重试"});
            }
            co_await recordAudit(
                    loop, LoginAuditEntry{credentials.username, credentials.clientIp, true, "登录成功，等待两步验证", credentials.traceId, user->id, user->departmentId, tenantId});
            co_return LoginResult{.mfaRequired = true, .challengeToken = *challenge, .challengeExpiresIn = MfaService::kChallengeTtlSeconds};
        }

        auto issued = co_await createSessionForUser(loop, *user, credentials.clientIp, credentials.traceId, "登录成功");
        if (issued.has_value())
        {
            // 成功登录同时清空按账号与按 IP 的失败计数，避免此前几次输错影响后续正常使用
            if (!co_await m_users.scoped({user->tenantId, false})->clearLoginFailures(loop, user->id))
            {
                // 命中失败只可能是用户行已不匹配（例如并发软删除）；本次登录仍然成立，但要在日志里留痕
                LOG_WARN_FMT("登录后清零失败计数未命中用户：user={} id={}", user->username, user->id);
            }
            static_cast<void>(co_await m_cache->erase(loop, failureKey));
        }
        co_return issued;
    }

    AsynGyanis::Core::Task<std::expected<LoginResult, AuthError>> AuthService::verifyMfa(AsynGyanis::Core::EventLoop &loop, std::string challengeToken, std::string code,
                                                                                         std::string clientIp, std::string traceId)
    {
        // 挑战失效、来源不符与验证码错误对客户端用同一条文案，避免帮助攻击者区分失败阶段
        const auto invalidChallenge = []() -> AuthError { return AuthError{Common::kCodeUnauthorized, Common::kCodeUnauthorized, "两步验证失败：验证码错误或已过期，请重新登录"}; };
        if (m_mfa == nullptr || challengeToken.empty())
        {
            co_return std::unexpected(invalidChallenge());
        }
        // 取值即删除：同一挑战只允许一次校验，失败次数由写回的挑战承担
        std::optional<MfaChallenge> challenge = co_await m_mfa->takeChallenge(loop, challengeToken);
        if (!challenge.has_value() || challenge->attempts >= MfaService::kChallengeMaxAttempts)
        {
            co_return std::unexpected(invalidChallenge());
        }
        if (challenge->clientIp != clientIp)
        {
            co_await recordAudit(loop, LoginAuditEntry{std::string{}, clientIp, false, "两步验证来源与登录请求不一致", traceId, challenge->userId, 0, challenge->tenantId});
            co_return std::unexpected(invalidChallenge());
        }

        const std::optional<System::UserRecord> user = co_await m_users.scoped({challenge->tenantId, false})->findById(loop, challenge->userId);
        if (!user.has_value() || user->status != 1)
        {
            co_return std::unexpected(invalidChallenge());
        }

        // TOTP 与恢复码共用同一失败计数：TOTP 未命中后再尝试恢复码，两者都失败才记一次
        bool verified = co_await m_mfa->verify(loop, user->id, code);
        if (!verified)
        {
            verified = co_await m_mfa->verifyRecovery(loop, user->id, code);
        }
        if (!verified)
        {
            const ChallengeFailureOutcome outcome = co_await m_mfa->recordChallengeFailure(loop, *challenge);
            std::string                   message = "两步验证失败";
            switch (outcome)
            {
                case ChallengeFailureOutcome::Retained:
                    break;
                case ChallengeFailureOutcome::LimitReached:
                    message = "两步验证失败次数已达上限";
                    break;
                case ChallengeFailureOutcome::Expired:
                    message = "两步验证挑战已过期";
                    break;
                case ChallengeFailureOutcome::WriteFailed:
                    // 挑战已在上一次 take 时删除；写回失败与次数上限是两种原因，审计分开留痕
                    message = "两步验证失败且挑战续期写入失败";
                    LOG_ERROR_FMT("两步验证挑战续期写入失败：user={} id={}", user->username, user->id);
                    break;
            }
            co_await recordAudit(loop, LoginAuditEntry{user->username, clientIp, false, message, traceId, user->id, user->departmentId, user->tenantId});
            co_return std::unexpected(invalidChallenge());
        }

        auto issued = co_await createSessionForUser(loop, *user, clientIp, traceId, "登录成功（两步验证）");
        if (issued.has_value())
        {
            if (!co_await m_users.scoped({user->tenantId, false})->clearLoginFailures(loop, user->id))
            {
                LOG_WARN_FMT("两步验证登录后清零失败计数未命中用户：user={} id={}", user->username, user->id);
            }
            static_cast<void>(co_await m_cache->erase(loop, ipFailureKey(clientIp)));
        }
        co_return issued;
    }

    AsynGyanis::Core::Task<bool> AuthService::verifyPassword(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId, std::string password)
    {
        const std::optional<System::UserRecord> user = co_await m_users.scoped({1, true})->findById(loop, userId);
        if (!user.has_value() || user->status != 1)
        {
            co_return false;
        }
        const std::string storedHash = user->passwordHash;
        co_return co_await AsynGyanis::Core::AsyncExecutor::shared().submit<bool>(loop, [storedHash, password = std::move(password)]
                                                                                  { return Common::verifyPassword(storedHash, password); });
    }

    AsynGyanis::Core::Task<std::expected<LoginResult, AuthError>> AuthService::issueSessionForUser(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId,
                                                                                                   std::string clientIp, std::string traceId, std::string auditMessage)
    {
        const std::optional<System::UserRecord> user = co_await m_users.scoped({1, true})->findById(loop, userId);
        if (!user.has_value() || user->status != 1)
        {
            co_return std::unexpected(AuthError{Common::kCodeUnauthorized, Common::kCodeUnauthorized, "账号不存在或已停用"});
        }
        co_return co_await createSessionForUser(loop, *user, clientIp, traceId, auditMessage);
    }

    AsynGyanis::Core::Task<std::expected<LoginResult, AuthError>> AuthService::issueSessionOrChallengeForUser(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId,
                                                                                                              std::string clientIp, std::string traceId, std::string auditMessage)
    {
        const std::optional<System::UserRecord> user = co_await m_users.scoped({1, true})->findById(loop, userId);
        if (!user.has_value() || user->status != 1)
        {
            co_return std::unexpected(AuthError{Common::kCodeUnauthorized, Common::kCodeUnauthorized, "账号不存在或已停用"});
        }
        if (m_mfa != nullptr && co_await m_mfa->enabled(loop, user->id))
        {
            const std::optional<std::string> challenge = co_await m_mfa->createChallenge(loop, user->id, user->tenantId, clientIp);
            if (!challenge.has_value())
            {
                co_await recordAudit(loop, LoginAuditEntry{user->username, clientIp, false, "两步验证挑战签发失败", traceId, user->id, user->departmentId, user->tenantId});
                co_return std::unexpected(AuthError{Common::kCodeServiceUnavailable, Common::kCodeServiceUnavailable, "两步验证服务暂不可用，请稍后重试"});
            }
            co_await recordAudit(loop, LoginAuditEntry{user->username, clientIp, true, "登录成功，等待两步验证", traceId, user->id, user->departmentId, user->tenantId});
            co_return LoginResult{.mfaRequired = true, .challengeToken = *challenge, .challengeExpiresIn = MfaService::kChallengeTtlSeconds};
        }
        co_return co_await createSessionForUser(loop, *user, clientIp, traceId, auditMessage);
    }

    AsynGyanis::Core::Task<std::expected<LoginResult, AuthError>> AuthService::createSessionForUser(AsynGyanis::Core::EventLoop &loop, const System::UserRecord &user,
                                                                                                    const std::string &clientIp, const std::string &traceId,
                                                                                                    const std::string &auditMessage)
    {
        const std::vector<std::string> roles       = co_await m_users.rolesFor(loop, user.id);
        const std::vector<std::string> permissions = co_await m_users.permissionsFor(loop, user.id);

        SessionRecord record;
        record.userId               = user.id;
        record.tenantId             = user.tenantId;
        record.tenantCode           = user.tenantCode;
        record.tenantName           = user.tenantName;
        record.platformAdmin        = user.platformAdmin;
        record.username             = user.username;
        record.nickname             = user.nickname;
        record.departmentId         = user.departmentId;
        record.departmentName       = user.departmentName;
        record.roles                = roles;
        record.permissions          = permissions;
        const SessionRecord session = co_await m_sessions->create(loop, std::move(record));

        // 登录时间更新失败不影响本次登录，但必须在日志里留下原因
        if (!co_await m_users.scoped({user.tenantId, false})->touchLastLogin(loop, user.id, Common::nowDateTimeUtc()))
        {
            LOG_WARN_FMT("登录时间未更新：user={}", user.username);
        }
        co_await recordAudit(loop, LoginAuditEntry{user.username, clientIp, true, auditMessage, traceId, user.id, user.departmentId, user.tenantId});
        co_return LoginResult{session.token, m_sessions->ttlSeconds()};
    }

    AsynGyanis::Core::Task<std::optional<SessionRecord>> AuthService::authenticate(AsynGyanis::Core::EventLoop &loop, const std::string_view token)
    {
        if (token.empty())
        {
            co_return std::nullopt;
        }
        std::optional<SessionRecord> session = co_await m_sessions->validateAndTouch(loop, token);
        if (!session.has_value() && m_apiTokens)
        {
            session = co_await m_apiTokens->authenticate(loop, token);
        }
        if (!session.has_value())
        {
            co_return std::nullopt;
        }
        if (session->platformAdmin)
        {
            co_return session;
        }
        if (!co_await tenantEnabled(loop, session->tenantId))
        {
            static_cast<void>(co_await m_sessions->remove(loop, token));
            co_return std::nullopt;
        }
        co_return session;
    }

    AsynGyanis::Core::Task<bool> AuthService::tenantEnabled(AsynGyanis::Core::EventLoop &loop, const std::int64_t tenantId)
    {
        // 停用租户后不能等会话自然过期：用短 TTL 缓存租户状态，更新租户时主动失效
        const std::string                key     = tenantStatusKey(tenantId);
        const std::optional<std::string> cached  = co_await m_cache->get(loop, key);
        bool                             enabled = cached.has_value() && *cached == "1";
        if (!cached.has_value() || (*cached != "1" && *cached != "0"))
        {
            const auto tenant = co_await m_tenants->findById(loop, tenantId);
            enabled           = tenant.has_value() && tenant->status == 1;
            static_cast<void>(co_await m_cache->set(loop, key, enabled ? "1" : "0", kTenantStatusTtl));
        }
        co_return enabled;
    }

    AsynGyanis::Core::Task<void> AuthService::invalidateTenant(AsynGyanis::Core::EventLoop &loop, const std::int64_t tenantId)
    {
        static_cast<void>(co_await m_cache->erase(loop, tenantStatusKey(tenantId)));
        co_return;
    }

    AsynGyanis::Core::Task<bool> AuthService::logout(AsynGyanis::Core::EventLoop &loop, const std::string_view token)
    {
        co_return co_await m_sessions->remove(loop, token);
    }

    AsynGyanis::Core::Task<std::vector<System::MenuNode>> AuthService::routes(AsynGyanis::Core::EventLoop &loop, const SessionRecord &session)
    {
        co_return co_await m_menus.treeForUser(loop, session.userId);
    }

    std::expected<void, std::string> AuthService::validatePassword(const std::string_view password) const
    {
        return m_passwordPolicy.validate(password);
    }

    AsynGyanis::Core::Task<std::expected<void, AuthError>> AuthService::changePassword(AsynGyanis::Core::EventLoop &loop, const SessionRecord &session, std::string oldPassword,
                                                                                       std::string newPassword)
    {
        // 改密只接受浏览器登录会话；令牌泄露不应连带接管账户口令
        if (session.apiToken)
        {
            co_return std::unexpected(AuthError{Common::kCodeForbidden, Common::kCodeForbidden, "API 令牌不能修改账户密码"});
        }
        if (const auto validation = validatePassword(newPassword); !validation)
        {
            co_return std::unexpected(AuthError{Common::kCodeBadRequest, Common::kCodeBadRequest, validation.error()});
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
