#include "Base/Runtime/TimeFormat.h"

#include <array>
#include <ctime>

namespace EdgePulse::Common
{
    namespace
    {
        [[nodiscard]] std::string formatWithPattern(const std::chrono::system_clock::time_point point, const char *pattern)
        {
            const std::time_t seconds = std::chrono::system_clock::to_time_t(point);
            std::tm           utc{};
#if defined(_WIN32)
            gmtime_s(&utc, &seconds);
#else
            gmtime_r(&seconds, &utc);
#endif
            std::array<char, 32> buffer{};
            const std::size_t    written = std::strftime(buffer.data(), buffer.size(), pattern, &utc);
            return std::string(buffer.data(), written);
        }
    } // namespace

    std::string formatUtc(const std::chrono::system_clock::time_point point)
    {
        return formatWithPattern(point, "%Y-%m-%dT%H:%M:%SZ");
    }

    std::string nowIso8601Utc()
    {
        return formatUtc(std::chrono::system_clock::now());
    }

    std::string formatDateTimeUtc(const std::chrono::system_clock::time_point point)
    {
        return formatWithPattern(point, "%Y-%m-%d %H:%M:%S");
    }

    std::string nowDateTimeUtc()
    {
        return formatDateTimeUtc(std::chrono::system_clock::now());
    }
} // namespace EdgePulse::Common
