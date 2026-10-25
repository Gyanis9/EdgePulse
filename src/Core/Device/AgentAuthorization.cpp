#include "Core/Device/AgentAuthorization.h"

#include "Base/Runtime/TimeFormat.h"
#include "Core/Device/CertificateBinding.h"
#include "Core/Metrics/BusinessMetrics.h"
#include "Core/System/TenantScope.h"
#include "Net/Http/Message/HttpRequest.h"

#include <Core/Tls/TlsPeerIdentity.h>

#include <optional>
#include <string>

namespace EdgePulse::Device
{
    AsynGyanis::Core::Task<std::expected<NodeRecord, AgentIdentityError>> requireAgentNode(AsynGyanis::Core::EventLoop &loop, NodeRepository &nodes,
                                                                                           const AsynGyanis::Net::HttpRequest &request, const std::int64_t nodeId)
    {
        // 非正标识不可能是合法节点：按客户端错误拒绝，避免把 -1 之类的输入当成仓储里的“查不到”
        if (nodeId <= 0)
        {
            co_return std::unexpected(AgentIdentityError{AgentIdentityError::Reason::InvalidRequest, 400, "nodeId 必须是正整数，请检查请求参数"});
        }

        // Agent 没有会话，节点标识全局唯一：按全租户视图查找，归属由节点自身决定
        const std::optional<NodeRecord> node = co_await nodes.scoped(System::TenantScope{0, true})->findById(loop, nodeId);
        if (!node.has_value())
        {
            co_return std::unexpected(AgentIdentityError{AgentIdentityError::Reason::NotFound, 404, "节点不存在或已删除，请先完成注册"});
        }

        // 判据顺序按规范：身份 → 吊销 → 指纹，任何一项缺失都 fail-closed
        const AsynGyanis::Core::TlsPeerIdentityPtr &identity = request.tlsPeerIdentity();
        if (!identity || !identity->isVerified())
        {
            Metrics::BusinessMetrics::instance().incrementIdentityRejections();
            co_return std::unexpected(AgentIdentityError{AgentIdentityError::Reason::Unauthenticated, 403, "Agent 通道必须出示通过校验的客户端证书"});
        }

        // 吊销先于指纹：无论 active 还是 previous 命中，吊销后都不能再代表节点
        if (!node->certRevokedAt.empty())
        {
            Metrics::BusinessMetrics::instance().incrementIdentityRejections();
            co_return std::unexpected(AgentIdentityError{AgentIdentityError::Reason::Forbidden, 403, "节点证书已吊销，请重新申请注册令牌并完成注册"});
        }

        // 复用注册路径的指纹归一化，并接受重叠窗口内的上一枚证书；空登记值与非法格式同样拒绝
        if (!certificateMatchesNode(identity, *node, Common::nowDateTimeUtc()))
        {
            Metrics::BusinessMetrics::instance().incrementIdentityRejections();
            co_return std::unexpected(AgentIdentityError{AgentIdentityError::Reason::Forbidden, 403, "连接证书与节点登记指纹不一致，请使用管理员签发的证书或改绑令牌"});
        }

        co_return *node;
    }
} // namespace EdgePulse::Device
