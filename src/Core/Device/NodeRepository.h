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
#include "Core/System/TenantScope.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <cstddef>
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
        std::int64_t tenantId{1};     ///< 归属租户
        std::string  name;            ///< 节点名
        std::string  status;          ///< online / offline
        std::string  agentVersion;    ///< Agent 版本
        std::string  certFingerprint; ///< 自报的证书 SHA-256，小写
        std::string  certNotAfter;    ///< 叶证书到期时间，可空
        std::string  certRevokedAt;   ///< 证书吊销时间，可空
        std::string  labelsJson;      ///< 标签 JSON 文本，可空
        std::string  lastSeen;        ///< 最近心跳时间，可空
        std::int64_t groupId{0};      ///< 所属分组
        std::string  groupName;       ///< 分组名
        std::int64_t ownerDeptId{0};  ///< 归属部门，供数据范围
        std::int64_t createdBy{0};    ///< 创建人，供 SELF 范围
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
     * @brief 一次心跳的结果
     */
    struct HeartbeatOutcome
    {
        NodeRecord node;                ///< 更新后的节点
        bool       becameOnline{false}; ///< 心跳前是否不在线
    };

    /**
     * @brief Agent 注册的结果
     */
    struct AgentUpsertOutcome
    {
        NodeRecord node;                ///< 注册后的节点
        bool       becameOnline{false}; ///< 注册前是否不在线
        bool       created{false};      ///< 是否新建了节点
    };

    /**
     * @brief Agent 注册被拒的原因
     */
    enum class AgentUpsertFailure
    {
        TokenRejected,    ///< 注册/改绑令牌无效、过期、已用或与节点名不符
        RebindNotAllowed, ///< 已有节点换了证书但没有改绑令牌
    };

    /**
     * @brief 按状态统计的节点数
     */
    struct NodeStatusCounts
    {
        std::int64_t online{0};  ///< 在线节点数
        std::int64_t offline{0}; ///< 离线节点数
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
        std::string  name;                     ///< 节点名
        std::string  agentVersion;             ///< Agent 版本
        std::string  certFingerprint;          ///< 自报证书指纹
        std::string  labelsJson;               ///< 标签 JSON 文本，可空
        std::int64_t tenantId{1};              ///< 归属租户
        std::string  enrollmentTokenHash;      ///< 令牌明文的 SHA-256；空表示没有令牌
        std::string  enrollmentAction;         ///< enroll / rebind
        bool         requireEnrollment{false}; ///< 本次注册是否必须消费令牌
        bool         allowRebind{true};        ///< 已有节点换证书时是否允许改绑
        std::string  certNotAfter;             ///< 叶证书到期时间，可空
    };

    /**
     * @brief 节点列表的数据范围
     */
    struct NodeListScope
    {
        bool                      all{true};       ///< 可见全部
        std::vector<std::int64_t> allowedDeptIds;  ///< 可见部门；仅在 all 为假时有意义
        bool                      selfOnly{false}; ///< 仅本人创建
        std::int64_t              userId{0};       ///< 当前账号
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
        explicit NodeRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant = {});

        /// 按租户边界派生仓储视图
        [[nodiscard]] std::shared_ptr<NodeRepository> scoped(System::TenantScope tenant) const;

        /**
         * @brief 分页查询节点
         * @param loop 恢复协程用的事件循环
         * @param parameters 分页参数
         * @param keyword 名称模糊匹配，可空
         * @param status 状态过滤，可空
         * @return 当前页与总数
         */
        AsynGyanis::Core::Task<NodePage> list(AsynGyanis::Core::EventLoop &loop, Common::PageParameters parameters, std::string keyword, std::string status,
                                              std::int64_t groupId = 0, NodeListScope scope = {});

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
                                                  std::int64_t ownerDeptId = 0, std::int64_t createdBy = 0);

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
         * @param loop 恢复协程用的事件循环
         * @param registration 注册输入
         * @return 成功返回节点与在线转换标记；令牌校验在同一事务内完成
         * @note 名称唯一键被并发注册抢占时，回滚后按已有节点重试一次
         */
        AsynGyanis::Core::Task<std::expected<AgentUpsertOutcome, AgentUpsertFailure>> upsertAgent(AsynGyanis::Core::EventLoop &loop, AgentUpsert registration);

        /**
         * @brief 吊销节点证书：清空指纹、置为离线并记录吊销时间
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点标识
         * @return 成功返回更新后的节点；节点不存在或已删除时为空
         */
        AsynGyanis::Core::Task<std::optional<NodeRecord>> revokeCertificate(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId);

        /**
         * @brief 统计指定天数内到期或已经到期的证书
         * @param loop 恢复协程用的事件循环
         * @param withinDays 观察窗口天数
         * @return 未吊销且未删除节点的证书数
         */
        AsynGyanis::Core::Task<std::int64_t> countExpiringCertificates(AsynGyanis::Core::EventLoop &loop, int withinDays);

        /**
         * @brief 心跳：更新在线状态与最近时间
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点标识
         * @param agentVersion Agent 版本；空表示不改
         * @return 节点不存在时为空；becameOnline 表示更新前是否不在线
         */
        AsynGyanis::Core::Task<std::optional<HeartbeatOutcome>> heartbeat(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId, std::string agentVersion);

        /**
         * @brief 把超时未心跳的在线节点批量置为离线
         * @param loop 恢复协程用的事件循环
         * @param cutoff 最近心跳早于该时刻即视为超时
         * @param limit 本次最多转换多少个节点
         * @return 本次真正转换的节点
         */
        AsynGyanis::Core::Task<std::vector<NodeRecord>> markStaleOffline(AsynGyanis::Core::EventLoop &loop, std::string cutoff, std::size_t limit);

        /**
         * @brief 查找已离线但缺少系统离线告警的节点
         * @param loop 恢复协程用的事件循环
         * @param limit 本次最多返回多少个节点
         * @return 需要补告警的节点；按仓储自身的租户视图查询
         */
        AsynGyanis::Core::Task<std::vector<NodeRecord>> listOfflineWithoutSystemAlarm(AsynGyanis::Core::EventLoop &loop, std::size_t limit);

        /**
         * @brief 按状态统计未删除节点
         * @param loop 恢复协程用的事件循环
         * @return 在线与离线节点数
         */
        AsynGyanis::Core::Task<NodeStatusCounts> countByStatus(AsynGyanis::Core::EventLoop &loop);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
        System::TenantScope               m_tenant;   ///< 租户边界
    };
} // namespace EdgePulse::Device
