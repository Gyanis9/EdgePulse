/**
 * @file DataScopeService.h
 * @brief 角色数据范围解析
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Database/Database.h"

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Auth
{
    /**
     * @brief 一个账号可见的数据范围
     * @details 多条角色范围取并集：只要有一条 ALL 就是全部；否则按部门集合与仅本人判断。
     */
    struct DataScope
    {
        bool                      all{false};      ///< 可见全部数据
        bool                      selfOnly{false}; ///< 仅本人
        std::int64_t              userId{0};       ///< 账号
        std::vector<std::int64_t> deptIds;         ///< 可见部门
    };

    /**
     * @brief 从角色配置解析数据范围
     */
    class DataScopeService
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         */
        explicit DataScopeService(std::shared_ptr<Common::Database> database);

        /**
         * @brief 解析账号的数据范围
         * @param loop 恢复协程用的事件循环
         * @param userId 账号
         * @return DataScope 数据范围
         */
        [[nodiscard]] AsynGyanis::Core::Task<DataScope> resolve(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
    };
} // namespace EdgePulse::Auth
