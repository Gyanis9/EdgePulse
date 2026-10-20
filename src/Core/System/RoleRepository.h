/**
 * @file RoleRepository.h
 * @brief 角色、菜单授权与数据范围仓储
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Api/Pagination.h"
#include "Base/Database/Database.h"
#include "Core/System/SystemModels.h"

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace EdgePulse::System
{
    /**
     * @brief 角色仓储
     */
    class RoleRepository
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         */
        explicit RoleRepository(std::shared_ptr<Common::Database> database);

        /**
         * @brief 分页查询角色
         * @param loop 恢复协程用的事件循环
         * @param keyword 名称或编码关键字
         * @param status 空、0、1
         * @param page 分页参数
         * @return Common::PageRows<RoleRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<RoleRecord>> list(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status, const Common::PageParameters &page);

        /**
         * @brief 导出角色（最多一万行）
         * @param loop 恢复协程用的事件循环
         * @param keyword 关键字
         * @param status 状态
         * @return std::vector<RoleRecord> 行
         */
        AsynGyanis::Core::Task<std::vector<RoleRecord>> exportAll(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status);

        /**
         * @brief 按标识查角色
         * @param loop 恢复协程用的事件循环
         * @param roleId 角色标识
         * @return std::optional<RoleRecord> 角色
         */
        AsynGyanis::Core::Task<std::optional<RoleRecord>> findById(AsynGyanis::Core::EventLoop &loop, std::int64_t roleId);

        /**
         * @brief 角色编码是否占用
         * @param loop 恢复协程用的事件循环
         * @param code 角色编码
         * @param excludeRoleId 排除的角色
         * @return bool 是否占用
         */
        AsynGyanis::Core::Task<bool> codeExists(AsynGyanis::Core::EventLoop &loop, std::string code, std::int64_t excludeRoleId = 0);

        /**
         * @brief 创建角色并写授权
         * @param loop 恢复协程用的事件循环
         * @param write 角色字段
         * @param operatorId 操作人
         * @return std::int64_t 新角色标识
         */
        AsynGyanis::Core::Task<std::int64_t> create(AsynGyanis::Core::EventLoop &loop, const RoleWrite &write, std::int64_t operatorId);

        /**
         * @brief 更新角色；menuIds 有值时整体替换授权
         * @param loop 恢复协程用的事件循环
         * @param roleId 角色标识
         * @param write 角色字段
         * @param operatorId 操作人
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> update(AsynGyanis::Core::EventLoop &loop, std::int64_t roleId, const RoleWrite &write, std::int64_t operatorId);

        /**
         * @brief 软删除角色并清理授权与用户关联
         * @param loop 恢复协程用的事件循环
         * @param roleId 角色标识
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> remove(AsynGyanis::Core::EventLoop &loop, std::int64_t roleId);

        /**
         * @brief 取角色已授权菜单
         * @param loop 恢复协程用的事件循环
         * @param roleId 角色标识
         * @return std::vector<std::int64_t> 菜单标识
         */
        AsynGyanis::Core::Task<std::vector<std::int64_t>> menuIdsFor(AsynGyanis::Core::EventLoop &loop, std::int64_t roleId);

        /**
         * @brief 取角色自定义数据范围的部门
         * @param loop 恢复协程用的事件循环
         * @param roleId 角色标识
         * @return std::vector<std::int64_t> 部门标识
         */
        AsynGyanis::Core::Task<std::vector<std::int64_t>> deptIdsFor(AsynGyanis::Core::EventLoop &loop, std::int64_t roleId);

        /**
         * @brief 替换角色菜单授权
         * @param loop 恢复协程用的事件循环
         * @param roleId 角色标识
         * @param menuIds 菜单标识
         * @return bool 是否成功
         */
        AsynGyanis::Core::Task<bool> updateMenus(AsynGyanis::Core::EventLoop &loop, std::int64_t roleId, std::vector<std::int64_t> menuIds);

        /**
         * @brief 取启用角色，供用户表单下拉
         * @param loop 恢复协程用的事件循环
         * @return std::vector<RoleRecord> 角色
         */
        AsynGyanis::Core::Task<std::vector<RoleRecord>> listOptions(AsynGyanis::Core::EventLoop &loop);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
    };
} // namespace EdgePulse::System
