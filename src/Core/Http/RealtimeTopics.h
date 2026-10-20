/**
 * @file RealtimeTopics.h
 * @brief 实时推送主题的租户命名约定
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace EdgePulse::Web
{
    /**
     * @brief 租户实时主题：base + ":" + tenantId
     * @param base 主题基名，如 nodes / alarms / commands
     * @param tenantId 租户标识
     * @return std::string 租户专属主题
     */
    [[nodiscard]] inline std::string tenantTopic(const std::string_view base, const std::int64_t tenantId)
    {
        return std::string{base} + ":" + std::to_string(tenantId);
    }
} // namespace EdgePulse::Web
