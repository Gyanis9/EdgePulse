/**
 * @file AgentRegistry.h
 * @brief mTLS Agent 注册、心跳与节点状态发布
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Device/NodeRepository.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace EdgePulse::Device
{
    /**
     * @brief Agent 注册失败的可恢复原因
     */
    struct AgentError
    {
        enum class Reason
        {
            InvalidRequest,      ///< 字段不合法
            FingerprintRejected, ///< 指纹不在白名单
            NotFound,            ///< 心跳的节点不存在
        };

        Reason      reason{Reason::InvalidRequest}; ///< 机器可判的原因
        std::string message;                        ///< 中文可操作说明
    };

    /**
     * @brief Agent 注册输入
     */
    struct AgentRegistration
    {
        std::string name;            ///< 节点名
        std::string agentVersion;    ///< Agent 版本
        std::string certFingerprint; ///< 自报证书 SHA-256
        std::string labelsJson;      ///< 标签 JSON，可空
    };

    /// 节点状态变化后的发布回调；在服务器循环线程上 co_await
    using NodeStatusPublisher = std::function<AsynGyanis::Core::Task<>(const NodeRecord &)>;

    /**
     * @brief Agent 注册表
     */
    class AgentRegistry
    {
    public:
        /**
         * @brief 选项
         */
        struct Options
        {
            std::vector<std::string> allowedFingerprints; ///< 允许的证书指纹；空表示只依赖 CA 链校验
            NodeStatusPublisher      publisher;           ///< 状态发布回调，可空
        };

        /**
         * @brief 构造
         * @param repository 节点仓储
         * @param options 指纹白名单与发布回调
         */
        AgentRegistry(std::shared_ptr<NodeRepository> repository, Options options);

        /**
         * @brief 注册或更新 Agent 对应节点
         * @param loop 恢复协程用的事件循环
         * @param registration 注册输入
         * @return 成功返回节点；失败返回可恢复错误
         */
        AsynGyanis::Core::Task<std::expected<NodeRecord, AgentError>> registerAgent(AsynGyanis::Core::EventLoop &loop, AgentRegistration registration);

        /**
         * @brief 处理一次心跳
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点标识
         * @param agentVersion Agent 版本
         * @return 成功返回更新后的节点；失败返回可恢复错误
         */
        AsynGyanis::Core::Task<std::expected<NodeRecord, AgentError>> heartbeat(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId, std::string agentVersion);

    private:
        /**
         * @brief 通知发布回调；回调异常只记日志，不推翻已经落库的状态
         */
        AsynGyanis::Core::Task<> publish(const NodeRecord &node);

        std::shared_ptr<NodeRepository> m_repository; ///< 节点仓储
        Options                         m_options;    ///< 选项
    };
} // namespace EdgePulse::Device
