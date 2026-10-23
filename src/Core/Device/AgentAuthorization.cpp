#include "Core/Device/AgentAuthorization.h"

#include "Core/System/TenantScope.h"
#include "Net/Http/Message/HttpRequest.h"

#include <Core/Tls/TlsPeerIdentity.h>

namespace EdgePulse::Device
{
    AsynGyanis::Core::Task<std::expected<NodeRecord, AgentIdentityError>> requireAgentNode(AsynGyanis::Core::EventLoop &loop, NodeRepository &nodes,
                                                                                           const AsynGyanis::Net::HttpRequest &request, const std::int64_t nodeId)
    {
        // Agent 没有会话，节点标识全局唯一：按全租户视图查找，归属由节点自身决定
        const std::optional<NodeRecord> node = co_await nodes.scoped(System::TenantScope{0, true})->findById(loop, nodeId);
        if (!node.has_value())
        {
            co_return std::unexpected(AgentIdentityError{AgentIdentityError::Reason::NotFound, 404, "节点不存在或已删除，请先完成注册"});
        }

        const AsynGyanis::Core::TlsPeerIdentityPtr &identity = request.tlsPeerIdentity();
        if (!identity || !identity->isVerified())
        {
            co_return std::unexpected(AgentIdentityError{AgentIdentityError::Reason::Unauthenticated, 403, "Agent 通道必须出示通过校验的客户端证书"});
        }

        // 空指纹表示节点尚未绑定；任何空值都不能被当成通配
        if (node->certFingerprint.empty() || node->certFingerprint != identity->sha256FingerprintHex())
        {
            co_return std::unexpected(AgentIdentityError{AgentIdentityError::Reason::Forbidden, 403, "连接证书与节点登记指纹不一致，请使用管理员签发的证书或改绑令牌"});
        }

        co_return *node;
    }
} // namespace EdgePulse::Device
