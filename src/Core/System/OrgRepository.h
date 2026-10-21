/**
 * @file OrgRepository.h
 * @brief 部门与岗位仓储
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Api/Pagination.h"
#include "Base/Database/Database.h"
#include "Core/System/SystemModels.h"
#include "Core/System/TenantScope.h"

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace EdgePulse::System
{
    /**
     * @brief 部门与岗位仓储
     */
    class OrgRepository
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         * @param tenant 租户边界
         */
        explicit OrgRepository(std::shared_ptr<Common::Database> database, TenantScope tenant = {});

        /// 按租户边界派生仓储视图
        [[nodiscard]] std::shared_ptr<OrgRepository> scoped(TenantScope tenant) const;

        /**
         * @brief 部门树
         * @param loop 恢复协程用的事件循环
         * @param keyword 名称关键字
         * @param status 空、0、1
         * @return std::vector<DeptRecord> 根部门
         */
        AsynGyanis::Core::Task<std::vector<DeptRecord>> deptTree(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status);

        /**
         * @brief 全部部门平铺列表，供数据范围与下拉使用
         * @param loop 恢复协程用的事件循环
         * @return std::vector<DeptRecord> 部门
         */
        AsynGyanis::Core::Task<std::vector<DeptRecord>> allDepts(AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 按标识查部门
         * @param loop 恢复协程用的事件循环
         * @param deptId 部门标识
         * @return std::optional<DeptRecord> 部门
         */
        AsynGyanis::Core::Task<std::optional<DeptRecord>> findDept(AsynGyanis::Core::EventLoop &loop, std::int64_t deptId);

        /**
         * @brief 同父节点下部门名是否重复
         * @param loop 恢复协程用的事件循环
         * @param parentId 父部门
         * @param name 部门名
         * @param excludeId 排除的部门
         * @return bool 是否重复
         */
        AsynGyanis::Core::Task<bool> deptNameExists(AsynGyanis::Core::EventLoop &loop, std::int64_t parentId, std::string name, std::int64_t excludeId = 0);

        /**
         * @brief 创建部门
         * @param loop 恢复协程用的事件循环
         * @param write 字段
         * @param operatorId 操作人
         * @return std::int64_t 新部门标识
         */
        AsynGyanis::Core::Task<std::int64_t> createDept(AsynGyanis::Core::EventLoop &loop, const DeptWrite &write, std::int64_t operatorId);

        /**
         * @brief 更新部门
         * @param loop 恢复协程用的事件循环
         * @param deptId 部门标识
         * @param write 字段
         * @param operatorId 操作人
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> updateDept(AsynGyanis::Core::EventLoop &loop, std::int64_t deptId, const DeptWrite &write, std::int64_t operatorId);

        /**
         * @brief 软删除部门
         * @param loop 恢复协程用的事件循环
         * @param deptId 部门标识
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> removeDept(AsynGyanis::Core::EventLoop &loop, std::int64_t deptId);

        /**
         * @brief 部门下是否还有子部门
         * @param loop 恢复协程用的事件循环
         * @param deptId 部门标识
         * @return bool 是否有子部门
         */
        AsynGyanis::Core::Task<bool> deptHasChildren(AsynGyanis::Core::EventLoop &loop, std::int64_t deptId);

        /**
         * @brief 部门下是否还有用户
         * @param loop 恢复协程用的事件循环
         * @param deptId 部门标识
         * @return bool 是否有用户
         */
        AsynGyanis::Core::Task<bool> deptHasUsers(AsynGyanis::Core::EventLoop &loop, std::int64_t deptId);

        /**
         * @brief 分页查询岗位
         * @param loop 恢复协程用的事件循环
         * @param keyword 关键字
         * @param status 空、0、1
         * @param page 分页
         * @return Common::PageRows<PostRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<PostRecord>> listPosts(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status,
                                                                       const Common::PageParameters &page);

        /**
         * @brief 导出岗位
         * @param loop 恢复协程用的事件循环
         * @param keyword 关键字
         * @param status 状态
         * @return std::vector<PostRecord> 行
         */
        AsynGyanis::Core::Task<std::vector<PostRecord>> exportPosts(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status);

        /**
         * @brief 岗位下拉
         * @param loop 恢复协程用的事件循环
         * @return std::vector<PostRecord> 启用岗位
         */
        AsynGyanis::Core::Task<std::vector<PostRecord>> postOptions(AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 岗位编码是否占用
         * @param loop 恢复协程用的事件循环
         * @param code 编码
         * @param excludeId 排除项
         * @return bool 是否占用
         */
        AsynGyanis::Core::Task<bool> postCodeExists(AsynGyanis::Core::EventLoop &loop, std::string code, std::int64_t excludeId = 0);

        /**
         * @brief 创建岗位
         * @param loop 恢复协程用的事件循环
         * @param write 字段
         * @param operatorId 操作人
         * @return std::int64_t 新岗位标识
         */
        AsynGyanis::Core::Task<std::int64_t> createPost(AsynGyanis::Core::EventLoop &loop, const PostWrite &write, std::int64_t operatorId);

        /**
         * @brief 更新岗位
         * @param loop 恢复协程用的事件循环
         * @param postId 岗位标识
         * @param write 字段
         * @param operatorId 操作人
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> updatePost(AsynGyanis::Core::EventLoop &loop, std::int64_t postId, const PostWrite &write, std::int64_t operatorId);

        /**
         * @brief 软删除岗位
         * @param loop 恢复协程用的事件循环
         * @param postId 岗位标识
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> removePost(AsynGyanis::Core::EventLoop &loop, std::int64_t postId);

        /**
         * @brief 岗位是否仍被用户使用
         * @param loop 恢复协程用的事件循环
         * @param postId 岗位标识
         * @return bool 是否仍有用户
         */
        AsynGyanis::Core::Task<bool> postHasUsers(AsynGyanis::Core::EventLoop &loop, std::int64_t postId);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
        TenantScope                       m_tenant;   ///< 租户边界
    };
} // namespace EdgePulse::System
