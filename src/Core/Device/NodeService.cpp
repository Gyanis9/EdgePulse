#include "Core/Device/NodeService.h"

#include "Base/Config/ConfigValue.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Device
{
    namespace
    {
        constexpr std::size_t kMaximumNameLength = 128;

        /**
         * @brief 校验名称
         * @return 合法时为空串，否则为中文原因
         */
        [[nodiscard]] std::string validateName(const std::string_view name)
        {
            if (name.empty())
            {
                return "节点名不能为空";
            }
            if (name.size() > kMaximumNameLength)
            {
                return "节点名不能超过 128 个字符";
            }
            return {};
        }

        /**
         * @brief 把标签文本归一成紧凑 JSON
         * @param labelsJson 原始 JSON 文本，空表示没有标签
         * @param error 失败原因
         * @return 成功返回归一化文本；失败为空
         */
        [[nodiscard]] std::optional<std::string> normalizeLabels(const std::string_view labelsJson, std::string &error)
        {
            if (labelsJson.empty())
            {
                return std::string{};
            }
            const std::optional<AsynGyanis::Base::ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(labelsJson);
            // 只收对象：数组或标量在列表页没有稳定的展示形状
            if (!parsed.has_value() || !parsed->is_object())
            {
                error = "labels 必须是 JSON 对象";
                return std::nullopt;
            }
            return parsed->dump();
        }
    } // namespace

    NodeService::NodeService(std::shared_ptr<NodeRepository> repository) : m_repository(std::move(repository))
    {
    }

    std::shared_ptr<NodeService> NodeService::scoped(const System::TenantScope tenant) const
    {
        return std::make_shared<NodeService>(m_repository->scoped(tenant));
    }

    AsynGyanis::Core::Task<NodePage> NodeService::list(AsynGyanis::Core::EventLoop &loop, Common::PageParameters parameters, std::string keyword, std::string status,
                                                       const std::int64_t groupId, NodeListScope scope)
    {
        co_return co_await m_repository->list(loop, parameters, std::move(keyword), std::move(status), groupId, std::move(scope));
    }

    AsynGyanis::Core::Task<std::expected<NodeRecord, NodeServiceError>> NodeService::create(AsynGyanis::Core::EventLoop &loop, std::string name, std::string labelsJson,
                                                                                            const std::int64_t groupId, const std::int64_t ownerDeptId,
                                                                                            const std::int64_t createdBy)
    {
        if (const std::string nameError = validateName(name); !nameError.empty())
        {
            co_return std::unexpected(NodeServiceError{NodeServiceError::Reason::Invalid, nameError});
        }
        std::string                      labelsError;
        const std::optional<std::string> normalizedLabels = normalizeLabels(labelsJson, labelsError);
        if (!normalizedLabels.has_value())
        {
            co_return std::unexpected(NodeServiceError{NodeServiceError::Reason::Invalid, labelsError});
        }
        // 名称唯一约束由数据库兜底，这里先查一次是为了给出 409 而不是 500
        if (const std::optional<NodeRecord> existing = co_await m_repository->findByName(loop, name); existing.has_value())
        {
            co_return std::unexpected(NodeServiceError{NodeServiceError::Reason::Conflict, "节点名已存在"});
        }
        co_return co_await m_repository->create(loop, std::move(name), *normalizedLabels, groupId, ownerDeptId, createdBy);
    }

    AsynGyanis::Core::Task<std::expected<NodeRecord, NodeServiceError>> NodeService::update(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId,
                                                                                            std::optional<std::string> name, std::optional<std::string> labelsJson,
                                                                                            std::optional<std::int64_t> groupId, const std::int64_t expectedVersion)
    {
        if (expectedVersion <= 0)
        {
            co_return std::unexpected(NodeServiceError{NodeServiceError::Reason::Invalid, "version 必须是正整数"});
        }
        if (!name.has_value() && !labelsJson.has_value() && !groupId.has_value())
        {
            co_return std::unexpected(NodeServiceError{NodeServiceError::Reason::Invalid, "name 与 labels 至少给一个"});
        }
        if (name.has_value())
        {
            if (const std::string nameError = validateName(*name); !nameError.empty())
            {
                co_return std::unexpected(NodeServiceError{NodeServiceError::Reason::Invalid, nameError});
            }
        }
        if (labelsJson.has_value())
        {
            std::string                labelsError;
            std::optional<std::string> normalized = normalizeLabels(*labelsJson, labelsError);
            if (!normalized.has_value())
            {
                co_return std::unexpected(NodeServiceError{NodeServiceError::Reason::Invalid, labelsError});
            }
            labelsJson = std::move(normalized);
        }

        const std::expected<NodeRecord, NodeMutationFailure> result = co_await m_repository->update(loop, nodeId, std::move(name), std::move(labelsJson), groupId, expectedVersion);
        if (!result.has_value())
        {
            if (result.error() == NodeMutationFailure::NotFound)
            {
                co_return std::unexpected(NodeServiceError{NodeServiceError::Reason::NotFound, "节点不存在或已删除"});
            }
            co_return std::unexpected(NodeServiceError{NodeServiceError::Reason::Conflict, "节点已被他人修改，请刷新后重试"});
        }
        co_return *result;
    }

    AsynGyanis::Core::Task<bool> NodeService::remove(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        co_return co_await m_repository->remove(loop, nodeId);
    }

    AsynGyanis::Core::Task<std::optional<NodeRecord>> NodeService::find(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        co_return co_await m_repository->findById(loop, nodeId);
    }
} // namespace EdgePulse::Device
