#include "Core/Auth/MfaService.h"

#include "Base/Coding/Base64.h"
#include "Base/Coding/SecureCompare.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Json/JsonFields.h"
#include "Base/Runtime/TimeFormat.h"
#include "Base/Security/Hmac.h"
#include "Base/Security/RandomBytes.h"
#include "Base/Security/RandomToken.h"
#include "Core/Auth/MfaCrypto.h"
#include "Core/Auth/Totp.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace EdgePulse::Auth
{
    namespace
    {
        constexpr std::string_view     kSetupCachePrefix = "edgepulse:mfa:setup:";
        constexpr std::chrono::seconds kSetupTtl{600};
        constexpr std::string_view     kAttemptCachePrefix          = "edgepulse:mfa:attempt:";
        constexpr std::string_view     kChallengeCachePrefix        = "edgepulse:mfa:challenge:";
        constexpr std::size_t          kChallengeTokenBytes         = 24;
        constexpr std::size_t          kMaximumChallengeTokenLength = 128;
        constexpr std::size_t          kRecoveryCodeCount           = 10;
        constexpr std::size_t          kRecoveryCodeGroupSize       = 4;
        constexpr std::string_view     kRecoveryAlphabet            = "123456789abcdefghjkmnpqrstuvwxyz";
        constexpr std::string_view     kIssuer                      = "EdgePulse";

        /**
         * @brief 去掉首尾空白
         * @param text 待裁剪文本
         * @return std::string 裁剪结果
         */
        [[nodiscard]] std::string trimCopy(const std::string_view text)
        {
            const auto begin = std::find_if_not(text.begin(), text.end(), [](const unsigned char character) { return std::isspace(character) != 0; });
            const auto end   = std::find_if_not(text.rbegin(), text.rend(), [](const unsigned char character) { return std::isspace(character) != 0; }).base();
            if (begin >= end)
            {
                return {};
            }
            return std::string(begin, end);
        }

        /**
         * @brief 把恢复码归一到小写并恢复固定分组
         * @param text 用户输入，可带空格或省略连字符
         * @return std::string 形如 xxxx-xxxx 的文本；字符数不足 8 时返回去掉分隔符的原样小写文本
         */
        [[nodiscard]] std::string canonicalRecoveryCode(const std::string_view text)
        {
            std::string normalized;
            for (const unsigned char character: text)
            {
                if (std::isalnum(character) != 0)
                {
                    normalized.push_back(static_cast<char>(std::tolower(character)));
                }
            }
            if (normalized.size() == 2 * kRecoveryCodeGroupSize)
            {
                normalized.insert(kRecoveryCodeGroupSize, 1, '-');
            }
            return normalized;
        }

        /**
         * @brief 生成一枚 xxxx-xxxx 恢复码
         * @return std::string 8 个随机字符加一个连字符
         * @throws Base::Exception 随机源不可用
         */
        [[nodiscard]] std::string generateRecoveryCode()
        {
            const std::vector<std::uint8_t> bytes = Common::randomSecretBytes(2 * kRecoveryCodeGroupSize);
            std::string                     code;
            code.reserve(2 * kRecoveryCodeGroupSize + 1);
            for (std::size_t index = 0; index < bytes.size(); ++index)
            {
                if (index == kRecoveryCodeGroupSize)
                {
                    code.push_back('-');
                }
                // 字母表长度是 32，256 能被整除，取模不会引入偏差
                code.push_back(kRecoveryAlphabet[bytes[index] % kRecoveryAlphabet.size()]);
            }
            return code;
        }

        /**
         * @brief 生成一组互不重复的恢复码
         * @return std::vector<std::string> 10 枚明文恢复码
         * @throws Base::Exception 随机源不可用
         */
        [[nodiscard]] std::vector<std::string> generateRecoveryCodes()
        {
            std::vector<std::string>        codes;
            std::unordered_set<std::string> seen;
            codes.reserve(kRecoveryCodeCount);
            while (codes.size() < kRecoveryCodeCount)
            {
                std::string code = generateRecoveryCode();
                if (seen.insert(code).second)
                {
                    codes.push_back(std::move(code));
                }
            }
            return codes;
        }

        /**
         * @brief 把恢复码哈希序列化成 JSON 数组
         * @param hashes 恢复码哈希的十六进制
         * @return std::string JSON 文本
         */
        [[nodiscard]] std::string serializeHashes(const std::vector<std::string> &hashes)
        {
            AsynGyanis::Base::ConfigValue array = AsynGyanis::Base::ConfigValue::array();
            for (const std::string &hash: hashes)
            {
                array.push_back(hash);
            }
            return array.dump();
        }

        /**
         * @brief 恢复码落库哈希：以主密钥为键的 HMAC-SHA256
         * @param key 32 字节主密钥
         * @param canonical 归一后的 xxxx-xxxx 文本
         * @return std::string 64 个十六进制字符
         */
        [[nodiscard]] std::string recoveryHash(const std::string_view key, const std::string_view canonical)
        {
            return Common::hmacSha256Hex(key, canonical);
        }

        /**
         * @brief 待启用秘密的缓存键
         * @param userId 用户标识
         * @return std::string 键
         */
        [[nodiscard]] std::string setupCacheKey(const std::int64_t userId)
        {
            return std::string{kSetupCachePrefix} + std::to_string(userId);
        }

        /**
         * @brief 登录挑战的缓存键
         * @param token 一次性令牌
         * @return std::string 键
         */
        [[nodiscard]] std::string challengeCacheKey(const std::string_view token)
        {
            return std::string{kChallengeCachePrefix} + std::string{token};
        }

        /**
         * @brief 按用户的会话态失败计数键
         * @param userId 用户标识
         * @return std::string 键
         */
        [[nodiscard]] std::string attemptCacheKey(const std::int64_t userId)
        {
            return std::string{kAttemptCachePrefix} + std::to_string(userId);
        }

        /**
         * @brief 当前 Unix 秒，用于挑战的绝对过期时刻
         * @return std::int64_t 秒数
         */
        [[nodiscard]] std::int64_t nowEpochSeconds()
        {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        /**
         * @brief 把挑战序列化成缓存值
         * @param challenge 挑战内容
         * @return std::string JSON 对象文本
         */
        [[nodiscard]] std::string serializeChallenge(const MfaChallenge &challenge)
        {
            return AsynGyanis::Base::ConfigValue::object({{"userId", challenge.userId},
                                                          {"tenantId", challenge.tenantId},
                                                          {"clientIp", challenge.clientIp},
                                                          {"attempts", challenge.attempts},
                                                          {"expiresAt", challenge.expiresAt}})
                    .dump();
        }

        /**
         * @brief 解析缓存里的挑战
         * @param token 令牌
         * @param text 缓存值
         * @return std::optional<MfaChallenge> 字段齐全、用户与租户为正且未过绝对期限时返回
         */
        [[nodiscard]] std::optional<MfaChallenge> parseChallenge(const std::string_view token, const std::string_view text)
        {
            const std::optional<AsynGyanis::Base::ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(text);
            if (!parsed.has_value() || !parsed->is_object())
            {
                return std::nullopt;
            }
            const std::optional<std::int64_t> userId   = Json::optionalInteger(*parsed, "userId");
            const std::optional<std::int64_t> tenantId = Json::optionalInteger(*parsed, "tenantId");
            if (!userId.has_value() || !tenantId.has_value() || *userId <= 0 || *tenantId <= 0)
            {
                return std::nullopt;
            }
            const std::optional<std::int64_t> expiresAt = Json::optionalInteger(*parsed, "expiresAt");
            if (!expiresAt.has_value() || *expiresAt <= nowEpochSeconds())
            {
                // 旧格式没有绝对过期时刻，升级后按失效处理，避免滑动续命
                return std::nullopt;
            }
            MfaChallenge challenge;
            challenge.token                            = std::string{token};
            challenge.userId                           = *userId;
            challenge.tenantId                         = *tenantId;
            challenge.clientIp                         = Json::stringField(*parsed, "clientIp");
            const std::optional<std::int64_t> attempts = Json::optionalInteger(*parsed, "attempts");
            challenge.attempts                         = attempts.has_value() ? std::max(0, static_cast<int>(*attempts)) : 0;
            challenge.expiresAt                        = *expiresAt;
            return challenge;
        }

        /**
         * @brief 把待启用秘密的密文序列化成缓存值
         * @param ciphertext 加密结果
         * @return std::string 含 nonce 与 ciphertext 的 JSON 对象文本
         */
        [[nodiscard]] std::string serializeSetupCiphertext(const MfaCiphertext &ciphertext)
        {
            return AsynGyanis::Base::ConfigValue::object({{"nonce", ciphertext.nonce}, {"ciphertext", ciphertext.ciphertext}}).dump();
        }

        /**
         * @brief 解析缓存里的待启用密文
         * @param text 缓存值
         * @return std::optional<MfaCiphertext> 两个字段都非空时返回
         */
        [[nodiscard]] std::optional<MfaCiphertext> parseSetupCiphertext(const std::string_view text)
        {
            const std::optional<AsynGyanis::Base::ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(text);
            if (!parsed.has_value() || !parsed->is_object())
            {
                return std::nullopt;
            }
            MfaCiphertext ciphertext;
            ciphertext.nonce      = parsed->value("nonce", std::string{});
            ciphertext.ciphertext = parsed->value("ciphertext", std::string{});
            if (ciphertext.nonce.empty() || ciphertext.ciphertext.empty())
            {
                return std::nullopt;
            }
            return ciphertext;
        }

        /**
         * @brief 解码配置里的 base64 主密钥
         * @param text base64 文本
         * @return std::string 32 字节原始密钥；空串或格式不符时为空
         */
        [[nodiscard]] std::string decodeKey(const std::string_view text)
        {
            if (text.empty())
            {
                return {};
            }
            const std::optional<std::string> decoded = AsynGyanis::Base::base64Decode(text);
            if (!decoded.has_value() || decoded->size() != kMfaKeyBytes)
            {
                return {};
            }
            return *decoded;
        }

        [[nodiscard]] MfaError serviceUnavailable(std::string message)
        {
            return MfaError{503, std::move(message)};
        }

        [[nodiscard]] MfaError badRequest(std::string message)
        {
            return MfaError{400, std::move(message)};
        }

        [[nodiscard]] MfaError unauthorized(std::string message)
        {
            return MfaError{401, std::move(message)};
        }

        [[nodiscard]] MfaError notFound(std::string message)
        {
            return MfaError{404, std::move(message)};
        }

        [[nodiscard]] MfaError conflict(std::string message)
        {
            return MfaError{409, std::move(message)};
        }

        [[nodiscard]] MfaError tooManyRequests(std::string message)
        {
            return MfaError{429, std::move(message)};
        }
    } // namespace

    MfaService::MfaService(std::shared_ptr<Common::Database> database, std::shared_ptr<Cache::KeyValueStore> cache, std::string encryptionKey) :
        m_cache(std::move(cache)), m_repository(std::move(database)), m_key(decodeKey(encryptionKey))
    {
    }

    AsynGyanis::Core::Task<std::optional<std::string>> MfaService::createChallenge(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId, const std::int64_t tenantId,
                                                                                   std::string clientIp)
    {
        if (userId <= 0 || tenantId <= 0)
        {
            co_return std::nullopt;
        }
        // 挑战是第二因素的入口，失败计数到上限后不再签发，避免换一枚令牌重新拿 5 次机会
        if (co_await attemptLimitReached(loop, userId))
        {
            co_return std::nullopt;
        }
        try
        {
            MfaChallenge challenge;
            challenge.token     = Common::randomTokenHex(kChallengeTokenBytes);
            challenge.userId    = userId;
            challenge.tenantId  = tenantId;
            challenge.clientIp  = std::move(clientIp);
            challenge.expiresAt = nowEpochSeconds() + kChallengeTtlSeconds;
            if (!co_await m_cache->set(loop, challengeCacheKey(challenge.token), serializeChallenge(challenge), std::chrono::seconds{kChallengeTtlSeconds}))
            {
                co_return std::nullopt;
            }
            co_return challenge.token;
        } catch (const std::exception &)
        {
            // 随机源不可用时不签发挑战，由调用方转成可恢复的服务不可用
            co_return std::nullopt;
        }
    }

    AsynGyanis::Core::Task<std::optional<MfaChallenge>> MfaService::takeChallenge(AsynGyanis::Core::EventLoop &loop, const std::string_view token)
    {
        if (token.empty() || token.size() > kMaximumChallengeTokenLength)
        {
            co_return std::nullopt;
        }
        const std::optional<std::string> stored = co_await m_cache->take(loop, challengeCacheKey(token));
        if (!stored.has_value())
        {
            co_return std::nullopt;
        }
        co_return parseChallenge(token, *stored);
    }

    AsynGyanis::Core::Task<ChallengeFailureOutcome> MfaService::recordChallengeFailure(AsynGyanis::Core::EventLoop &loop, MfaChallenge challenge)
    {
        if (challenge.token.empty())
        {
            co_return ChallengeFailureOutcome::WriteFailed;
        }
        const std::int64_t remaining = challenge.expiresAt - nowEpochSeconds();
        if (remaining <= 0)
        {
            co_return ChallengeFailureOutcome::Expired;
        }
        ++challenge.attempts;
        if (challenge.attempts >= kChallengeMaxAttempts)
        {
            // 第 5 次失败不再写回，令牌在上一次 take 时已经删除
            co_return ChallengeFailureOutcome::LimitReached;
        }
        const bool retained = co_await m_cache->set(loop, challengeCacheKey(challenge.token), serializeChallenge(challenge), std::chrono::seconds{remaining});
        co_return retained ? ChallengeFailureOutcome::Retained : ChallengeFailureOutcome::WriteFailed;
    }

    bool MfaService::keyConfigured() const noexcept
    {
        return m_key.size() == kMfaKeyBytes;
    }

    AsynGyanis::Core::Task<bool> MfaService::attemptLimitReached(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        const std::optional<std::string> stored = co_await m_cache->get(loop, attemptCacheKey(userId));
        if (!stored.has_value())
        {
            co_return false;
        }
        std::int64_t failures = 0;
        const auto   result   = std::from_chars(stored->data(), stored->data() + stored->size(), failures);
        co_return result.ec == std::errc{} && failures >= kAttemptMaxFailures;
    }

    AsynGyanis::Core::Task<> MfaService::recordAttemptFailure(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        static_cast<void>(co_await m_cache->increment(loop, attemptCacheKey(userId), std::chrono::seconds{kAttemptTtlSeconds}));
        co_return;
    }

    AsynGyanis::Core::Task<> MfaService::clearAttemptFailures(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        static_cast<void>(co_await m_cache->erase(loop, attemptCacheKey(userId)));
        co_return;
    }

    std::optional<std::string> MfaService::decryptSecret(const MfaRecord &record) const
    {
        if (!keyConfigured())
        {
            return std::nullopt;
        }
        return MfaCrypto::decrypt(record.secretCiphertext, record.secretNonce, m_key);
    }

    std::optional<std::int64_t> MfaService::matchingStepForSecret(const std::string_view secret, const std::string_view code, const std::int64_t nowStep,
                                                                  const std::int64_t lastUsedStep) const
    {
        if (code.size() != static_cast<std::size_t>(Totp::kDefaultDigits))
        {
            return std::nullopt;
        }
        for (int offset = -Totp::kDefaultWindow; offset <= Totp::kDefaultWindow; ++offset)
        {
            const std::int64_t candidate = nowStep + offset;
            if (candidate <= lastUsedStep)
            {
                continue;
            }
            const std::string expected = Totp::code(secret, candidate);
            if (!expected.empty() && AsynGyanis::Base::constantTimeEquals(expected, code))
            {
                return candidate;
            }
        }
        return std::nullopt;
    }

    std::optional<std::int64_t> MfaService::matchingStep(const MfaRecord &record, const std::string_view code, const std::int64_t nowStep, const std::int64_t lastUsedStep) const
    {
        const std::optional<std::string> secret = decryptSecret(record);
        if (!secret.has_value())
        {
            return std::nullopt;
        }
        return matchingStepForSecret(*secret, code, nowStep, lastUsedStep);
    }

    AsynGyanis::Core::Task<std::expected<MfaSetup, MfaError>> MfaService::beginSetup(AsynGyanis::Core::EventLoop &loop, SessionRecord session)
    {
        if (!keyConfigured())
        {
            co_return std::unexpected(serviceUnavailable("未配置两步验证加密密钥，请联系管理员配置 EDGEPULSE_MFA_KEY 后重试"));
        }
        if (co_await attemptLimitReached(loop, session.userId))
        {
            co_return std::unexpected(tooManyRequests("两步验证尝试过于频繁：请 5 分钟后再试"));
        }
        const std::shared_ptr<MfaRepository> repository = m_repository.scoped({session.tenantId, false});
        const std::optional<MfaRecord>       existing   = co_await repository->findByUser(loop, session.userId);
        if (existing.has_value() && existing->enabled)
        {
            co_return std::unexpected(conflict("两步验证已启用，如需更换密钥请先关闭两步验证"));
        }

        MfaSetup setup;
        setup.secret                                 = Totp::generateSecret();
        setup.otpauthUri                             = Totp::otpauthUri(kIssuer, session.username, setup.secret);
        const std::optional<MfaCiphertext> encrypted = MfaCrypto::encrypt(setup.secret, m_key);
        if (!encrypted.has_value())
        {
            co_return std::unexpected(serviceUnavailable("两步验证密钥加密失败，请稍后重试"));
        }

        // 先写一条停用行：启用时只需要一次条件 UPDATE，不再依赖“先 upsert 再 enable”的两次写
        const std::string now = Common::nowDateTimeUtc();
        MfaRecord         record;
        record.userId           = session.userId;
        record.tenantId         = session.tenantId;
        record.secretCiphertext = encrypted->ciphertext;
        record.secretNonce      = encrypted->nonce;
        record.keyVersion       = 1;
        record.createdAt        = now;
        record.updatedAt        = now;
        if (!co_await repository->upsertSecret(loop, std::move(record)))
        {
            co_return std::unexpected(conflict("两步验证状态已被并发修改，请刷新后重试"));
        }

        const bool cached = co_await m_cache->set(loop, setupCacheKey(session.userId), serializeSetupCiphertext(*encrypted), kSetupTtl);
        if (!cached)
        {
            co_return std::unexpected(serviceUnavailable("两步验证设置缓存不可用，请稍后重试"));
        }
        co_return setup;
    }

    AsynGyanis::Core::Task<std::expected<std::vector<std::string>, MfaError>> MfaService::enable(AsynGyanis::Core::EventLoop &loop, SessionRecord session, std::string code)
    {
        if (!keyConfigured())
        {
            co_return std::unexpected(serviceUnavailable("未配置两步验证加密密钥，请联系管理员配置 EDGEPULSE_MFA_KEY 后重试"));
        }
        const std::shared_ptr<MfaRepository> repository = m_repository.scoped({session.tenantId, false});
        const std::optional<MfaRecord>       existing   = co_await repository->findByUser(loop, session.userId);
        if (existing.has_value() && existing->enabled)
        {
            co_return std::unexpected(conflict("两步验证已启用，如需更换密钥请先关闭两步验证"));
        }
        if (co_await attemptLimitReached(loop, session.userId))
        {
            co_return std::unexpected(tooManyRequests("两步验证尝试过于频繁：请 5 分钟后再试"));
        }

        const std::optional<std::string> pending = co_await m_cache->get(loop, setupCacheKey(session.userId));
        if (!pending.has_value() || pending->empty())
        {
            co_return std::unexpected(badRequest("两步验证设置已过期，请重新开始设置"));
        }
        const std::optional<MfaCiphertext> stored = parseSetupCiphertext(*pending);
        if (!stored.has_value())
        {
            co_return std::unexpected(badRequest("两步验证设置已过期，请重新开始设置"));
        }
        const std::optional<std::string> pendingSecret = MfaCrypto::decrypt(stored->ciphertext, stored->nonce, m_key);
        if (!pendingSecret.has_value())
        {
            co_return std::unexpected(badRequest("两步验证设置已过期，请重新开始设置"));
        }
        const std::optional<std::int64_t> matchedStep = matchingStepForSecret(*pendingSecret, trimCopy(code), Totp::currentStep(), 0);
        if (!matchedStep.has_value())
        {
            co_await recordAttemptFailure(loop, session.userId);
            co_return std::unexpected(unauthorized("验证码错误，请确认身份验证器时间后重试"));
        }

        const std::vector<std::string> recoveryCodes = generateRecoveryCodes();
        std::vector<std::string>       hashes;
        hashes.reserve(recoveryCodes.size());
        for (const std::string &recoveryCode: recoveryCodes)
        {
            hashes.push_back(recoveryHash(m_key, recoveryCode));
        }

        const std::string now = Common::nowDateTimeUtc();
        MfaRecord         record;
        record.userId           = session.userId;
        record.tenantId         = session.tenantId;
        record.secretCiphertext = stored->ciphertext;
        record.secretNonce      = stored->nonce;
        record.keyVersion       = 1;
        record.lastUsedStep     = *matchedStep;
        record.recoveryHashes   = hashes;
        record.enabled          = false;
        record.createdAt        = now;
        record.updatedAt        = now;
        // 启用行在 beginSetup 时已经建好；这里只用一次条件 UPDATE 同时写入密文、恢复码与启用位
        if (!co_await repository->enable(loop, record, serializeHashes(hashes), *matchedStep, now))
        {
            co_return std::unexpected(conflict("两步验证状态已被并发修改，请刷新后重试"));
        }
        static_cast<void>(co_await m_cache->erase(loop, setupCacheKey(session.userId)));
        co_await clearAttemptFailures(loop, session.userId);
        co_return recoveryCodes;
    }

    AsynGyanis::Core::Task<std::expected<void, MfaError>> MfaService::disable(AsynGyanis::Core::EventLoop &loop, SessionRecord session, std::string code)
    {
        if (!keyConfigured())
        {
            co_return std::unexpected(serviceUnavailable("未配置两步验证加密密钥，请联系管理员配置 EDGEPULSE_MFA_KEY 后重试"));
        }
        const std::shared_ptr<MfaRepository> repository = m_repository.scoped({session.tenantId, false});
        const std::optional<MfaRecord>       existing   = co_await repository->findByUser(loop, session.userId);
        if (!existing.has_value() || !existing->enabled)
        {
            co_return std::unexpected(notFound("两步验证未启用"));
        }
        if (co_await attemptLimitReached(loop, session.userId))
        {
            co_return std::unexpected(tooManyRequests("两步验证尝试过于频繁：请 5 分钟后再试"));
        }
        // 与 verify 共用步号水位：登录时刚用过的口令不能再关闭两步验证
        const std::optional<std::int64_t> matched = matchingStep(*existing, trimCopy(code), Totp::currentStep(), existing->lastUsedStep);
        if (!matched.has_value())
        {
            co_await recordAttemptFailure(loop, session.userId);
            co_return std::unexpected(unauthorized("验证码错误或已使用，请等待身份验证器刷新后重试"));
        }
        // 先条件消费再删除：并发 verify 抢先用掉同一步时，这里拿到 false 而不是删掉记录
        if (!co_await repository->consumeStep(loop, session.userId, *matched, Common::nowDateTimeUtc()))
        {
            co_await recordAttemptFailure(loop, session.userId);
            co_return std::unexpected(unauthorized("验证码错误或已使用，请等待身份验证器刷新后重试"));
        }
        if (!co_await repository->remove(loop, session.userId))
        {
            co_return std::unexpected(notFound("两步验证未启用"));
        }
        static_cast<void>(co_await m_cache->erase(loop, setupCacheKey(session.userId)));
        co_await clearAttemptFailures(loop, session.userId);
        co_return std::expected<void, MfaError>{};
    }

    AsynGyanis::Core::Task<std::expected<std::vector<std::string>, MfaError>> MfaService::regenerateRecoveryCodes(AsynGyanis::Core::EventLoop &loop, SessionRecord session,
                                                                                                                  std::string code)
    {
        if (!keyConfigured())
        {
            co_return std::unexpected(serviceUnavailable("未配置两步验证加密密钥，请联系管理员配置 EDGEPULSE_MFA_KEY 后重试"));
        }
        const std::shared_ptr<MfaRepository> repository = m_repository.scoped({session.tenantId, false});
        const std::optional<MfaRecord>       existing   = co_await repository->findByUser(loop, session.userId);
        if (!existing.has_value() || !existing->enabled)
        {
            co_return std::unexpected(notFound("两步验证未启用"));
        }
        if (co_await attemptLimitReached(loop, session.userId))
        {
            co_return std::unexpected(tooManyRequests("两步验证尝试过于频繁：请 5 分钟后再试"));
        }
        // 与登录共用步号水位：刚用于登录或关闭的验证码不能再签发恢复码
        const std::optional<std::int64_t> matched = matchingStep(*existing, trimCopy(code), Totp::currentStep(), existing->lastUsedStep);
        if (!matched.has_value() || !co_await repository->consumeStep(loop, session.userId, *matched, Common::nowDateTimeUtc()))
        {
            co_await recordAttemptFailure(loop, session.userId);
            co_return std::unexpected(unauthorized("验证码错误或已使用，请等待身份验证器刷新后重试"));
        }

        const std::vector<std::string> recoveryCodes = generateRecoveryCodes();
        std::vector<std::string>       hashes;
        hashes.reserve(recoveryCodes.size());
        for (const std::string &recoveryCode: recoveryCodes)
        {
            hashes.push_back(recoveryHash(m_key, recoveryCode));
        }
        if (!co_await repository->updateRecoveryCodes(loop, session.userId, serializeHashes(hashes), Common::nowDateTimeUtc()))
        {
            co_return std::unexpected(conflict("两步验证记录已被并发修改，请刷新后重试"));
        }
        co_await clearAttemptFailures(loop, session.userId);
        co_return recoveryCodes;
    }

    AsynGyanis::Core::Task<bool> MfaService::verify(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId, std::string code)
    {
        if (!keyConfigured())
        {
            co_return false;
        }
        // 登录校验只知道用户标识，先用主键读出租户，再把写路径收回到该租户的视图
        const std::optional<MfaRecord> record = co_await m_repository.scoped({0, true})->findByUser(loop, userId);
        if (!record.has_value() || !record->enabled)
        {
            co_return false;
        }
        const std::optional<std::int64_t> matched = matchingStep(*record, trimCopy(code), Totp::currentStep(), record->lastUsedStep);
        if (!matched.has_value())
        {
            co_return false;
        }
        co_return co_await m_repository.scoped({record->tenantId, false})->consumeStep(loop, userId, *matched, Common::nowDateTimeUtc());
    }

    AsynGyanis::Core::Task<bool> MfaService::verifyRecovery(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId, std::string code)
    {
        if (!keyConfigured())
        {
            co_return false;
        }
        const std::optional<MfaRecord> record = co_await m_repository.scoped({0, true})->findByUser(loop, userId);
        if (!record.has_value() || !record->enabled)
        {
            co_return false;
        }
        const std::string canonical = canonicalRecoveryCode(code);
        if (canonical.empty())
        {
            co_return false;
        }
        co_return co_await m_repository.scoped({record->tenantId, false})->consumeRecoveryCode(loop, userId, recoveryHash(m_key, canonical), Common::nowDateTimeUtc());
    }

    AsynGyanis::Core::Task<bool> MfaService::enabled(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        const std::optional<MfaRecord> record = co_await m_repository.scoped({0, true})->findByUser(loop, userId);
        co_return record.has_value() && record->enabled;
    }

    AsynGyanis::Core::Task<MfaStatus> MfaService::status(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        const std::optional<MfaRecord> record = co_await m_repository.scoped({0, true})->findByUser(loop, userId);
        MfaStatus                      status;
        if (record.has_value() && record->enabled)
        {
            status.enabled                = true;
            status.remainingRecoveryCodes = record->recoveryHashes.size();
        }
        co_return status;
    }

    AsynGyanis::Core::Task<bool> MfaService::reset(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        const bool removed = co_await m_repository.scoped({0, true})->remove(loop, userId);
        static_cast<void>(co_await m_cache->erase(loop, setupCacheKey(userId)));
        co_await clearAttemptFailures(loop, userId);
        co_return removed;
    }
} // namespace EdgePulse::Auth
