#include "Core/Auth/CaptchaService.h"

#include "Base/Coding/SecureCompare.h"
#include "Base/Security/RandomBytes.h"
#include "Base/Security/RandomToken.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Auth
{
    namespace
    {
        constexpr std::array<char, 32>        kCodeAlphabet{'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K', 'L', 'M', 'N', 'P', 'Q', 'R',
                                                            'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '2', '3', '4', '5', '6', '7', '8', '9'};
        constexpr std::array<const char *, 5> kTextColors{"#1f3a5f", "#7a3e1d", "#2d5a3d", "#6b2c5a", "#3f3f46"};
        constexpr std::size_t                 kCodeLength = 4;
        constexpr std::chrono::seconds        kRateWindow{60};
        constexpr std::string_view            kChallengePrefix = "edgepulse:captcha:challenge:";
        constexpr std::string_view            kRatePrefix      = "edgepulse:captcha:rate:";

        /**
         * @brief ASCII 大写归一，用于答案比较
         * @param text 原始文本
         * @return std::string 大写文本
         */
        [[nodiscard]] std::string upperCopy(const std::string_view text)
        {
            std::string result{text};
            for (char &character: result)
            {
                if (character >= 'a' && character <= 'z')
                {
                    character = static_cast<char>(character - ('a' - 'A'));
                }
            }
            return result;
        }

        /**
         * @brief 生成 4 位答案
         * @return std::string 答案
         */
        [[nodiscard]] std::string makeCode()
        {
            const std::vector<std::uint8_t> random = Common::randomBytes(kCodeLength);
            std::string                     code;
            code.reserve(kCodeLength);
            for (const std::uint8_t value: random)
            {
                // 字母表恰为 32 项，取模不引入偏置
                code.push_back(kCodeAlphabet[value % kCodeAlphabet.size()]);
            }
            return code;
        }

        /**
         * @brief 生成可渲染的验证码 SVG
         * @param code 4 位答案
         * @return std::string SVG 文本
         */
        [[nodiscard]] std::string makeSvg(const std::string_view code)
        {
            const std::vector<std::uint8_t> noise = Common::randomBytes(16);
            std::string                     svg   = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"40\" viewBox=\"0 0 120 40\">"
                                                    "<rect width=\"120\" height=\"40\" rx=\"6\" fill=\"#f5f7fa\"/>";
            for (int index = 0; index < 2; ++index)
            {
                const int x1 = 4 + (noise[static_cast<std::size_t>(index) * 4] % 110);
                const int y1 = 4 + (noise[static_cast<std::size_t>(index) * 4 + 1] % 30);
                const int x2 = 4 + (noise[static_cast<std::size_t>(index) * 4 + 2] % 110);
                const int y2 = 4 + (noise[static_cast<std::size_t>(index) * 4 + 3] % 30);
                svg += "<path d=\"M" + std::to_string(x1) + " " + std::to_string(y1) + " L" + std::to_string(x2) + " " + std::to_string(y2) +
                       "\" stroke=\"#c7d2fe\" stroke-width=\"1.4\"/>";
            }
            for (std::size_t index = 0; index < code.size(); ++index)
            {
                const int x        = 14 + static_cast<int>(index) * 26;
                const int rotation = static_cast<int>(index) * 5 - 8 + static_cast<int>(noise[index] % 9) - 4;
                svg += "<text x=\"" + std::to_string(x) + "\" y=\"28\" font-size=\"24\" font-family=\"monospace\" font-weight=\"700\" fill=\"" +
                       kTextColors[noise[index] % kTextColors.size()] + "\" transform=\"rotate(" + std::to_string(rotation) + " " + std::to_string(x) + " 24)\">" +
                       std::string(1, code[index]) + "</text>";
            }
            svg += "</svg>";
            return svg;
        }
    } // namespace

    CaptchaService::CaptchaService(std::shared_ptr<Cache::KeyValueStore> store, CaptchaOptions options) :
        m_store(std::move(store)), m_ttlSeconds(options.ttlSeconds), m_requestsPerMinute(options.requestsPerMinute)
    {
    }

    AsynGyanis::Core::Task<std::expected<CaptchaChallenge, CaptchaError>> CaptchaService::issue(AsynGyanis::Core::EventLoop &loop, std::string clientIp)
    {
        const std::int64_t issued = co_await m_store->increment(loop, std::string{kRatePrefix} + clientIp, kRateWindow);
        // 限流按窗口计数；超限时连答案都不生成，避免给撞库者提供可试的凭据
        if (issued > m_requestsPerMinute.load(std::memory_order_relaxed))
        {
            co_return std::unexpected(CaptchaError{CaptchaErrorReason::RateLimited, "验证码请求过于频繁：请稍后再试或更换来源 IP"});
        }

        CaptchaChallenge challenge;
        challenge.uuid = Common::randomTokenHex(16);
        challenge.code = makeCode();
        challenge.svg  = makeSvg(challenge.code);
        const bool stored =
                co_await m_store->set(loop, std::string{kChallengePrefix} + challenge.uuid, challenge.code, std::chrono::seconds{m_ttlSeconds.load(std::memory_order_relaxed)});
        if (!stored)
        {
            co_return std::unexpected(CaptchaError{CaptchaErrorReason::RateLimited, "验证码存储不可用：请检查缓存后端"});
        }
        co_return challenge;
    }

    void CaptchaService::setOptions(const CaptchaOptions options) noexcept
    {
        if (options.ttlSeconds > 0)
        {
            m_ttlSeconds.store(options.ttlSeconds, std::memory_order_relaxed);
        }
        if (options.requestsPerMinute > 0)
        {
            m_requestsPerMinute.store(options.requestsPerMinute, std::memory_order_relaxed);
        }
    }

    AsynGyanis::Core::Task<bool> CaptchaService::consume(AsynGyanis::Core::EventLoop &loop, const std::string_view uuid, const std::string_view code)
    {
        const std::string key = std::string{kChallengePrefix} + std::string{uuid};
        // 取值即删除：答案对错都在这一步消耗掉，同一个 uuid 无法被反复试答案
        const std::optional<std::string> stored = co_await m_store->take(loop, key);
        if (!stored.has_value())
        {
            co_return false;
        }
        co_return AsynGyanis::Base::constantTimeEquals(upperCopy(*stored), upperCopy(code));
    }
} // namespace EdgePulse::Auth
