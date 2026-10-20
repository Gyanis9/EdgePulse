/**
 * @file NodeRepository.h
 * @brief 边缘节点仓储
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Database/Database.h"
#include "Base/Api/Pagination.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace EdgePulse::Device
{
    /**
     * @brief 节点行
     */
    struct NodeRecord
    {
        std::int64_t id{0};           ///< 主键
        std::string  name;            ///< 节点名
        std::string  status;          ///< online / offline
        std::string  agentVersion;    ///< Agent 版本
        std::string  certFingerprint; ///< 自报的证书 SHA-256，小写
        std::string  labelsJson;      ///< 标签 JSON 文本，可空
        std::string  lastSeen;        ///< 最近心跳时间，可空
        std::int64_t groupId{0};      ///< 所属分组
        std::string  groupName;       ///< 分组名
        std::int64_t ownerDeptId{0};  ///< 归属部门，供数据范围
        std::int64_t version{1};      ///< 乐观锁版本
    };

    /**
     * @brief 一页节点
     */
    struct NodePage
    {
        std::vector<NodeRecord> rows;     ///< 当前页
        std::int64_t            total{0}; ///< 总行数
    };

    /**
     * @brief 更新失败的两类可恢复原因
     */
    enum class NodeMutationFailure
    {
        NotFound, ///< 节点不存在或已删除
        Conflict, ///< 版本不匹配
    };

    /**
     * @brief Agent 注册输入
     */
    struct AgentUpsert
    {
        std::string name;            ///< 节点名
        std::string agentVersion;    ///< Agent 版本
        std::string certFingerprint; ///< 自报证书指纹
        std::string labelsJson;      ///< 标签 JSON 文本，可空
    };

    /**
     * @brief 节点与 Agent 的 SQL 访问
     */
    class NodeRepository
    {
    public:
        /**
         * @brief 用数据库封装构造
         * @param database 进程级数据库
         */
        explicit NodeRepository(std::shared_ptr<Common::Database> database);

        /**
         * @brief 分页查询节点
         * @param loop 恢复协程用的事件循环
         * @param parameters 分页参数
         * @param keyword 名称模糊匹配，可空
         * @param status 状态过滤，可空
         * @return 当前页与总数
         */
        AsynGyanis::Core::Task<NodePage> list(AsynGyanis::Core::EventLoop &loop, Common::PageParameters parameters, std::string keyword, std::string status,
                                              std::int64_t groupId = 0);

        /**
         * @brief 按主键查未删除节点
         */
        AsynGyanis::Core::Task<std::optional<NodeRecord>> findById(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId);

        /**
         * @brief 按名称查未删除节点
         */
        AsynGyanis::Core::Task<std::optional<NodeRecord>> findByName(AsynGyanis::Core::EventLoop &loop, std::string name);

        /**
         * @brief 创建节点
         * @param loop 恢复协程用的事件循环
         * @param name 节点名
         * @param labelsJson 标签 JSON，可空
         * @return 新建节点
         */
        AsynGyanis::Core::Task<NodeRecord> create(AsynGyanis::Core::EventLoop &loop, std::string name, std::string labelsJson, std::int64_t groupId = 200,
                                                  std::int64_t ownerDeptId = 0);

        /**
         * @brief 带乐观锁更新节点
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点标识
         * @param name 新名称；空表示不改
         * @param labelsJson 新标签；空表示不改
         * @param expectedVersion 期望版本
         * @return 成功返回更新后的节点；失败区分不存在与版本冲突
         */
        AsynGyanis::Core::Task<std::expected<NodeRecord, NodeMutationFailure>> update(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId, std::optional<std::string> name,
                                                                                      std::optional<std::string> labelsJson, std::optional<std::int64_t> groupId,
                                                                                      std::int64_t expectedVersion);

        /**
         * @brief 软删除节点
         * @return 是否命中一行
         */
        AsynGyanis::Core::Task<bool> remove(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId);

        /**
         * @brief Agent 注册：按名称更新或新建，并置为在线
         */
        AsynGyanis::Core::Task<NodeRecord> upsertAgent(AsynGyanis::Core::EventLoop &loop, AgentUpsert registration);

        /**
         * @brief 心跳：更新在线状态与最近时间
         * @return 节点不存在时为空
         */
        AsynGyanis::Core::Task<std::optional<NodeRecord>> heartbeat(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId, std::string agentVersion);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
    };
} // namespace EdgePulse::Device
