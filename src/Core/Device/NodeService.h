/**
 * @file NodeService.h
 * @brief 节点写操作的校验与错误归一
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Api/Pagination.h"
#include "Core/Device/NodeRepository.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace EdgePulse::Device
{
    /**
     * @brief 节点服务的可恢复错误
     */
    struct NodeServiceError
    {
        enum class Reason
        {
            Invalid,  ///< 输入不合法
            NotFound, ///< 节点不存在
            Conflict, ///< 名称重复或版本冲突
        };

        Reason      reason{Reason::Invalid}; ///< 机器可判的原因
        std::string message;                 ///< 中文可操作说明
    };

    /**
     * @brief 节点业务服务
     */
    class NodeService
    {
    public:
        /**
         * @brief 用仓储构造
         * @param repository 节点仓储
         */
        explicit NodeService(std::shared_ptr<NodeRepository> repository);

        /**
         * @brief 分页查询
         */
        AsynGyanis::Core::Task<NodePage> list(AsynGyanis::Core::EventLoop &loop, Common::PageParameters parameters, std::string keyword, std::string status,
                                              std::int64_t groupId = 0, NodeListScope scope = {});

        /**
         * @brief 创建节点
         * @param loop 恢复协程用的事件循环
         * @param name 节点名，1..128 个字符
         * @param labelsJson 标签 JSON 对象文本，可空
         * @return 成功返回节点；失败返回可恢复错误
         */
        AsynGyanis::Core::Task<std::expected<NodeRecord, NodeServiceError>> create(AsynGyanis::Core::EventLoop &loop, std::string name, std::string labelsJson,
                                                                                   std::int64_t groupId = 200, std::int64_t ownerDeptId = 0, std::int64_t createdBy = 0);

        /**
         * @brief 乐观锁更新
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点标识
         * @param name 新名称；空表示不改
         * @param labelsJson 新标签 JSON；空表示不改
         * @param expectedVersion 期望版本
         * @return 成功返回更新后的节点；失败返回可恢复错误
         */
        AsynGyanis::Core::Task<std::expected<NodeRecord, NodeServiceError>> update(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId, std::optional<std::string> name,
                                                                                   std::optional<std::string> labelsJson, std::optional<std::int64_t> groupId,
                                                                                   std::int64_t expectedVersion);

        /**
         * @brief 软删除
         * @return 是否命中一行
         */
        AsynGyanis::Core::Task<bool> remove(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId);

        /**
         * @brief 按标识查节点
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点标识
         * @return std::optional<NodeRecord> 节点
         */
        AsynGyanis::Core::Task<std::optional<NodeRecord>> find(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId);

    private:
        std::shared_ptr<NodeRepository> m_repository; ///< 节点仓储
    };
} // namespace EdgePulse::Device
