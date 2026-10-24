#include "Core/Auth/MfaService.h"

#include "Base/Coding/Base64.h"
#include "Base/Coding/SecureCompare.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Runtime/TimeFormat.h"
#include "Base/Security/RandomBytes.h"
#include "Core/Auth/MfaCrypto.h"
#include "Core/Auth/Totp.h"
#include "Core/Crypto/Digest.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
        constexpr std::size_t          kRecoveryCodeCount     = 10;
        constexpr std::size_t          kRecoveryCodeGroupSize = 4;
        constexpr std::string_view     kRecoveryAlphabet      = "123456789abcdefghjkmnpqrstuvwxyz";
        constexpr std::string_view     kIssuer                = "EdgePulse";

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
         * @param hashes SHA-256 十六进制
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
         * @brief 待启用秘密的缓存键
         * @param userId 用户标识
         * @return std::string 键
         */
        [[nodiscard]] std::string setupCacheKey(const std::int64_t userId)
        {
            return std::string{kSetupCachePrefix} + std::to_string(userId);
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
    } // namespace

    MfaService::MfaService(std::shared_ptr<Common::Database> database, std::shared_ptr<Cache::KeyValueStore> cache, std::string encryptionKey) :
        m_cache(std::move(cache)), m_repository(std::move(database)), m_key(decodeKey(encryptionKey))
    {
    }

    bool MfaService::keyConfigured() const noexcept
    {
        return m_key.size() == kMfaKeyBytes;
    }

    std::optional<std::string> MfaService::decryptSecret(const MfaRecord &record) const
    {
        if (!keyConfigured())
        {
            return std::nullopt;
        }
        return MfaCrypto::decrypt(record.secretCiphertext, record.secretNonce, m_key);
    }

    std::optional<std::int64_t> MfaService::matchingStep(const MfaRecord &record, const std::string_view code, const std::int64_t nowStep, const std::int64_t lastUsedStep) const
    {
        if (code.size() != static_cast<std::size_t>(Totp::kDefaultDigits))
        {
            return std::nullopt;
        }
        const std::optional<std::string> secret = decryptSecret(record);
        if (!secret.has_value())
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
            const std::string expected = Totp::code(*secret, candidate);
            if (!expected.empty() && AsynGyanis::Base::constantTimeEquals(expected, code))
            {
                return candidate;
            }
        }
        return std::nullopt;
    }

    AsynGyanis::Core::Task<std::expected<MfaSetup, MfaError>> MfaService::beginSetup(AsynGyanis::Core::EventLoop &loop, SessionRecord session)
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

        MfaSetup setup;
        setup.secret      = Totp::generateSecret();
        setup.otpauthUri  = Totp::otpauthUri(kIssuer, session.username, setup.secret);
        const bool cached = co_await m_cache->set(loop, setupCacheKey(session.userId), setup.secret, kSetupTtl);
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

        const std::optional<std::string> pending = co_await m_cache->get(loop, setupCacheKey(session.userId));
        if (!pending.has_value() || pending->empty())
        {
            co_return std::unexpected(badRequest("两步验证设置已过期，请重新开始设置"));
        }
        if (!Totp::verify(*pending, trimCopy(code), Totp::currentStep(), 0))
        {
            co_return std::unexpected(unauthorized("验证码错误，请确认身份验证器时间后重试"));
        }

        const std::optional<MfaCiphertext> encrypted = MfaCrypto::encrypt(*pending, m_key);
        if (!encrypted.has_value())
        {
            co_return std::unexpected(serviceUnavailable("两步验证密钥加密失败，请稍后重试"));
        }

        const std::vector<std::string> recoveryCodes = generateRecoveryCodes();
        std::vector<std::string>       hashes;
        hashes.reserve(recoveryCodes.size());
        for (const std::string &recoveryCode: recoveryCodes)
        {
            hashes.push_back(AsynGyanis::Core::Digest::sha256Hex(recoveryCode));
        }

        const std::string now = Common::nowDateTimeUtc();
        MfaRecord         record;
        record.userId           = session.userId;
        record.tenantId         = session.tenantId;
        record.secretCiphertext = encrypted->ciphertext;
        record.secretNonce      = encrypted->nonce;
        record.keyVersion       = 1;
        record.lastUsedStep     = 0;
        record.recoveryHashes   = hashes;
        record.enabled          = false;
        record.createdAt        = now;
        record.updatedAt        = now;
        co_await repository->upsertSecret(loop, std::move(record));
        if (!co_await repository->enable(loop, session.userId, serializeHashes(hashes), now))
        {
            co_return std::unexpected(conflict("两步验证状态已被并发修改，请刷新后重试"));
        }
        static_cast<void>(co_await m_cache->erase(loop, setupCacheKey(session.userId)));
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
        // 关闭只校验当前窗口，不推进步号：紧随登录之后的关闭请求不该被刚消费的步号拦住
        if (!matchingStep(*existing, trimCopy(code), Totp::currentStep(), 0).has_value())
        {
            co_return std::unexpected(unauthorized("验证码错误，请确认身份验证器时间后重试"));
        }
        if (!co_await repository->remove(loop, session.userId))
        {
            co_return std::unexpected(notFound("两步验证未启用"));
        }
        static_cast<void>(co_await m_cache->erase(loop, setupCacheKey(session.userId)));
        co_return std::expected<void, MfaError>{};
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
        co_return co_await m_repository.scoped({record->tenantId, false})
                ->consumeRecoveryCode(loop, userId, AsynGyanis::Core::Digest::sha256Hex(canonical), Common::nowDateTimeUtc());
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
        co_return removed;
    }
} // namespace EdgePulse::Auth
