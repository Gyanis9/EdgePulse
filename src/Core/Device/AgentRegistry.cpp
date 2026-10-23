#include "Core/Device/AgentRegistry.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Core/Device/CertificateBinding.h"

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

        AgentUpsert upsert;
        upsert.tenantId        = registration.tenantId;
        upsert.name            = registration.name;
        upsert.agentVersion    = registration.agentVersion;
        upsert.certFingerprint = *fingerprint;
        upsert.labelsJson      = *labels;
        const NodeRecord node  = co_await m_repository->upsertAgent(loop, std::move(upsert));
        co_await publish(node);
        co_return node;
    }

    AsynGyanis::Core::Task<std::expected<NodeRecord, AgentError>> AgentRegistry::heartbeat(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId, std::string agentVersion)
    {
        if (nodeId <= 0)
        {
            co_return std::unexpected(AgentError{AgentError::Reason::InvalidRequest, "nodeId 必须是正整数"});
        }
        // Agent 侧没有会话，节点标识全局唯一；按全租户视图查节点，避免只认默认租户
        const std::optional<NodeRecord> node = co_await m_repository->scoped(System::TenantScope{0, true})->heartbeat(loop, nodeId, std::move(agentVersion));
        if (!node.has_value())
        {
            co_return std::unexpected(AgentError{AgentError::Reason::NotFound, "节点不存在或已删除"});
        }
        co_await publish(*node);
        co_return *node;
    }

    AsynGyanis::Core::Task<> AgentRegistry::publish(const NodeRecord &node)
    {
        if (!m_options.publisher)
        {
            co_return;
        }
        try
        {
            co_await m_options.publisher(node);
        } catch (const std::exception &failure)
        {
            // 状态已经落库；推送失败只影响实时界面，不能在注册/心跳路径上放大成 500
            LOG_ERROR_FMT("节点状态发布失败：{}", failure.what());
        }
        co_return;
    }
} // namespace EdgePulse::Device
