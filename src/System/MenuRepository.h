/**
 * @file MenuRepository.h
 * @brief 按用户角色读取菜单树
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Common/Database.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <memory>
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
        std::string           component;   ///< 组件标识，如 device/Nodes
        std::string           permission;  ///< 权限码，目录可为空
        std::string           icon;        ///< 图标标识
        int                   sort{0};     ///< 排序值
        int                   visible{1};  ///< 1 可见、0 隐藏
        int                   status{1};   ///< 1 启用、0 停用
        std::vector<MenuNode> children;    ///< 子节点，按 sort 与 id 排序
    };

    /**
     * @brief 菜单查询
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
         * @return 根节点列表；目录在前，按 sort 与 id 排序
         */
        AsynGyanis::Core::Task<std::vector<MenuNode>> treeForUser(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
    };
} // namespace EdgePulse::System
