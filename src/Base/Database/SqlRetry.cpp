#include "Base/Database/SqlRetry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <string>
#include <string_view>

namespace EdgePulse::Sql
{
    namespace
    {
        constexpr std::array<std::string_view, 6> kRetryableMarkers{"locked", "busy", "deadlock", "unique", "duplicate entry", "lock wait timeout"}; ///< 并发失败原文
        constexpr std::array<std::string_view, 2> kUniqueMarkers{"unique", "duplicate"};                                                             ///< 唯一键冲突原文

        /**
         * @brief 把错误文本折成小写，便于按固定标记匹配
         * @param message 驱动原文
         * @return std::string 小写副本
         */
        [[nodiscard]] std::string lowerCopy(const std::string_view message)
        {
            std::string lowered;
            lowered.reserve(message.size());
            for (const char character: message)
            {
                lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
            }
            return lowered;
        }

        /**
         * @brief 判断已折成小写的文本是否含任一标记
         * @param lowered 小写错误文本
         * @param markers 待匹配的标记集合
         * @return true 表示至少命中一个标记
         */
        [[nodiscard]] bool containsAnyMarker(const std::string_view lowered, const std::span<const std::string_view> markers)
        {
            return std::any_of(markers.begin(), markers.end(), [lowered](const std::string_view marker) { return lowered.find(marker) != std::string_view::npos; });
        }
    } // namespace

    bool isRetryableSqlFailure(const std::string_view message)
    {
        return containsAnyMarker(lowerCopy(message), kRetryableMarkers);
    }

    bool isUniqueViolation(const std::string_view message)
    {
        return containsAnyMarker(lowerCopy(message), kUniqueMarkers);
    }
} // namespace EdgePulse::Sql
