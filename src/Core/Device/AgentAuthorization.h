/**
 * @file AgentAuthorization.h
 * @brief 把 Agent 请求里的节点标识与 mTLS 叶证书身份绑定
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Device/NodeRepository.h"

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <expected>
#include <string>

namespace AsynGyanis::Core
{
    class EventLoop;
}
namespace AsynGyanis::Net
{
    class HttpRequest;
}

namespace EdgePulse::Device
{
    /**
     * @brief Agent 身份校验失败的原因
     */
    struct AgentIdentityError
    {
        enum class Reason
        {
            InvalidRequest,  ///< 节点标识不合法
            Unauthenticated, ///< 没有出示客户端证书或证书未验证
            Forbidden,       ///< 证书与登记指纹不一致
            NotFound,        ///< 节点不存在或已删除
        };

        Reason      reason{Reason::Forbidden}; ///< 机器可判的原因
        int         httpStatus{403};           ///< 建议的 HTTP 状态码
        std::string message;                   ///< 中文可操作说明
    };

    /**
     * @brief 校验请求身份并返回对应节点
     * @param loop 恢复协程用的事件循环
     * @param nodes 节点仓储
     * @param request 当前请求
     * @param nodeId 请求声明的节点标识
     * @return 成功返回节点；失败返回身份错误
     * @note Agent 路由只挂在 mTLS 监听器上；没有已验证的叶证书一律拒绝，空指纹也不会被当成通配
     */
    [[nodiscard]] AsynGyanis::Core::Task<std::expected<NodeRecord, AgentIdentityError>> requireAgentNode(AsynGyanis::Core::EventLoop &loop, NodeRepository &nodes,
                                                                                                         const AsynGyanis::Net::HttpRequest &request, std::int64_t nodeId);
} // namespace EdgePulse::Device
