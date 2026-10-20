#include "Core/Http/Authorization.h"

#include "Net/Http/HttpRequest.h"

#include <cctype>
#include <string_view>

namespace EdgePulse::Web
{
    std::optional<std::string> bearerToken(const AsynGyanis::Net::HttpRequest &request)
    {
        constexpr std::string_view       kBearerPrefix = "Bearer ";
        const std::optional<std::string> header        = request.getHeader("authorization");
        if (!header.has_value() || header->size() <= kBearerPrefix.size())
        {
            return std::nullopt;
        }
        // 前缀按 RFC 9110 的大小写不敏感规则比对；令牌本体保持原样
        for (std::size_t index = 0; index < kBearerPrefix.size(); ++index)
        {
            const auto actual   = static_cast<unsigned char>((*header)[index]);
            const auto expected = static_cast<unsigned char>(kBearerPrefix[index]);
            if (std::tolower(actual) != std::tolower(expected))
            {
                return std::nullopt;
            }
        }
        return header->substr(kBearerPrefix.size());
    }
} // namespace EdgePulse::Web
