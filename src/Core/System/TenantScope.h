/**
 * @file TenantScope.h
 * @brief 租户可见范围
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>

namespace EdgePulse::System
{
    /**
     * @brief 一次查询的租户边界
     * @details allTenants 只授予平台管理员；普通租户账号必须带 tenantId 过滤。
     */
    struct TenantScope
    {
        std::int64_t tenantId{1};      ///< 当前租户
        bool         allTenants{false}; ///< 是否跨租户（仅平台管理员）

        /**
         * @brief 判断某行是否在范围内
         * @param ownerTenantId 行归属租户
         * @return bool 是否可见
         */
        [[nodiscard]] bool includes(const std::int64_t ownerTenantId) const noexcept
        {
            return allTenants || ownerTenantId == tenantId;
        }
    };
} // namespace EdgePulse::System
