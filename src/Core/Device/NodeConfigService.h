/**
 * @file NodeConfigService.h
 * @brief 设备影子的校验、版本推进、上报与回滚
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Command/CommandRepository.h"
#include "Core/Device/NodeConfigRepository.h"
#include "Core/Device/NodeRepository.h"
#include "Core/System/TenantScope.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace EdgePulse::Command
{
    class AgentChannelRegistry;
}

namespace EdgePulse::Device
{
    /**
     * @brief 配置操作的可恢复错误
     */
    struct NodeConfigError
    {
        NodeConfigFailure reason{NodeConfigFailure::Invalid}; ///< 机器可判的原因
        std::string       message;                            ///< 中文可操作说明
    };

    /**
     * @brief 设备影子业务服务
     * @note 内容在写入前统一规范化为紧凑 JSON 并重算 SHA-256，客户端自报的 hash 不参与判定
     */
    class NodeConfigService
    {
    public:
        /// 命令状态通知；由 App 注入实时总线
        using CommandNotifier = std::function<AsynGyanis::Core::Task<>(const Command::CommandRecord &)>;

        /**
         * @brief 构造
         * @param configs 配置仓储
         * @param nodes 节点仓储，用于校验节点存在与归属
         * @param commands 命令仓储；空表示不下发 config.apply
         * @param channels Agent 在线通道；空表示不直推
         * @param notifier 命令通知回调，可空
         */
        NodeConfigService(std::shared_ptr<NodeConfigRepository> configs, std::shared_ptr<NodeRepository> nodes, std::shared_ptr<Command::CommandRepository> commands,
                          Command::AgentChannelRegistry *channels, CommandNotifier notifier);

        /**
         * @brief 下发新的期望配置
         * @param loop 恢复协程用的事件循环
         * @param tenant 调用方的租户边界
         * @param nodeId 节点标识
         * @param content 原始 JSON 文本
         * @param expectedVersion 调用方读到的当前版本
         * @param note 修订说明
         * @param operatorId 操作人
         * @return 成功返回新影子；失败区分内容不合法、节点不存在与版本冲突
         * @note 版本推进成功后创建 config.apply 命令，在线 Agent 直推，离线留在待执行队列
         */
        AsynGyanis::Core::Task<std::expected<NodeConfigRecord, NodeConfigError>> applyDesired(AsynGyanis::Core::EventLoop &loop, System::TenantScope tenant, std::int64_t nodeId,
                                                                                              std::string content, std::int64_t expectedVersion, std::string note,
                                                                                              std::int64_t operatorId);

        /**
         * @brief 记录 Agent 上报的配置与状态
         * @param loop 恢复协程用的事件循环
         * @param tenant 调用方的租户边界
         * @param nodeId 节点标识
         * @param version 上报的版本，必须非负
         * @param content 可选的上报内容；空表示只更新状态
         * @param status applied / failed / pending
         * @param message 上报说明，最长 255 字节；落库的同时也进日志
         * @return 成功返回新影子；失败区分内容不合法、状态不合法与节点/配置不存在
         */
        AsynGyanis::Core::Task<std::expected<NodeConfigRecord, NodeConfigError>> report(AsynGyanis::Core::EventLoop &loop, System::TenantScope tenant, std::int64_t nodeId,
                                                                                        std::int64_t version, std::string content, std::string status, std::string message);

        /**
         * @brief 把一条历史修订的内容作为新版本重新下发
         * @param loop 恢复协程用的事件循环
         * @param tenant 调用方的租户边界
         * @param nodeId 节点标识
         * @param revisionId 修订标识
         * @param note 修订说明；空时默认「回滚到版本 N」
         * @param operatorId 操作人
         * @return 成功返回新影子；失败区分节点/修订不存在与版本冲突
         * @note 修订本身不改写，回滚同样走版本推进与 config.apply 下发
         */
        AsynGyanis::Core::Task<std::expected<NodeConfigRecord, NodeConfigError>> rollback(AsynGyanis::Core::EventLoop &loop, System::TenantScope tenant, std::int64_t nodeId,
                                                                                          std::int64_t revisionId, std::string note, std::int64_t operatorId);

    private:
        /**
         * @brief 为一次版本推进创建 config.apply 命令
         * @param loop 恢复协程用的事件循环
         * @param node 目标节点
         * @param version 新版本号
         * @param operatorId 操作人
         * @note 没有命令仓储或通道时跳过属于设计内的可选装配；配置提交后下发失败只记日志，不向调用方抛出
         */
        AsynGyanis::Core::Task<> dispatchConfigApply(AsynGyanis::Core::EventLoop &loop, const NodeRecord &node, std::int64_t version, std::int64_t operatorId);

        std::shared_ptr<NodeConfigRepository>       m_repository; ///< 配置仓储
        std::shared_ptr<NodeRepository>             m_nodes;      ///< 节点仓储
        std::shared_ptr<Command::CommandRepository> m_commands;   ///< 命令仓储，可空
        Command::AgentChannelRegistry              *m_channels;   ///< Agent 通道，可空
        CommandNotifier                             m_notifier;   ///< 命令通知，可空
    };
} // namespace EdgePulse::Device
