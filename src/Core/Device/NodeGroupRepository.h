/**
 * @file NodeGroupRepository.h
 * @brief 节点分组仓储
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
#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Device
{
    /**
     * @brief 节点分组
     */
    struct NodeGroupRecord
    {
        std::int64_t                 id{0};        ///< 主键
        std::int64_t                 parentId{0};  ///< 父分组
        std::string                  name;         ///< 名称
        int                          sort{0};      ///< 排序
        std::string                  remark;       ///< 备注
        std::int64_t                 nodeCount{0}; ///< 直属节点数
        std::string                  createTime;   ///< 创建时间
        std::vector<NodeGroupRecord> children;     ///< 子分组
    };

    /**
     * @brief 节点分组仓储
     */
    class NodeGroupRepository
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         */
        explicit NodeGroupRepository(std::shared_ptr<Common::Database> database);

        /**
         * @brief 分组树
         * @param loop 恢复协程用的事件循环
         * @return std::vector<NodeGroupRecord> 根分组
         */
        AsynGyanis::Core::Task<std::vector<NodeGroupRecord>> tree(AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 分组是否存在
         * @param loop 恢复协程用的事件循环
         * @param groupId 分组标识
         * @return bool 是否存在
         */
        AsynGyanis::Core::Task<bool> exists(AsynGyanis::Core::EventLoop &loop, std::int64_t groupId);

        /**
         * @brief 创建分组
         * @param loop 恢复协程用的事件循环
         * @param parentId 父分组
         * @param name 名称
         * @param sort 排序
         * @param remark 备注
         * @param operatorId 操作人
         * @return std::int64_t 新分组标识
         */
        AsynGyanis::Core::Task<std::int64_t> create(AsynGyanis::Core::EventLoop &loop, std::int64_t parentId, std::string name, int sort, std::string remark,
                                                    std::int64_t operatorId);

        /**
         * @brief 更新分组
         * @param loop 恢复协程用的事件循环
         * @param groupId 分组标识
         * @param parentId 父分组，空表示不改
         * @param name 名称，空表示不改
         * @param sort 排序，空表示不改
         * @param remark 备注，空表示不改
         * @param operatorId 操作人
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> update(AsynGyanis::Core::EventLoop &loop, std::int64_t groupId, std::optional<std::int64_t> parentId, std::optional<std::string> name,
                                            std::optional<int> sort, std::optional<std::string> remark, std::int64_t operatorId);

        /**
         * @brief 删除分组
         * @param loop 恢复协程用的事件循环
         * @param groupId 分组标识
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> remove(AsynGyanis::Core::EventLoop &loop, std::int64_t groupId);

        /**
         * @brief 分组下是否还有子分组或节点
         * @param loop 恢复协程用的事件循环
         * @param groupId 分组标识
         * @return bool 是否非空
         */
        AsynGyanis::Core::Task<bool> isInUse(AsynGyanis::Core::EventLoop &loop, std::int64_t groupId);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
    };
} // namespace EdgePulse::Device
