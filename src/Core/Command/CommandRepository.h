/**
 * @file CommandRepository.h
 * @brief 节点命令队列
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Api/Pagination.h"
#include "Base/Database/Database.h"
#include "Core/System/TenantScope.h"

#include <Core/Coroutine/Task.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Command
{
    /**
     * @brief 一条命令
     */
    struct CommandRecord
    {
        std::int64_t id{0};              ///< 主键
        std::int64_t tenantId{1};        ///< 归属租户
        std::int64_t nodeId{0};          ///< 目标节点
        std::string  nodeName;           ///< 节点名
        std::string  type;               ///< 命令类型
        std::string  payload;            ///< 载荷 JSON
        std::string  status;             ///< pending/running/succeeded/failed/timeout/canceled
        std::string  result;             ///< 结果 JSON
        std::string  message;            ///< 说明
        std::int64_t createdBy{0};       ///< 创建人
        std::string  createdAt;          ///< 创建时间
        std::string  dispatchedAt;       ///< 下发时间
        std::string  finishedAt;         ///< 结束时间
        int          timeoutSeconds{60}; ///< 超时秒数
        std::string  scheduledAt;        ///< 计划执行时间，空表示立即
        std::int64_t batchId{0};         ///< 批量标识，0 表示单条
    };

    /**
     * @brief 命令仓储
     */
    class CommandRepository
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         */
        explicit CommandRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant = {});

        /// 按租户边界派生仓储视图
        [[nodiscard]] std::shared_ptr<CommandRepository> scoped(System::TenantScope tenant) const;

        /**
         * @brief 创建命令
         * @param loop 恢复协程用的事件循环
         * @param nodeId 目标节点
         * @param type 命令类型
         * @param payload JSON 载荷
         * @param createdBy 创建人
         * @param timeoutSeconds 超时秒数
         * @param scheduledAt 计划执行时间（SQL DATETIME），空表示立即
         * @param batchId 批量标识，0 表示单条
         * @return std::optional<CommandRecord> 新命令；节点不存在时为空
         */
        AsynGyanis::Core::Task<std::optional<CommandRecord>> create(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId, std::string type, std::string payload,
                                                                    std::int64_t createdBy, int timeoutSeconds, std::string scheduledAt = {}, std::int64_t batchId = 0);

        /**
         * @brief 分页查询命令
         * @param loop 恢复协程用的事件循环
         * @param status 状态过滤
         * @param nodeId 节点过滤，0 表示全部
         * @param page 分页
         * @return Common::PageRows<CommandRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<CommandRecord>> list(AsynGyanis::Core::EventLoop &loop, std::string status, std::int64_t nodeId,
                                                                     const Common::PageParameters &page);

        /**
         * @brief 按标识取命令
         * @param loop 恢复协程用的事件循环
         * @param commandId 命令标识
         * @return std::optional<CommandRecord> 命令
         */
        AsynGyanis::Core::Task<std::optional<CommandRecord>> find(AsynGyanis::Core::EventLoop &loop, std::int64_t commandId);

        /**
         * @brief 为 Agent 取最早一条待执行命令并置为执行中
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点
         * @return std::optional<CommandRecord> 命令；没有待执行时为空
         */
        AsynGyanis::Core::Task<std::optional<CommandRecord>> takeNext(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId);

        /**
         * @brief 取已到执行时间的待下发命令
         * @param loop 恢复协程用的事件循环
         * @param limit 本次最多取多少条
         * @return std::vector<CommandRecord> 命令
         */
        AsynGyanis::Core::Task<std::vector<CommandRecord>> listDue(AsynGyanis::Core::EventLoop &loop, std::size_t limit = 100);

        /**
         * @brief 标记命令已下发
         * @param loop 恢复协程用的事件循环
         * @param commandId 命令标识
         * @return bool 是否命中
         */
        AsynGyanis::Core::Task<bool> markDispatched(AsynGyanis::Core::EventLoop &loop, std::int64_t commandId);

        /**
         * @brief 写入执行结果
         * @param loop 恢复协程用的事件循环
         * @param commandId 命令标识
         * @param status succeeded/failed/timeout
         * @param result 结果 JSON
         * @param message 说明
         * @return std::optional<CommandRecord> 更新后的命令
         */
        AsynGyanis::Core::Task<std::optional<CommandRecord>> finish(AsynGyanis::Core::EventLoop &loop, std::int64_t commandId, std::string status, std::string result,
                                                                    std::string message);

        /**
         * @brief 取消尚未结束的命令
         * @param loop 恢复协程用的事件循环
         * @param commandId 命令标识
         * @return std::optional<CommandRecord> 更新后的命令
         */
        AsynGyanis::Core::Task<std::optional<CommandRecord>> cancel(AsynGyanis::Core::EventLoop &loop, std::int64_t commandId);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
        System::TenantScope               m_tenant;   ///< 租户边界
    };
} // namespace EdgePulse::Command
