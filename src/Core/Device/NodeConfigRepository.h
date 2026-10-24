/**
 * @file NodeConfigRepository.h
 * @brief 节点期望/实际配置与不可变修订的 SQL 访问
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Api/Pagination.h"
#include "Base/Database/Database.h"
#include "Core/System/TenantScope.h"

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
     * @brief 一个节点的配置影子
     */
    struct NodeConfigRecord
    {
        std::int64_t nodeId{0};           ///< 节点标识
        std::int64_t tenantId{1};         ///< 归属租户
        std::string  desiredJson;         ///< 期望配置，规范化后的 JSON 文本
        std::int64_t desiredVersion{0};   ///< 期望配置版本；0 表示尚未下发
        std::string  desiredHash;         ///< 期望配置的 SHA-256，小写十六进制
        std::string  desiredUpdatedAt;    ///< 期望配置写入时间
        std::int64_t desiredUpdatedBy{0}; ///< 期望配置写入人
        std::string  reportedJson;        ///< Agent 上报的配置，规范化后的 JSON 文本
        std::int64_t reportedVersion{0};  ///< Agent 上报的版本
        std::string  reportedHash;        ///< Agent 上报内容的 SHA-256
        std::string  reportedStatus;      ///< applied / failed / pending；空表示尚未上报
        std::string  reportedMessage;     ///< Agent 上报的说明文本，最长 255 字节
        std::string  reportedAt;          ///< 最近一次上报时间，可空
        bool         inSync{false};       ///< 版本、hash 相同且状态为 applied
    };

    /**
     * @brief 一条不可变配置修订
     */
    struct NodeConfigRevision
    {
        std::int64_t id{0};        ///< 修订标识
        std::int64_t nodeId{0};    ///< 节点标识
        std::int64_t version{0};   ///< 对应期望配置版本
        std::string  contentJson;  ///< 写入时的规范化内容
        std::string  contentHash;  ///< 内容的 SHA-256
        std::string  note;         ///< 写入说明
        std::int64_t createdBy{0}; ///< 写入人
        std::string  createdAt;    ///< 写入时间
    };

    /**
     * @brief 配置写入失败的可恢复原因
     */
    enum class NodeConfigFailure
    {
        Invalid,         ///< 内容不合法
        NotFound,        ///< 节点、配置或修订不存在
        VersionConflict, ///< expectedVersion 与当前版本不一致
    };

    /**
     * @brief 配置影子与修订的 SQL 访问
     * @note 写入时按节点自身租户落库，平台管理员的全租户视图不会把配置写进自己的租户
     */
    class NodeConfigRepository
    {
    public:
        /**
         * @brief 用数据库封装构造
         * @param database 进程级数据库
         * @param tenant 可见租户范围
         */
        explicit NodeConfigRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant = {});

        /**
         * @brief 按租户边界派生仓储视图
         * @param tenant 新视图可见的租户范围；全租户视图不过滤 tenant_id
         * @return 共享同一数据库、使用新租户边界的仓储
         */
        [[nodiscard]] std::shared_ptr<NodeConfigRepository> scoped(System::TenantScope tenant) const;

        /**
         * @brief 查节点的配置影子
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点标识
         * @return 没有配置行时为空；有配置行时按期望与上报计算 inSync
         */
        AsynGyanis::Core::Task<std::optional<NodeConfigRecord>> find(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId);

        /**
         * @brief 读节点当前的期望配置版本
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点标识
         * @return 没有配置行时为 0
         * @note 心跳用它告知 Agent 是否该拉取新配置；调用方必须显式取全租户视图
         */
        AsynGyanis::Core::Task<std::int64_t> desiredVersion(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId);

        /**
         * @brief 推进期望配置版本并写一条修订
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点标识
         * @param contentJson 规范化后的期望配置
         * @param contentHash 内容摘要
         * @param expectedVersion 调用方读到的当前版本；0 表示首次写入
         * @param note 修订说明
         * @param operatorId 写人人标识
         * @return 成功返回新影子；版本冲突或并发锁在重试后仍失败返回 VersionConflict，节点不可见返回 NotFound
         * @note 读版本、写入、记修订与清理窗口外旧修订在同一事务里完成；整段事务在可重试的 SQL 并发失败后最多尝试三次
         * @note 保留窗口是含最新版本在内的最近 100 条修订，更早的版本在同一事务里删除
         */
        AsynGyanis::Core::Task<std::expected<NodeConfigRecord, NodeConfigFailure>> applyDesired(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId, std::string contentJson,
                                                                                                std::string contentHash, std::int64_t expectedVersion, std::string note,
                                                                                                std::int64_t operatorId);

        /**
         * @brief 记录 Agent 上报的配置与状态
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点标识
         * @param version 上报的版本
         * @param contentJson 规范化后的上报内容；空表示保留上一次内容，只更新状态
         * @param contentHash 上报内容摘要；contentJson 为空时忽略
         * @param status applied / failed / pending
         * @param message 上报说明，最长 255 字节；空串表示本次没有说明
         * @return 成功返回新影子；节点或配置行不存在返回 NotFound
         * @note hash 由服务端按内容重算，仓储只接受已经规范化的内容
         * @note 整段事务在可重试的 SQL 并发失败后最多尝试三次，重试用尽时把最后一次失败抛出
         */
        AsynGyanis::Core::Task<std::expected<NodeConfigRecord, NodeConfigFailure>> report(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId, std::int64_t version,
                                                                                          std::string contentJson, std::string contentHash, std::string status,
                                                                                          std::string message);

        /**
         * @brief 分页查询修订，按版本降序
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点标识
         * @param page 分页参数
         * @return 当前页与总数
         */
        AsynGyanis::Core::Task<Common::PageRows<NodeConfigRevision>> listRevisions(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId, const Common::PageParameters &page);

        /**
         * @brief 按标识取一条修订
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点标识
         * @param revisionId 修订标识
         * @return 不属于该节点或不在租户范围内时为空
         */
        AsynGyanis::Core::Task<std::optional<NodeConfigRevision>> findRevision(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId, std::int64_t revisionId);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
        System::TenantScope               m_tenant;   ///< 租户边界
    };
} // namespace EdgePulse::Device
