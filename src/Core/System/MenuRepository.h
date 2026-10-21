/**
 * @file MenuRepository.h
 * @brief 菜单树读取与管理
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Database/Database.h"
#include "Core/System/SystemModels.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace EdgePulse::System
{
    /**
     * @brief 前端动态路由所需的菜单节点
     */
    struct MenuNode
    {
        std::int64_t          id{0};       ///< 主键
        std::int64_t          parentId{0}; ///< 父节点，0 为根
        std::string           name;        ///< 展示名
        std::string           type;        ///< dir / menu / btn
        std::string           path;        ///< 前端路径
        std::string           component;   ///< 组件标识
        std::string           permission;  ///< 权限码
        std::string           icon;        ///< 图标标识
        int                   sort{0};     ///< 排序值
        int                   visible{1};  ///< 1 可见、0 隐藏
        int                   status{1};   ///< 1 启用、0 停用
        std::vector<MenuNode> children;    ///< 子节点
    };

    /**
     * @brief 菜单仓储
     */
    class MenuRepository
    {
    public:
        /**
         * @brief 用数据库封装构造
         * @param database 进程级数据库
         */
        explicit MenuRepository(std::shared_ptr<Common::Database> database);

        /**
         * @brief 取用户可见的菜单树
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return std::vector<MenuNode> 根节点列表
         */
        AsynGyanis::Core::Task<std::vector<MenuNode>> treeForUser(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 取管理端完整菜单树（含按钮）
         * @param loop 恢复协程用的事件循环
         * @param keyword 名称、路径或权限码关键字
         * @param status 空、0、1
         * @return std::vector<MenuNode> 根节点列表
         */
        AsynGyanis::Core::Task<std::vector<MenuNode>> tree(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status);

        /**
         * @brief 按标识查菜单
         * @param loop 恢复协程用的事件循环
         * @param menuId 菜单标识
         * @return std::optional<MenuNode> 菜单
         */
        AsynGyanis::Core::Task<std::optional<MenuNode>> findById(AsynGyanis::Core::EventLoop &loop, std::int64_t menuId);

        /**
         * @brief 创建菜单
         * @param loop 恢复协程用的事件循环
         * @param write 菜单字段
         * @param operatorId 操作人
         * @return std::int64_t 新菜单标识
         */
        AsynGyanis::Core::Task<std::int64_t> create(AsynGyanis::Core::EventLoop &loop, const MenuWrite &write, std::int64_t operatorId);

        /**
         * @brief 更新菜单
         * @param loop 恢复协程用的事件循环
         * @param menuId 菜单标识
         * @param write 菜单字段
         * @param operatorId 操作人
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> update(AsynGyanis::Core::EventLoop &loop, std::int64_t menuId, const MenuWrite &write, std::int64_t operatorId);

        /**
         * @brief 删除菜单并清理角色授权
         * @param loop 恢复协程用的事件循环
         * @param menuId 菜单标识
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> remove(AsynGyanis::Core::EventLoop &loop, std::int64_t menuId);

        /**
         * @brief 菜单下是否还有子节点
         * @param loop 恢复协程用的事件循环
         * @param menuId 菜单标识
         * @return bool 是否有子节点
         */
        AsynGyanis::Core::Task<bool> hasChildren(AsynGyanis::Core::EventLoop &loop, std::int64_t menuId);

        /**
         * @brief 一组菜单携带的权限码
         * @param loop 恢复协程用的事件循环
         * @param menuIds 菜单标识
         * @return std::vector<std::string> 非空权限码
         */
        AsynGyanis::Core::Task<std::vector<std::string>> permissionsFor(AsynGyanis::Core::EventLoop &loop, std::vector<std::int64_t> menuIds);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
    };
} // namespace EdgePulse::System
