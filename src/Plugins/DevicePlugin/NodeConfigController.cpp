#include "Plugins/DevicePlugin/NodeConfigController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/Pagination.h"
#include "Base/Api/QueryParameters.h"
#include "Base/Api/RequestParsing.h"
#include "Base/Json/JsonFields.h"
#include "Core/Auth/DataScopeService.h"
#include "Core/Device/AgentAuthorization.h"
#include "Core/Device/NodeConfigRepository.h"
#include "Core/Device/NodeConfigService.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Device
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        constexpr std::string_view kListPermission  = "device:node:list";
        constexpr std::string_view kWritePermission = "device:node:write";

        /**
         * @brief 把存储的配置正文还原成响应里的 content
         * @param text 规范化 JSON 文本
         * @return 解析成功且为对象时返回对象；否则返回空对象
         */
        [[nodiscard]] ConfigValue contentOf(const std::string &text)
        {
            const std::optional<ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(text);
            if (parsed.has_value() && parsed->is_object())
            {
                return *parsed;
            }
            return ConfigValue::object();
        }

        /**
         * @brief 读请求体里的 content 字段
         * @param body 请求体
         * @param content 输出规范化前的正文
         * @return 字段存在且为对象或字符串时 true
         */
        [[nodiscard]] bool readContent(const ConfigValue &body, std::string &content)
        {
            if (!body.is_object())
            {
                return false;
            }
            const auto iterator = body.find("content");
            if (iterator == body.end())
            {
                return false;
            }
            if (iterator->is_object())
            {
                content = iterator->dump();
                return true;
            }
            if (iterator->is_string())
            {
                content = iterator->get<std::string>();
                return true;
            }
            return false;
        }

        [[nodiscard]] ConfigValue configJson(const NodeConfigRecord &record)
        {
            return ConfigValue::object({{"desired", ConfigValue::object({{"version", record.desiredVersion},
                                                                         {"content", contentOf(record.desiredJson)},
                                                                         {"hash", record.desiredHash},
                                                                         {"updatedAt", record.desiredUpdatedAt},
                                                                         {"updatedBy", record.desiredUpdatedBy}})},
                                        {"reported", ConfigValue::object({{"version", record.reportedVersion},
                                                                          {"content", contentOf(record.reportedJson)},
                                                                          {"hash", record.reportedHash},
                                                                          {"status", record.reportedStatus},
                                                                          {"at", record.reportedAt}})},
                                        {"inSync", record.inSync}});
        }

        /**
         * @brief 尚未下发过配置时的响应形状
         * @return 期望版本 0、内容空对象，并视为没有漂移
         */
        [[nodiscard]] ConfigValue emptyConfigJson()
        {
            NodeConfigRecord record;
            record.inSync = true;
            return configJson(record);
        }

        [[nodiscard]] ConfigValue revisionJson(const NodeConfigRevision &revision)
        {
            return ConfigValue::object({{"id", revision.id},
                                        {"nodeId", revision.nodeId},
                                        {"version", revision.version},
                                        {"content", contentOf(revision.contentJson)},
                                        {"hash", revision.contentHash},
                                        {"note", revision.note},
                                        {"createdBy", revision.createdBy},
                                        {"createdAt", revision.createdAt}});
        }

        void writeConfigError(AsynGyanis::Net::HttpResponse &response, const NodeConfigError &error, const std::string_view traceId)
        {
            switch (error.reason)
            {
                case NodeConfigFailure::Invalid:
                    Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, error.message, traceId);
                    break;
                case NodeConfigFailure::NotFound:
                    Common::writeFailure(response, Common::kCodeNotFound, Common::kCodeNotFound, error.message, traceId);
                    break;
                case NodeConfigFailure::VersionConflict:
                    Common::writeFailure(response, Common::kCodeConflict, Common::kCodeConflict, error.message, traceId);
                    break;
            }
        }

        /**
         * @brief 解析调用方的数据范围
         */
        [[nodiscard]] AsynGyanis::Core::Task<Auth::DataScope> resolveScope(Auth::DataScopeService *dataScopes, AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
        {
            if (dataScopes == nullptr)
            {
                co_return Auth::DataScope{true, false, userId, {}};
            }
            co_return co_await dataScopes->resolve(loop, userId);
        }

        [[nodiscard]] bool nodeInScope(const NodeRecord &node, const Auth::DataScope &scope)
        {
            if (scope.all)
            {
                return true;
            }
            if (scope.selfOnly)
            {
                return node.createdBy == scope.userId;
            }
            return std::find(scope.deptIds.begin(), scope.deptIds.end(), node.ownerDeptId) != scope.deptIds.end();
        }

        /**
         * @brief 校验节点标识、租户边界与数据范围
         * @return 通过时返回节点；失败时已经写好响应
         */
        [[nodiscard]] AsynGyanis::Core::Task<std::optional<NodeRecord>> requireNodeAccess(AsynGyanis::Core::EventLoop &loop, const NodeConfigDependencies &dependencies,
                                                                                          const Auth::SessionRecord &session, AsynGyanis::Net::HttpRequest &request,
                                                                                          AsynGyanis::Net::HttpResponse &response, const std::string_view traceId)
        {
            const std::optional<std::int64_t> nodeId = Common::parsePositiveIdentifier(request.param("id"));
            if (!nodeId.has_value())
            {
                Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "节点标识不合法", traceId);
                co_return std::nullopt;
            }
            const std::optional<NodeRecord> node = co_await dependencies.nodes->scoped(session.tenantScope())->findById(loop, *nodeId);
            if (!node.has_value())
            {
                Common::writeFailure(response, Common::kCodeNotFound, Common::kCodeNotFound, "节点不存在或已删除", traceId);
                co_return std::nullopt;
            }
            const Auth::DataScope scope = co_await resolveScope(dependencies.dataScopes, loop, session.userId);
            if (!nodeInScope(*node, scope))
            {
                Common::writeFailure(response, Common::kCodeNotFound, Common::kCodeNotFound, "节点不存在或已删除", traceId);
                co_return std::nullopt;
            }
            co_return node;
        }
    } // namespace

    void registerNodeConfigRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const NodeConfigDependencies &dependencyInput)
    {
        const auto dependencies = std::make_shared<NodeConfigDependencies>(dependencyInput);

        router.get("/api/nodes/:id/config",
                   [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*dependencies->auth, loop, request, response, kListPermission, traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const std::optional<NodeRecord> node = co_await requireNodeAccess(loop, *dependencies, *session, request, response, traceId);
                       if (!node.has_value())
                       {
                           co_return;
                       }
                       const std::optional<NodeConfigRecord> record = co_await dependencies->configs->scoped({node->tenantId, false})->find(loop, node->id);
                       Common::writeSuccess(response, record.has_value() ? configJson(*record) : emptyConfigJson(), traceId);
                       co_return;
                   });

        router.put("/api/nodes/:id/config",
                   [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*dependencies->auth, loop, request, response, kWritePermission, traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const std::optional<NodeRecord> node = co_await requireNodeAccess(loop, *dependencies, *session, request, response, traceId);
                       if (!node.has_value())
                       {
                           co_return;
                       }
                       const std::optional<ConfigValue> body = request.jsonBody();
                       if (!body.has_value() || !body->is_object())
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                           co_return;
                       }
                       const std::optional<std::int64_t> expectedVersion = Json::optionalInteger(*body, "expectedVersion");
                       if (!expectedVersion.has_value() || *expectedVersion < 0)
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少非负整数 expectedVersion", traceId);
                           co_return;
                       }
                       std::string content;
                       if (!readContent(*body, content))
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "content 必须是 JSON 对象或 JSON 字符串", traceId);
                           co_return;
                       }
                       const auto applied = co_await dependencies->service->applyDesired(loop, session->tenantScope(), node->id, std::move(content), *expectedVersion,
                                                                                         Json::stringField(*body, "note"), session->userId);
                       if (!applied.has_value())
                       {
                           writeConfigError(response, applied.error(), traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, configJson(*applied), traceId);
                       co_return;
                   });

        router.get("/api/nodes/:id/config/revisions",
                   [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*dependencies->auth, loop, request, response, kListPermission, traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const std::optional<NodeRecord> node = co_await requireNodeAccess(loop, *dependencies, *session, request, response, traceId);
                       if (!node.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap       query      = request.queryParams();
                       const Common::PageParameters parameters = Common::parsePageParameters(query);
                       const auto                   revisions  = co_await dependencies->configs->scoped({node->tenantId, false})->listRevisions(loop, node->id, parameters);
                       ConfigValue                  rows       = ConfigValue::array();
                       for (const NodeConfigRevision &revision: revisions.rows)
                       {
                           rows.push_back(revisionJson(revision));
                       }
                       Common::writeSuccess(response, Common::makePageResult(parameters, revisions.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.post("/api/nodes/:id/config/rollback",
                    [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(*dependencies->auth, loop, request, response, kWritePermission, traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const std::optional<NodeRecord> node = co_await requireNodeAccess(loop, *dependencies, *session, request, response, traceId);
                        if (!node.has_value())
                        {
                            co_return;
                        }
                        const std::optional<ConfigValue> body = request.jsonBody();
                        if (!body.has_value() || !body->is_object())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                            co_return;
                        }
                        const std::optional<std::int64_t> revisionId = Json::optionalInteger(*body, "revisionId");
                        if (!revisionId.has_value() || *revisionId <= 0)
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少正整数 revisionId", traceId);
                            co_return;
                        }
                        const auto rolled =
                                co_await dependencies->service->rollback(loop, session->tenantScope(), node->id, *revisionId, Json::stringField(*body, "note"), session->userId);
                        if (!rolled.has_value())
                        {
                            writeConfigError(response, rolled.error(), traceId);
                            co_return;
                        }
                        Common::writeSuccess(response, configJson(*rolled), traceId);
                        co_return;
                    });
    }

    void registerNodeConfigAgentRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const NodeConfigDependencies &dependencyInput)
    {
        const auto dependencies = std::make_shared<NodeConfigDependencies>(dependencyInput);

        router.get("/api/agent/nodes/:id/config",
                   [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string                 traceId = Web::resolveRequestId(request);
                       const std::optional<std::int64_t> nodeId  = Common::parsePositiveIdentifier(request.param("id"));
                       if (!nodeId.has_value())
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "节点标识不合法", traceId);
                           co_return;
                       }
                       const auto identity = co_await requireAgentNode(loop, *dependencies->nodes, request, *nodeId);
                       if (!identity.has_value())
                       {
                           Common::writeFailure(response, identity.error().httpStatus, identity.error().httpStatus, identity.error().message, traceId);
                           co_return;
                       }
                       const std::optional<NodeConfigRecord> record = co_await dependencies->configs->scoped({identity->tenantId, false})->find(loop, identity->id);
                       if (!record.has_value())
                       {
                           // 从未下发过配置：版本 0 与空对象让 Agent 保持现有行为
                           Common::writeSuccess(response, ConfigValue::object({{"version", 0}, {"content", ConfigValue::object()}, {"hash", ""}}), traceId);
                           co_return;
                       }
                       Common::writeSuccess(response,
                                            ConfigValue::object({{"version", record->desiredVersion}, {"content", contentOf(record->desiredJson)}, {"hash", record->desiredHash}}),
                                            traceId);
                       co_return;
                   });

        router.post("/api/agent/nodes/:id/config/report",
                    [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string                 traceId = Web::resolveRequestId(request);
                        const std::optional<std::int64_t> nodeId  = Common::parsePositiveIdentifier(request.param("id"));
                        if (!nodeId.has_value())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "节点标识不合法", traceId);
                            co_return;
                        }
                        const auto identity = co_await requireAgentNode(loop, *dependencies->nodes, request, *nodeId);
                        if (!identity.has_value())
                        {
                            Common::writeFailure(response, identity.error().httpStatus, identity.error().httpStatus, identity.error().message, traceId);
                            co_return;
                        }
                        const std::optional<ConfigValue> body = request.jsonBody();
                        if (!body.has_value() || !body->is_object())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                            co_return;
                        }
                        const std::optional<std::int64_t> version = Json::optionalInteger(*body, "version");
                        if (!version.has_value() || *version < 0)
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少非负整数 version", traceId);
                            co_return;
                        }
                        const std::string status = Json::stringField(*body, "status");
                        if (status != "applied" && status != "failed" && status != "pending")
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "status 只允许 applied、failed 或 pending", traceId);
                            co_return;
                        }
                        std::string content;
                        if (body->contains("content") && !(*body)["content"].is_null() && !readContent(*body, content))
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "content 必须是 JSON 对象或 JSON 字符串", traceId);
                            co_return;
                        }
                        const auto reported = co_await dependencies->service->report(loop, {identity->tenantId, false}, identity->id, *version, std::move(content), status,
                                                                                     Json::stringField(*body, "message"));
                        if (!reported.has_value())
                        {
                            writeConfigError(response, reported.error(), traceId);
                            co_return;
                        }
                        Common::writeSuccess(response, configJson(*reported), traceId);
                        co_return;
                    });
    }
} // namespace EdgePulse::Device
