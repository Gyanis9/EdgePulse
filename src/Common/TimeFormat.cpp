#include "Common/TimeFormat.h"

#include <array>
#include <ctime>

namespace EdgePulse::Common
{
    std::string formatUtc(const std::chrono::system_clock::time_point point)
    {
        const std::time_t seconds = std::chrono::system_clock::to_time_t(point);
        std::tm           utc{};
#if defined(_WIN32)
        gmtime_s(&utc, &seconds);
#else
        gmtime_r(&seconds, &utc);
#endif
        std::array<char, 32> buffer{};
        const std::size_t    written = std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return std::string(buffer.data(), written);
    }

    std::string nowIso8601Utc()
    {
        return formatUtc(std::chrono::system_clock::now());
    }
} // namespace EdgePulse::Common
