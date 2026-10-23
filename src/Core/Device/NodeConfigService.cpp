#include "Core/Device/NodeConfigService.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Core/Command/AgentChannelRegistry.h"
#include "Core/Crypto/Digest.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Device
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        constexpr std::size_t kMaximumConfigBytes        = 64U * 1024U; ///< 规范化后配置正文上限
        constexpr std::size_t kMaximumNoteLength         = 255;         ///< 修订说明列宽
        constexpr int         kConfigApplyTimeoutSeconds = 60;          ///< config.apply 的超时秒数

        /**
         * @brief 把外部配置正文规范成紧凑 JSON 对象
         * @param content 原始正文
         * @return 成功返回规范化文本；失败返回中文原因
         */
        [[nodiscard]] std::expected<std::string, std::string> normalizeConfig(const std::string &content)
        {
            if (content.empty())
            {
                return std::unexpected("配置不能为空");
            }
            const std::optional<ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(content);
            if (!parsed.has_value())
            {
                return std::unexpected("配置必须是合法 JSON");
            }
            if (!parsed->is_object())
            {
                return std::unexpected("配置必须是 JSON 对象");
            }
            const std::optional<std::string> serialized = AsynGyanis::Base::serializeConfigValue(*parsed);
            if (!serialized.has_value())
            {
                return std::unexpected("配置包含无法序列化的内容");
            }
            if (serialized->size() > kMaximumConfigBytes)
            {
                return std::unexpected("配置规范化后不能超过 64 KiB");
            }
            return *serialized;
        }

        [[nodiscard]] bool validReportStatus(const std::string_view status)
        {
            return status == "applied" || status == "failed" || status == "pending";
        }

        [[nodiscard]] NodeConfigError repositoryError(const NodeConfigFailure failure)
        {
            switch (failure)
            {
                case NodeConfigFailure::VersionConflict:
                    return NodeConfigError{failure, "配置已被他人修改，请刷新后重试"};
                case NodeConfigFailure::NotFound:
                    return NodeConfigError{failure, "节点或配置不存在"};
                case NodeConfigFailure::Invalid:
                    break;
            }
            return NodeConfigError{failure, "配置内容不合法"};
        }
    } // namespace

    NodeConfigService::NodeConfigService(std::shared_ptr<NodeConfigRepository> configs, std::shared_ptr<NodeRepository> nodes, std::shared_ptr<Command::CommandRepository> commands,
                                         Command::AgentChannelRegistry *channels, CommandNotifier notifier) :
        m_repository(std::move(configs)), m_nodes(std::move(nodes)), m_commands(std::move(commands)), m_channels(channels), m_notifier(std::move(notifier))
    {
    }

    AsynGyanis::Core::Task<std::expected<NodeConfigRecord, NodeConfigError>> NodeConfigService::applyDesired(AsynGyanis::Core::EventLoop &loop, const System::TenantScope tenant,
                                                                                                             const std::int64_t nodeId, std::string content,
                                                                                                             const std::int64_t expectedVersion, std::string note,
                                                                                                             const std::int64_t operatorId)
    {
        const auto canonical = normalizeConfig(content);
        if (!canonical.has_value())
        {
            co_return std::unexpected(NodeConfigError{NodeConfigFailure::Invalid, canonical.error()});
        }
        if (note.size() > kMaximumNoteLength)
        {
            co_return std::unexpected(NodeConfigError{NodeConfigFailure::Invalid, "修订说明不能超过 255 个字节"});
        }
        const std::optional<NodeRecord> node = co_await m_nodes->scoped(tenant)->findById(loop, nodeId);
        if (!node.has_value())
        {
            co_return std::unexpected(NodeConfigError{NodeConfigFailure::NotFound, "节点不存在或已删除"});
        }
        const std::string hash    = AsynGyanis::Core::Digest::sha256Hex(*canonical);
        const auto        applied = co_await m_repository->scoped(tenant)->applyDesired(loop, nodeId, *canonical, hash, expectedVersion, std::move(note), operatorId);
        if (!applied.has_value())
        {
            co_return std::unexpected(repositoryError(applied.error()));
        }
        co_await dispatchConfigApply(loop, *node, applied->desiredVersion, operatorId);
        LOG_INFO_FMT("配置下发：node={} version={} operator={}", nodeId, applied->desiredVersion, operatorId);
        co_return *applied;
    }

    AsynGyanis::Core::Task<std::expected<NodeConfigRecord, NodeConfigError>> NodeConfigService::report(AsynGyanis::Core::EventLoop &loop, const System::TenantScope tenant,
                                                                                                       const std::int64_t nodeId, const std::int64_t version, std::string content,
                                                                                                       std::string status, std::string message)
    {
        if (!validReportStatus(status))
        {
            co_return std::unexpected(NodeConfigError{NodeConfigFailure::Invalid, "上报状态只允许 applied、failed 或 pending"});
        }
        if (version < 0)
        {
            co_return std::unexpected(NodeConfigError{NodeConfigFailure::Invalid, "配置版本不能为负数"});
        }
        if (message.size() > kMaximumNoteLength)
        {
            co_return std::unexpected(NodeConfigError{NodeConfigFailure::Invalid, "上报说明不能超过 255 个字节"});
        }
        std::string canonical;
        std::string hash;
        if (!content.empty())
        {
            const auto normalized = normalizeConfig(content);
            if (!normalized.has_value())
            {
                co_return std::unexpected(NodeConfigError{NodeConfigFailure::Invalid, normalized.error()});
            }
            canonical = *normalized;
            // 摘要只信服务端按规范化内容重算的结果；请求体里的任何自报摘要都不参与
            hash = AsynGyanis::Core::Digest::sha256Hex(canonical);
        }
        const std::optional<NodeRecord> node = co_await m_nodes->scoped(tenant)->findById(loop, nodeId);
        if (!node.has_value())
        {
            co_return std::unexpected(NodeConfigError{NodeConfigFailure::NotFound, "节点不存在或已删除"});
        }
        const auto reported = co_await m_repository->scoped(tenant)->report(loop, nodeId, version, std::move(canonical), std::move(hash), status, message);
        if (!reported.has_value())
        {
            co_return std::unexpected(repositoryError(reported.error()));
        }
        if (message.empty())
        {
            LOG_INFO_FMT("配置上报：node={} version={} status={}", nodeId, version, status);
        } else
        {
            LOG_INFO_FMT("配置上报：node={} version={} status={} message={}", nodeId, version, status, message);
        }
        co_return *reported;
    }

    AsynGyanis::Core::Task<std::expected<NodeConfigRecord, NodeConfigError>> NodeConfigService::rollback(AsynGyanis::Core::EventLoop &loop, const System::TenantScope tenant,
                                                                                                         const std::int64_t nodeId, const std::int64_t revisionId, std::string note,
                                                                                                         const std::int64_t operatorId)
    {
        const std::optional<NodeRecord> node = co_await m_nodes->scoped(tenant)->findById(loop, nodeId);
        if (!node.has_value())
        {
            co_return std::unexpected(NodeConfigError{NodeConfigFailure::NotFound, "节点不存在或已删除"});
        }
        const std::shared_ptr<NodeConfigRepository> scopedRepository = m_repository->scoped(tenant);
        const std::optional<NodeConfigRevision>     revision         = co_await scopedRepository->findRevision(loop, nodeId, revisionId);
        if (!revision.has_value())
        {
            co_return std::unexpected(NodeConfigError{NodeConfigFailure::NotFound, "配置修订不存在或已过保留期"});
        }
        const auto canonical = normalizeConfig(revision->contentJson);
        if (!canonical.has_value())
        {
            // 修订只能由本服务写入；读回不合法说明数据被外部改写，拒绝悄悄回滚成别的形状
            co_return std::unexpected(NodeConfigError{NodeConfigFailure::Invalid, "配置修订内容不合法：" + canonical.error()});
        }
        if (note.empty())
        {
            note = "回滚到版本 " + std::to_string(revision->version);
        }
        if (note.size() > kMaximumNoteLength)
        {
            co_return std::unexpected(NodeConfigError{NodeConfigFailure::Invalid, "修订说明不能超过 255 个字节"});
        }
        const std::int64_t currentVersion = co_await scopedRepository->desiredVersion(loop, nodeId);
        const std::string  hash           = AsynGyanis::Core::Digest::sha256Hex(*canonical);
        const auto         applied        = co_await scopedRepository->applyDesired(loop, nodeId, *canonical, hash, currentVersion, std::move(note), operatorId);
        if (!applied.has_value())
        {
            co_return std::unexpected(repositoryError(applied.error()));
        }
        co_await dispatchConfigApply(loop, *node, applied->desiredVersion, operatorId);
        LOG_INFO_FMT("配置回滚：node={} revision={} version={} operator={}", nodeId, revisionId, applied->desiredVersion, operatorId);
        co_return *applied;
    }

    AsynGyanis::Core::Task<> NodeConfigService::dispatchConfigApply(AsynGyanis::Core::EventLoop &loop, const NodeRecord &node, const std::int64_t version,
                                                                    const std::int64_t operatorId)
    {
        if (m_commands == nullptr)
        {
            co_return;
        }
        try
        {
            const auto                            scopedCommands = m_commands->scoped(System::TenantScope{node.tenantId, false});
            const std::string                     payload        = ConfigValue::object({{"version", version}}).dump();
            std::optional<Command::CommandRecord> created        = co_await scopedCommands->create(loop, node.id, "config.apply", payload, operatorId, kConfigApplyTimeoutSeconds);
            if (!created.has_value())
            {
                // 节点在配置落库与命令创建之间被删除：期望版本仍保留，Agent 重连后按心跳版本自行拉取
                co_return;
            }
            if (m_channels != nullptr && m_channels->isConnected(node.id))
            {
                const ConfigValue commandValue = ConfigValue::object(
                        {{"id", created->id}, {"nodeId", created->nodeId}, {"type", created->type}, {"payload", created->payload}, {"timeoutSeconds", created->timeoutSeconds}});
                const std::string message = ConfigValue::object({{"type", "command"}, {"command", commandValue}}).dump();
                if (co_await m_channels->send(node.id, message))
                {
                    if (co_await scopedCommands->markDispatched(loop, created->id))
                    {
                        const auto refreshed = co_await scopedCommands->find(loop, created->id);
                        if (refreshed.has_value())
                        {
                            created = refreshed;
                        }
                    }
                }
            }
            if (m_notifier)
            {
                static_cast<void>(co_await m_notifier(*created));
            }
        } catch (const std::exception &failure)
        {
            // 配置已经提交；下发只是加速通道，失败不能再让接口报 500，Agent 会按心跳里的版本自行拉取
            LOG_ERROR_FMT("配置命令下发失败：node={} version={} reason={}", node.id, version, failure.what());
        } catch (...)
        {
            LOG_ERROR_FMT("配置命令下发失败：node={} version={} reason=未知异常", node.id, version);
        }
        co_return;
    }
} // namespace EdgePulse::Device
