/**
 * @file AgentRegistry.h
 * @brief mTLS Agent 注册、心跳与心跳钩子
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Device/NodeRepository.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>
#include <Core/Tls/TlsPeerIdentity.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace EdgePulse::Device
{
    class EnrollmentService;

    /**
     * @brief Agent 注册失败的可恢复原因
     */
    struct AgentError
    {
        enum class Reason
        {
            InvalidRequest,      ///< 字段不合法
            FingerprintRejected, ///< 指纹不在白名单或注册令牌被拒
            NotFound,            ///< 心跳的节点不存在
            Conflict,            ///< 并发注册在有限次重试后仍冲突，可稍后重试
        };

        Reason      reason{Reason::InvalidRequest}; ///< 机器可判的原因
        std::string message;                        ///< 中文可操作说明
    };

    /**
     * @brief Agent 注册输入
     */
    struct AgentRegistration
    {
        std::string  name;            ///< 节点名
        std::string  agentVersion;    ///< Agent 版本
        std::string  certFingerprint; ///< 自报证书 SHA-256
        std::string  labelsJson;      ///< 标签 JSON，可空
        std::int64_t tenantId{1};     ///< 节点归属租户，由注册请求的租户编码解析

        AsynGyanis::Core::TlsPeerIdentityPtr connectionIdentity{}; ///< 注册请求的 TLS 对端身份；非 TLS 链路为空
        std::string                          enrollmentToken;      ///< 一次性注册/改绑令牌明文，可空
    };

    /// 心跳后的节点状态回调；每次注册或心跳成功后都会调用，becameOnline 表示本次是否把节点拉回在线
    using NodeStatusPublisher = std::function<AsynGyanis::Core::Task<>(const NodeRecord &, bool becameOnline)>;

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
            std::vector<std::string> allowedFingerprints;   ///< 允许的证书指纹；空表示只依赖 CA 链校验
            NodeStatusPublisher      publisher;             ///< 心跳钩子，可空；在每次注册与心跳成功后调用，由回调决定是否发布状态
            std::string              enrollmentMode{"off"}; ///< off 或 required；required 下新节点与改绑必须持令牌
            EnrollmentService       *enrollments{nullptr};  ///< 令牌服务，用于计算明文摘要；required 模式下必须配置
        };

        /**
         * @brief 构造
         * @param repository 节点仓储
         * @param options 指纹白名单与心跳钩子
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
         * @param installedFirmwareVersion Agent 上报的已安装固件版本；空表示未上报，保留节点原值
         * @return 成功返回更新后的节点与在线转换标记；失败返回可恢复错误
         */
        AsynGyanis::Core::Task<std::expected<HeartbeatOutcome, AgentError>> heartbeat(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId, std::string agentVersion,
                                                                                      std::string installedFirmwareVersion = {});

    private:
        /**
         * @brief 每次注册或心跳成功后调用心跳钩子；回调异常只记日志，不推翻已经落库的状态
         */
        AsynGyanis::Core::Task<> notifyHeartbeat(const NodeRecord &node, bool becameOnline);

        std::shared_ptr<NodeRepository> m_repository; ///< 节点仓储
        Options                         m_options;    ///< 选项
    };
} // namespace EdgePulse::Device
