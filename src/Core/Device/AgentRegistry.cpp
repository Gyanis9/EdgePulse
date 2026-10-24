#include "Core/Device/AgentRegistry.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Core/Device/CertificateBinding.h"
#include "Core/Device/EnrollmentService.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Device
{
    namespace
    {
        /**
         * @brief 校验并归一化标签
         */
        [[nodiscard]] std::optional<std::string> normalizeLabels(const std::string_view labelsJson)
        {
            if (labelsJson.empty())
            {
                return std::string{};
            }
            const std::optional<AsynGyanis::Base::ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(labelsJson);
            if (!parsed.has_value() || !parsed->is_object())
            {
                return std::nullopt;
            }
            return parsed->dump();
        }
    } // namespace

    AgentRegistry::AgentRegistry(std::shared_ptr<NodeRepository> repository, Options options) : m_repository(std::move(repository)), m_options(std::move(options))
    {
    }

    AsynGyanis::Core::Task<std::expected<NodeRecord, AgentError>> AgentRegistry::registerAgent(AsynGyanis::Core::EventLoop &loop, AgentRegistration registration)
    {
        if (registration.name.empty() || registration.name.size() > 128)
        {
            co_return std::unexpected(AgentError{AgentError::Reason::InvalidRequest, "节点名必须是 1 到 128 个字符"});
        }
        if (registration.agentVersion.size() > 64)
        {
            co_return std::unexpected(AgentError{AgentError::Reason::InvalidRequest, "Agent 版本不能超过 64 个字符"});
        }
        const std::optional<std::string> fingerprint = normalizeCertificateFingerprint(registration.certFingerprint);
        if (!fingerprint.has_value())
        {
            co_return std::unexpected(AgentError{AgentError::Reason::InvalidRequest, "certFingerprint 必须是 64 位十六进制"});
        }
        // required 模式必须拿到已验证的叶证书：certificateBindingFailure 对空身份返回放行，
        // 只适合 off 模式的测试/开发链路，不能让它绕过 required 的身份约束
        if (m_options.enrollmentMode == "required" && (!registration.connectionIdentity || !registration.connectionIdentity->isVerified()))
        {
            co_return std::unexpected(AgentError{AgentError::Reason::FingerprintRejected, "required 模式下注册必须出示通过 TLS 校验的客户端证书"});
        }
        // 自报值必须与这条 mTLS 连接出示的叶证书一致，否则任何持 CA 签发证书的对端都能冒名注册
        if (const std::optional<std::string> bindingFailure = certificateBindingFailure(registration.connectionIdentity, *fingerprint); bindingFailure.has_value())
        {
            co_return std::unexpected(AgentError{AgentError::Reason::FingerprintRejected, *bindingFailure});
        }
        // 白名单为空时只依赖 mTLS 的 CA 链校验；非空时在链校验之上再点名
        if (!m_options.allowedFingerprints.empty() &&
            std::find(m_options.allowedFingerprints.begin(), m_options.allowedFingerprints.end(), *fingerprint) == m_options.allowedFingerprints.end())
        {
            co_return std::unexpected(AgentError{AgentError::Reason::FingerprintRejected, "证书指纹不在允许名单内"});
        }
        const std::optional<std::string> labels = normalizeLabels(registration.labelsJson);
        if (!labels.has_value())
        {
            co_return std::unexpected(AgentError{AgentError::Reason::InvalidRequest, "labels 必须是 JSON 对象"});
        }
        const std::optional<std::string> notAfter = certificateNotAfter(registration.connectionIdentity);
        if (!notAfter.has_value())
        {
            co_return std::unexpected(AgentError{AgentError::Reason::InvalidRequest, "连接证书的到期时间无法解析，请检查证书格式"});
        }
        if (m_options.enrollmentMode == "required" && m_options.enrollments == nullptr)
        {
            // 配置要求令牌却没人能算摘要：属于装配错误，不能让注册悄悄退回只看指纹
            co_return std::unexpected(AgentError{AgentError::Reason::InvalidRequest, "注册令牌服务未配置，无法完成 required 模式注册"});
        }

        // 先看节点当前登记做一次友好的提前拒绝；真正的 enroll/rebind 判定在仓储事务里完成，
        // 这里读到的快照可能已经过期，不能作为放行依据
        if (m_options.enrollmentMode == "required" && registration.enrollmentToken.empty())
        {
            const std::optional<NodeRecord> current         = co_await m_repository->scoped({registration.tenantId, false})->findByName(loop, registration.name);
            const bool                      sameCertificate = current.has_value() && current->certRevokedAt.empty() && current->certFingerprint == *fingerprint;
            if (!sameCertificate)
            {
                const std::string message = current.has_value() ? "该节点已登记其他证书，请使用改绑令牌" : "该节点需要一次性注册令牌，请向管理员申请";
                co_return std::unexpected(AgentError{AgentError::Reason::FingerprintRejected, message});
            }
        }

        AgentUpsert upsert;
        upsert.tenantId            = registration.tenantId;
        upsert.name                = registration.name;
        upsert.agentVersion        = registration.agentVersion;
        upsert.certFingerprint     = *fingerprint;
        upsert.labelsJson          = *labels;
        upsert.enrollmentMode      = m_options.enrollmentMode;
        upsert.enrollmentTokenHash = registration.enrollmentToken.empty() ? std::string{} : EnrollmentService::hashToken(registration.enrollmentToken);
        upsert.certNotAfter        = *notAfter;
        const auto outcome         = co_await m_repository->upsertAgent(loop, std::move(upsert));
        if (!outcome.has_value())
        {
            // 仓储拒绝分三类：同名行（其他租户的活节点或已删除记录）按名字冲突返回 409 并给出改名/清理建议；
            // 令牌/改绑不通过按授权失败返回 403；整段事务仍撞并发返回 409 让 Agent 稍后重试
            if (outcome.error() == AgentUpsertFailure::NameConflict)
            {
                co_return std::unexpected(AgentError{AgentError::Reason::Conflict, "节点名已被其他租户或已删除节点占用，请改名或联系管理员清理"});
            }
            if (outcome.error() == AgentUpsertFailure::ConcurrentRegistration)
            {
                co_return std::unexpected(AgentError{AgentError::Reason::Conflict, "并发注册冲突，请稍后重试"});
            }
            co_return std::unexpected(AgentError{AgentError::Reason::FingerprintRejected, "注册令牌无效、过期、已用或与节点名不符"});
        }
        co_await notifyHeartbeat(outcome->node, outcome->becameOnline);
        co_return outcome->node;
    }

    AsynGyanis::Core::Task<std::expected<HeartbeatOutcome, AgentError>> AgentRegistry::heartbeat(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId,
                                                                                                 std::string agentVersion)
    {
        if (nodeId <= 0)
        {
            co_return std::unexpected(AgentError{AgentError::Reason::InvalidRequest, "nodeId 必须是正整数"});
        }
        // Agent 侧没有会话，节点标识全局唯一；按全租户视图查节点，避免只认默认租户
        const auto outcome = co_await m_repository->scoped(System::TenantScope{0, true})->heartbeat(loop, nodeId, std::move(agentVersion));
        if (!outcome.has_value())
        {
            co_return std::unexpected(AgentError{AgentError::Reason::NotFound, "节点不存在或已删除"});
        }
        co_await notifyHeartbeat(outcome->node, outcome->becameOnline);
        co_return *outcome;
    }

    AsynGyanis::Core::Task<> AgentRegistry::notifyHeartbeat(const NodeRecord &node, const bool becameOnline)
    {
        // 每次心跳都进钩子：是否广播由回调按 becameOnline 决定，生命周期还要靠这次调用销案离线告警
        if (!m_options.publisher)
        {
            co_return;
        }
        try
        {
            co_await m_options.publisher(node, becameOnline);
        } catch (const std::exception &failure)
        {
            // 状态已经落库；钩子失败只影响实时界面与告警销案，不能在注册/心跳路径上放大成 500
            LOG_ERROR_FMT("心跳钩子执行失败：{}", failure.what());
        }
        co_return;
    }
} // namespace EdgePulse::Device
