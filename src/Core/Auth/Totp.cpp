#include "Core/Auth/Totp.h"

#include "Base/Coding/SecureCompare.h"
#include "Base/Security/RandomBytes.h"
#include "Core/Crypto/Digest.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace EdgePulse::Auth::Totp
{
    namespace
    {
        constexpr std::string_view kBase32Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
        constexpr std::string_view kHexDigits      = "0123456789ABCDEF";
        constexpr std::size_t      kCounterBytes   = 8;
        constexpr std::uint32_t    kTruncationMask = 0x7FU;
        constexpr int              kMinimumDigits  = 1;
        constexpr int              kMaximumDigits  = 9;

        /**
         * @brief 把 base32 字符映射到 5 位数值
         * @param character 待映射字符
         * @return std::optional<std::uint8_t> 合法时返回 0 到 31
         */
        [[nodiscard]] std::optional<std::uint8_t> base32Value(const char character)
        {
            const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            const auto found = kBase32Alphabet.find(upper);
            if (found == std::string_view::npos)
            {
                return std::nullopt;
            }
            return static_cast<std::uint8_t>(found);
        }

        /**
         * @brief 按 RFC 3986 编码一段 URI 组件，只保留非保留字符
         * @param text 待编码文本
         * @return std::string 百分号编码结果
         */
        [[nodiscard]] std::string percentEncode(const std::string_view text)
        {
            std::string encoded;
            encoded.reserve(text.size());
            for (const unsigned char character: text)
            {
                // 只按 ASCII 非保留字符放行；其余字节逐个百分号编码，避免依赖 locale 的字符分类
                const bool unreserved = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
                                        character == '-' || character == '.' || character == '_' || character == '~';
                if (unreserved)
                {
                    encoded.push_back(static_cast<char>(character));
                    continue;
                }
                encoded.push_back('%');
                encoded.push_back(kHexDigits[character >> 4U]);
                encoded.push_back(kHexDigits[character & 0x0FU]);
            }
            return encoded;
        }
    } // namespace

    std::string base32Encode(const std::span<const std::uint8_t> bytes)
    {
        std::string encoded;
        encoded.reserve((bytes.size() * 8U + 4U) / 5U);
        std::uint32_t buffer = 0;
        int           bits   = 0;
        for (const std::uint8_t byte: bytes)
        {
            buffer = (buffer << 8U) | byte;
            bits += 8;
            while (bits >= 5)
            {
                bits -= 5;
                encoded.push_back(kBase32Alphabet[(buffer >> static_cast<unsigned int>(bits)) & 0x1FU]);
            }
        }
        // 收尾不足 5 位时把剩余位移到高位再取一组，末尾不补 '='
        if (bits > 0)
        {
            encoded.push_back(kBase32Alphabet[(buffer << static_cast<unsigned int>(5 - bits)) & 0x1FU]);
        }
        return encoded;
    }

    std::optional<std::vector<std::uint8_t>> base32Decode(const std::string_view text)
    {
        std::vector<std::uint8_t> decoded;
        std::uint32_t             buffer = 0;
        int                       bits   = 0;
        for (const char character: text)
        {
            // 展示时常按 4 字符分组，空白不算内容；填充符一律忽略，无填充写法也照常接受
            if (std::isspace(static_cast<unsigned char>(character)) != 0 || character == '=')
            {
                continue;
            }
            const std::optional<std::uint8_t> value = base32Value(character);
            if (!value.has_value())
            {
                return std::nullopt;
            }
            buffer = (buffer << 5U) | *value;
            bits += 5;
            if (bits >= 8)
            {
                bits -= 8;
                decoded.push_back(static_cast<std::uint8_t>((buffer >> static_cast<unsigned int>(bits)) & 0xFFU));
            }
        }
        // 余下 5 位以上说明多出了一整组；更少的余位必须全为零，否则同一字节串会有多种写法
        if (bits > 0)
        {
            if (bits >= 5)
            {
                return std::nullopt;
            }
            if ((buffer & ((1U << static_cast<unsigned int>(bits)) - 1U)) != 0)
            {
                return std::nullopt;
            }
        }
        return decoded;
    }

    std::string generateSecret()
    {
        const std::vector<std::uint8_t> bytes = Common::randomSecretBytes(kSecretByteCount);
        return base32Encode(bytes);
    }

    std::int64_t currentStep()
    {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        return seconds / kDefaultPeriodSeconds;
    }

    std::string code(const std::string_view secret, const std::int64_t step, const int digits)
    {
        if (step < 0 || digits < kMinimumDigits || digits > kMaximumDigits)
        {
            return {};
        }
        const std::optional<std::vector<std::uint8_t>> key = base32Decode(secret);
        if (!key.has_value() || key->empty())
        {
            return {};
        }

        std::array<std::uint8_t, kCounterBytes> counter{};
        auto                                    remaining = static_cast<std::uint64_t>(step);
        for (std::size_t index = kCounterBytes; index > 0; --index)
        {
            counter[index - 1] = static_cast<std::uint8_t>(remaining & 0xFFU);
            remaining >>= 8U;
        }
        const std::string counterText(reinterpret_cast<const char *>(counter.data()), counter.size());
        const std::string keyText(reinterpret_cast<const char *>(key->data()), key->size());
        const auto        digest = AsynGyanis::Core::Digest::hmacSha1(keyText, counterText);

        // RFC 4226 §5.3 的动态截断：低 4 位选偏移，再取 31 位无符号整数
        const std::size_t   offset    = static_cast<std::size_t>(digest[digest.size() - 1] & 0x0FU);
        const std::uint32_t truncated = ((static_cast<std::uint32_t>(digest[offset]) & kTruncationMask) << 24U) | (static_cast<std::uint32_t>(digest[offset + 1]) << 16U) |
                                        (static_cast<std::uint32_t>(digest[offset + 2]) << 8U) | static_cast<std::uint32_t>(digest[offset + 3]);
        std::uint32_t       modulus   = 1;
        for (int index = 0; index < digits; ++index)
        {
            modulus *= 10;
        }
        std::string result = std::to_string(truncated % modulus);
        if (result.size() < static_cast<std::size_t>(digits))
        {
            result.insert(0, static_cast<std::size_t>(digits) - result.size(), '0');
        }
        return result;
    }

    bool verify(const std::string_view secret, const std::string_view code, const std::int64_t nowStep, const std::int64_t lastUsedStep, const int window)
    {
        if (code.size() != static_cast<std::size_t>(kDefaultDigits) || window < 0)
        {
            return false;
        }
        for (const char character: code)
        {
            if (std::isdigit(static_cast<unsigned char>(character)) == 0)
            {
                return false;
            }
        }
        for (int offset = -window; offset <= window; ++offset)
        {
            const std::int64_t candidate = nowStep + offset;
            // 已消费的步号不再接受，否则同一枚口令能在窗口内重复使用
            if (candidate <= lastUsedStep)
            {
                continue;
            }
            const std::string expected = Totp::code(secret, candidate);
            if (!expected.empty() && AsynGyanis::Base::constantTimeEquals(expected, code))
            {
                return true;
            }
        }
        return false;
    }

    std::string otpauthUri(const std::string_view issuer, const std::string_view account, const std::string_view secret, const int digits, const int periodSeconds)
    {
        return "otpauth://totp/" + percentEncode(issuer) + ":" + percentEncode(account) + "?secret=" + std::string{secret} + "&issuer=" + percentEncode(issuer) +
               "&digits=" + std::to_string(digits) + "&period=" + std::to_string(periodSeconds);
    }
} // namespace EdgePulse::Auth::Totp
