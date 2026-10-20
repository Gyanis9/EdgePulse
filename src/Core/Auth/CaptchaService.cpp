#include "Core/Auth/CaptchaService.h"

#include "Base/Security/RandomToken.h"
#include "Base/Security/RandomBytes.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <mutex>
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
        constexpr std::size_t                 kPruneThreshold = 1024;

        /**
         * @brief ASCII 大小写无关比较
         */
        [[nodiscard]] bool equalsIgnoreCase(const std::string_view left, const std::string_view right)
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                const auto leftCharacter  = static_cast<unsigned char>(left[index]);
                const auto rightCharacter = static_cast<unsigned char>(right[index]);
                const auto leftLower      = leftCharacter >= 'A' && leftCharacter <= 'Z' ? static_cast<unsigned char>(leftCharacter + ('a' - 'A')) : leftCharacter;
                const auto rightLower     = rightCharacter >= 'A' && rightCharacter <= 'Z' ? static_cast<unsigned char>(rightCharacter + ('a' - 'A')) : rightCharacter;
                if (leftLower != rightLower)
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 生成 4 位答案
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
         * @return SVG 文本
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

    CaptchaService::CaptchaService(CaptchaOptions options, Clock clock) : m_options(options), m_clock(std::move(clock))
    {
    }

    std::chrono::steady_clock::time_point CaptchaService::now() const
    {
        return m_clock ? m_clock() : std::chrono::steady_clock::now();
    }

    void CaptchaService::pruneExpired(const std::chrono::steady_clock::time_point moment)
    {
        // 调用方已持锁；只清过期项，避免缓存被一次性验证码与限流窗口无界撑大
        for (auto iterator = m_challenges.begin(); iterator != m_challenges.end();)
        {
            iterator = iterator->second.expiresAt <= moment ? m_challenges.erase(iterator) : std::next(iterator);
        }
        for (auto iterator = m_rateWindows.begin(); iterator != m_rateWindows.end();)
        {
            const bool isOld = moment - iterator->second.windowStart >= kRateWindow && iterator->second.count == 0;
            iterator         = isOld ? m_rateWindows.erase(iterator) : std::next(iterator);
        }
    }

    std::expected<CaptchaChallenge, CaptchaError> CaptchaService::issue(const std::string &clientIp)
    {
        const std::chrono::steady_clock::time_point moment = now();
        std::lock_guard<std::mutex>                 lock(m_mutex);
        if (m_challenges.size() + m_rateWindows.size() > kPruneThreshold)
        {
            pruneExpired(moment);
        }

        RateWindow &window = m_rateWindows[clientIp];
        if (window.count == 0 || moment - window.windowStart >= kRateWindow)
        {
            window.windowStart = moment;
            window.count       = 0;
        }
        // 限流按窗口计数；超限时连答案都不生成，避免给撞库者提供可试的凭据
        if (window.count >= m_options.requestsPerMinute)
        {
            return std::unexpected(CaptchaError{CaptchaErrorReason::RateLimited, "验证码请求过于频繁：请稍后再试或更换来源 IP"});
        }
        ++window.count;

        CaptchaChallenge challenge;
        challenge.uuid = Common::randomTokenHex(16);
        challenge.code = makeCode();
        challenge.svg  = makeSvg(challenge.code);
        Entry entry;
        entry.code      = challenge.code;
        entry.expiresAt = moment + std::chrono::seconds(m_options.ttlSeconds);
        m_challenges.emplace(challenge.uuid, std::move(entry));
        return challenge;
    }

    bool CaptchaService::consume(const std::string_view uuid, const std::string_view code)
    {
        const std::chrono::steady_clock::time_point moment = now();
        std::lock_guard<std::mutex>                 lock(m_mutex);
        const auto                                  iterator = m_challenges.find(std::string{uuid});
        if (iterator == m_challenges.end())
        {
            return false;
        }
        const bool isExpired = iterator->second.expiresAt <= moment;
        const bool isMatch   = !isExpired && equalsIgnoreCase(iterator->second.code, code);
        // 一次性：无论对错都删掉，避免同一个 uuid 被反复试答案
        m_challenges.erase(iterator);
        return isMatch;
    }

    std::size_t CaptchaService::size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_challenges.size();
    }
} // namespace EdgePulse::Auth
