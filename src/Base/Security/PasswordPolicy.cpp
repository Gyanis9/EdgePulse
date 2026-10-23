#include "Base/Security/PasswordPolicy.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace EdgePulse::Common
{
    namespace
    {
        constexpr std::size_t kMaximumPasswordLength = 64; ///< 与建号、重置接口共用的上限
    } // namespace

    PasswordPolicy::PasswordPolicy(const int minimumLength, const bool requireComplexity) : m_minimumLength(minimumLength), m_requireComplexity(requireComplexity)
    {
    }

    std::expected<void, std::string> PasswordPolicy::validate(const std::string_view password) const
    {
        if (password.size() < static_cast<std::size_t>(m_minimumLength) || password.size() > kMaximumPasswordLength)
        {
            return std::unexpected("口令长度必须是 " + std::to_string(m_minimumLength) + " 到 " + std::to_string(kMaximumPasswordLength) + " 个字符");
        }
        if (!m_requireComplexity)
        {
            return {};
        }
        bool hasUpper  = false;
        bool hasLower  = false;
        bool hasDigit  = false;
        bool hasSymbol = false;
        for (const unsigned char character: password)
        {
            hasUpper  = hasUpper || std::isupper(character) != 0;
            hasLower  = hasLower || std::islower(character) != 0;
            hasDigit  = hasDigit || std::isdigit(character) != 0;
            hasSymbol = hasSymbol || (!std::isalnum(character) && !std::isspace(character));
        }
        if (!hasUpper || !hasLower || !hasDigit || !hasSymbol)
        {
            return std::unexpected("口令必须同时包含大写字母、小写字母、数字与符号");
        }
        return {};
    }
} // namespace EdgePulse::Common
