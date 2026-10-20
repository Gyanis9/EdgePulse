#include "Common/Environment.h"

#include <cstdlib>
#include <string>

namespace EdgePulse::Common
{
    std::optional<std::string> environmentValue(const std::string_view name)
    {
        const std::string variableName{name};
#if defined(_MSC_VER)
        char  *buffer = nullptr;
        size_t length = 0;
        if (_dupenv_s(&buffer, &length, variableName.c_str()) != 0 || buffer == nullptr)
        {
            return std::nullopt;
        }
        std::string value(buffer);
        std::free(buffer);
#else
        const char *environmentValueText = std::getenv(variableName.c_str());
        if (environmentValueText == nullptr)
        {
            return std::nullopt;
        }
        std::string value(environmentValueText);
#endif
        if (value.empty())
        {
            return std::nullopt;
        }
        return value;
    }
} // namespace EdgePulse::Common
