#include "Plugins/FirmwarePlugin/FirmwareController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/Pagination.h"
#include "Base/Api/QueryParameters.h"
#include "Base/Api/RequestParsing.h"
#include "Base/Json/JsonFields.h"
#include "Base/Log/LogMacros.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/DataScopeService.h"
#include "Core/Command/AgentCommandDelivery.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/AgentAuthorization.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Firmware/FirmwareFileWriter.h"
#include "Core/Firmware/FirmwareRepository.h"
#include "Core/Firmware/FirmwareRolloutRepository.h"
#include "Core/Firmware/FirmwareRolloutService.h"
#include "Core/Firmware/FirmwareSignatureVerifier.h"
#include "Core/Firmware/FirmwareSigningPolicy.h"
#include "Core/Http/RealtimeBus.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Core/System/TenantRepository.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpRequestBody.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "Platform/IO/MemoryMappedFile.h"

#include <Core/Coroutine/AsyncExecutor.h>
#include <Core/EventLoop/EventLoop.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <exception>
#include <functional>
#include <system_error>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::Firmware
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        [[nodiscard]] ConfigValue firmwareJson(const FirmwareRecord &record)
        {
            return ConfigValue::object({{"id", record.id},
                                        {"tenantId", record.tenantId},
                                        {"name", record.name},
                                        {"version", record.version},
                                        {"fileName", record.fileName},
                                        {"contentType", record.contentType},
                                        {"sizeBytes", record.sizeBytes},
                                        {"sha256", record.sha256},
                                        {"signed", !record.signature.empty()},
                                        {"signatureKeyId", record.signingKeyId},
                                        {"signatureAlgorithm", record.signatureAlgorithm},
                                        {"createdAt", record.createdAt},
                                        {"createdBy", record.createdBy}});
        }

        [[nodiscard]] ConfigValue taskJson(const FirmwareTaskRecord &task)
        {
            return ConfigValue::object({{"id", task.id},
                                        {"firmwareId", task.firmwareId},
                                        {"nodeId", task.nodeId},
                                        {"nodeName", task.nodeName},
                                        {"status", task.status},
                                        {"progress", task.progress},
                                        {"message", task.message},
                                        {"createdAt", task.createdAt},
                                        {"updatedAt", task.updatedAt}});
        }

        [[nodiscard]] ConfigValue parsedPolicy(const std::string &policyJson)
        {
            const std::optional<ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(policyJson);
            return parsed.has_value() ? *parsed : ConfigValue{policyJson};
        }

        [[nodiscard]] ConfigValue gateEntries(const std::string &gateJson)
        {
            ConfigValue entries = ConfigValue::array();
            std::size_t start   = 0;
            while (start < gateJson.size())
            {
                const std::size_t end  = gateJson.find('\n', start);
                const std::string line = gateJson.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!line.empty())
                {
                    const std::optional<ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(line);
                    entries.push_back(parsed.has_value() ? *parsed : ConfigValue{line});
                }
                if (end == std::string::npos)
                {
                    break;
                }
                start = end + 1;
            }
            return entries;
        }

        [[nodiscard]] ConfigValue parsedHealth(const std::string &healthJson)
        {
            const std::optional<ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(healthJson);
            return parsed.has_value() ? *parsed : ConfigValue::object();
        }

        [[nodiscard]] ConfigValue rolloutJson(const FirmwareRolloutRecord &rollout)
        {
            return ConfigValue::object({{"id", rollout.id},
                                        {"tenantId", rollout.tenantId},
                                        {"firmwareId", rollout.firmwareId},
                                        {"name", rollout.name},
                                        {"policy", parsedPolicy(rollout.policyJson)},
                                        {"status", rollout.status},
                                        {"currentWave", rollout.currentWave},
                                        {"totalWaves", rollout.totalWaves},
                                        {"createdBy", rollout.createdBy},
                                        {"createdAt", rollout.createdAt},
                                        {"startedAt", rollout.startedAt},
                                        {"finishedAt", rollout.finishedAt},
                                        {"pausedAt", rollout.pausedAt},
                                        {"pauseReason", rollout.pauseReason},
                                        {"lastError", rollout.lastError},
                                        {"version", rollout.version}});
        }

        [[nodiscard]] ConfigValue waveJson(const FirmwareRolloutWaveRecord &wave)
        {
            return ConfigValue::object({{"id", wave.id},
                                        {"rolloutId", wave.rolloutId},
                                        {"waveIndex", wave.waveIndex},
                                        {"status", wave.status},
                                        {"targetCount", wave.targetCount},
                                        {"dispatchedAt", wave.dispatchedAt},
                                        {"deadlineAt", wave.deadlineAt},
                                        {"settledAt", wave.settledAt},
                                        {"gate", gateEntries(wave.gateJson)},
                                        {"createdAt", wave.createdAt},
                                        {"updatedAt", wave.updatedAt}});
        }

        [[nodiscard]] ConfigValue targetJson(const FirmwareRolloutTargetRecord &target)
        {
            return ConfigValue::object({{"id", target.id},
                                        {"rolloutId", target.rolloutId},
                                        {"waveId", target.waveId},
                                        {"tenantId", target.tenantId},
                                        {"nodeId", target.nodeId},
                                        {"previousFirmwareId", target.previousFirmwareId},
                                        {"taskId", target.taskId},
                                        {"commandId", target.commandId},
                                        {"status", target.status},
                                        {"health", parsedHealth(target.healthJson)},
                                        {"updatedAt", target.updatedAt}});
        }

        [[nodiscard]] ConfigValue rolloutAggregateJson(const FirmwareRolloutAggregate &aggregate)
        {
            ConfigValue root  = rolloutJson(aggregate.rollout);
            ConfigValue waves = ConfigValue::array();
            for (const FirmwareRolloutWaveRecord &wave: aggregate.waves)
            {
                waves.push_back(waveJson(wave));
            }
            ConfigValue targets = ConfigValue::array();
            for (const FirmwareRolloutTargetRecord &target: aggregate.targets)
            {
                targets.push_back(targetJson(target));
            }
            root["waves"]   = std::move(waves);
            root["targets"] = std::move(targets);
            return root;
        }

        [[nodiscard]] int rolloutHttpStatus(const FirmwareRolloutErrorKind kind)
        {
            switch (kind)
            {
                case FirmwareRolloutErrorKind::NotFound:
                    return Common::kCodeNotFound;
                case FirmwareRolloutErrorKind::Conflict:
                    return Common::kCodeConflict;
                case FirmwareRolloutErrorKind::Internal:
                    return Common::kCodeInternalServerError;
                case FirmwareRolloutErrorKind::InvalidInput:
                default:
                    return Common::kCodeBadRequest;
            }
        }

        [[nodiscard]] std::string policyTextFrom(const ConfigValue &body)
        {
            if (body.contains("policy"))
            {
                return body.at("policy").is_object() ? body.at("policy").dump() : std::string{};
            }
            if (body.contains("policyJson") && body.at("policyJson").is_string())
            {
                return body.at("policyJson").get<std::string>();
            }
            return {};
        }

        void fail(AsynGyanis::Net::HttpResponse &response, const int status, const std::string &message, const std::string_view traceId)
        {
            Common::writeFailure(response, status, status, message, traceId);
        }

        using FirmwareRolloutAction =
                std::function<AsynGyanis::Core::Task<FirmwareRolloutResult<bool>>(AsynGyanis::Core::EventLoop &, System::TenantScope, std::int64_t, const std::string &)>;
        using FirmwareRolloutActionHandler = std::function<AsynGyanis::Core::Task<>(AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &)>;

        [[nodiscard]] AsynGyanis::Core::Task<Auth::DataScope> resolveDataScope(Auth::DataScopeService *dataScopes, AsynGyanis::Core::EventLoop &loop,
                                                                               const Auth::SessionRecord &session);
        [[nodiscard]] FirmwareRolloutTargetScope targetScopeFrom(const Auth::DataScope &scope);

        /**
         * @brief 生成人工动作的处理函数
         * @param loop 请求所属事件循环
         * @param dependencies 固件依赖
         * @param action 具体动作
         * @param requiresReason 是否要求正文给出 reason
         * @return FirmwareRolloutActionHandler 完成鉴权、动作与最新聚合回写的处理器
         */
        [[nodiscard]] FirmwareRolloutActionHandler makeRolloutActionHandler(AsynGyanis::Core::EventLoop &loop, const FirmwareDependencies &dependencies,
                                                                            FirmwareRolloutAction action, const bool requiresReason)
        {
            return [&loop, dependencies, action = std::move(action), requiresReason](AsynGyanis::Net::HttpRequest  &request,
                                                                                     AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
            {
                const std::string traceId = Web::resolveRequestId(request);
                const auto        session = co_await Web::requireSession(*dependencies.auth, loop, request, response, "device:firmware:upload", traceId);
                if (!session.has_value())
                {
                    co_return;
                }
                const auto id = Common::parsePositiveIdentifier(request.param("id"));
                if (!id.has_value())
                {
                    fail(response, Common::kCodeBadRequest, "灰度发布标识不合法", traceId);
                    co_return;
                }
                std::string reason;
                if (const auto body = request.jsonBody(); body.has_value() && body->is_object())
                {
                    reason = Json::stringField(*body, "reason");
                }
                if (requiresReason && reason.empty())
                {
                    fail(response, Common::kCodeBadRequest, "强制提升必须提供 reason", traceId);
                    co_return;
                }
                const Auth::DataScope scope = co_await resolveDataScope(dependencies.dataScopes, loop, *session);
                if (!scope.all)
                {
                    // 动作会影响整笔发布；只要有一个目标在数据范围外就拒绝，避免借动作越权触碰别家节点
                    const auto full = co_await dependencies.rollouts->find(loop, session->tenantScope(), *id);
                    if (full.has_value())
                    {
                        const auto visible = co_await dependencies.rollouts->find(loop, session->tenantScope(), *id, targetScopeFrom(scope));
                        if (!visible.has_value() || visible->targets.size() != full->targets.size())
                        {
                            fail(response, Common::kCodeForbidden, "灰度发布包含数据范围外的目标，不能操作", traceId);
                            co_return;
                        }
                    }
                }
                const auto changed = co_await action(loop, session->tenantScope(), *id, reason);
                if (!changed.has_value())
                {
                    fail(response, rolloutHttpStatus(changed.error().kind), changed.error().message, traceId);
                    co_return;
                }
                const auto aggregate = co_await dependencies.rollouts->find(loop, session->tenantScope(), *id, targetScopeFrom(scope));
                if (!aggregate.has_value())
                {
                    fail(response, Common::kCodeNotFound, "灰度发布不存在", traceId);
                    co_return;
                }
                Common::writeSuccess(response, rolloutAggregateJson(*aggregate), traceId);
                co_return;
            };
        }

        /**
         * @brief 在工作线程上删除被拒绝的固件成品
         * @param loop 请求所属事件循环，删除尝试结束后把协程恢复到这里
         * @param path 待删除的成品路径
         * @return AsynGyanis::Core::Task<> 删除尝试结束即返回，失败只记日志
         * @note 删除失败不改写响应状态码：请求本来就要失败，成品残留交给运维清理
         */
        AsynGyanis::Core::Task<> removeStoredFile(AsynGyanis::Core::EventLoop &loop, std::string path)
        {
            try
            {
                const bool removed = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<bool>(loop,
                                                                                                     [path]()
                                                                                                     {
                                                                                                         std::error_code error;
                                                                                                         std::filesystem::remove(path, error);
                                                                                                         return !error;
                                                                                                     });
                if (!removed)
                {
                    LOG_WARN_FMT("删除固件成品失败：path={}", path);
                }
            } catch (const std::exception &failure)
            {
                LOG_WARN_FMT("提交固件成品删除任务失败：path={} reason={}", path, failure.what());
            }
            co_return;
        }

        [[nodiscard]] bool signatureRequired(const FirmwareDependencies &dependencies) noexcept
        {
            return dependencies.signingPolicy != nullptr && dependencies.signingPolicy->signatureRequired();
        }

        /**
         * @brief 解析会话的数据范围；平台管理员与未配置数据范围时按全部可见处理
         */
        [[nodiscard]] AsynGyanis::Core::Task<Auth::DataScope> resolveDataScope(Auth::DataScopeService *dataScopes, AsynGyanis::Core::EventLoop &loop,
                                                                               const Auth::SessionRecord &session)
        {
            if (dataScopes == nullptr || session.platformAdmin)
            {
                co_return Auth::DataScope{true, false, session.userId, {}};
            }
            co_return co_await dataScopes->resolve(loop, session.userId);
        }

        [[nodiscard]] FirmwareRolloutTargetScope targetScopeFrom(const Auth::DataScope &scope)
        {
            FirmwareRolloutTargetScope targetScope;
            targetScope.all       = scope.all;
            targetScope.selfOnly  = scope.selfOnly;
            targetScope.userId    = scope.userId;
            targetScope.deptIds   = scope.deptIds;
            return targetScope;
        }

        [[nodiscard]] bool nodeVisibleInScope(const Device::NodeRecord &node, const Auth::DataScope &scope)
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

        [[nodiscard]] std::string joinedIdentifiers(const std::vector<std::int64_t> &identifiers)
        {
            std::string joined;
            for (const std::int64_t identifier: identifiers)
            {
                if (!joined.empty())
                {
                    joined += "、";
                }
                joined += std::to_string(identifier);
            }
            return joined;
        }

        /**
         * @brief 在会话租户边界内挑出不能作为下发/回滚目标的节点
         * @param loop 恢复协程用的事件循环
         * @param nodes 节点仓储
         * @param scope 会话租户边界
         * @param nodeIds 请求里的节点标识
         * @param firmwareTenantId 固件归属租户
         * @param dataScope 会话数据范围；调用方已解析时传入
         * @return 不存在、不可见、不在数据范围或与固件不同租户的节点标识；全部合法时为空
         * @note 平台管理员的边界是全租户，此时仍要求节点租户与固件租户一致
         */
        [[nodiscard]] AsynGyanis::Core::Task<std::vector<std::int64_t>> invalidFirmwareTargets(AsynGyanis::Core::EventLoop &loop, const Device::NodeRepository &nodes,
                                                                                               const System::TenantScope scope, const std::vector<std::int64_t> &nodeIds,
                                                                                               const std::int64_t firmwareTenantId, const Auth::DataScope &dataScope)
        {
            const auto                scopedNodes = nodes.scoped(scope);
            std::vector<std::int64_t> invalid;
            for (const std::int64_t nodeId: nodeIds)
            {
                const auto node = co_await scopedNodes->findById(loop, nodeId);
                if (!node.has_value() || node->tenantId != firmwareTenantId || !nodeVisibleInScope(*node, dataScope))
                {
                    invalid.push_back(nodeId);
                }
            }
            co_return invalid;
        }

        [[nodiscard]] std::string safeFileName(const std::string &fileName)
        {
            std::string name = std::filesystem::path(fileName).filename().string();
            if (name.empty())
            {
                name = "firmware.bin";
            }
            for (char &character: name)
            {
                const unsigned char code = static_cast<unsigned char>(character);
                if (!(std::isalnum(code) != 0 || character == '.' || character == '-' || character == '_'))
                {
                    character = '_';
                }
            }
            if (name.size() > 200)
            {
                name.resize(200);
            }
            return name;
        }

        struct ByteRange
        {
            std::int64_t start{0};  ///< 起始偏移
            std::int64_t length{0}; ///< 长度
        };

        [[nodiscard]] std::optional<ByteRange> parseRange(const std::string_view header, const std::int64_t size)
        {
            if (!header.starts_with("bytes="))
            {
                return std::nullopt;
            }
            const std::string_view value = header.substr(6);
            if (value.find(',') != std::string_view::npos)
            {
                return std::nullopt;
            }
            const std::size_t dash = value.find('-');
            if (dash == std::string_view::npos)
            {
                return std::nullopt;
            }
            const std::string_view startText = value.substr(0, dash);
            const std::string_view endText   = value.substr(dash + 1);
            std::int64_t           start     = 0;
            std::int64_t           end       = size - 1;
            if (startText.empty())
            {
                // bytes=-N：最后 N 字节
                std::int64_t suffix            = 0;
                const auto [endPointer, error] = std::from_chars(endText.data(), endText.data() + endText.size(), suffix);
                if (error != std::errc{} || endPointer != endText.data() + endText.size() || suffix <= 0)
                {
                    return std::nullopt;
                }
                start = std::max<std::int64_t>(0, size - suffix);
            } else
            {
                const auto [startPointer, startError] = std::from_chars(startText.data(), startText.data() + startText.size(), start);
                if (startError != std::errc{} || startPointer != startText.data() + startText.size() || start < 0 || start >= size)
                {
                    return std::nullopt;
                }
                if (!endText.empty())
                {
                    const auto [endPointer, endError] = std::from_chars(endText.data(), endText.data() + endText.size(), end);
                    if (endError != std::errc{} || endPointer != endText.data() + endText.size() || end < start)
                    {
                        return std::nullopt;
                    }
                    end = std::min(end, size - 1);
                }
            }
            return ByteRange{start, end - start + 1};
        }

        [[nodiscard]] std::string downloadPayload(const FirmwareRecord &record, const std::int64_t nodeId)
        {
            return ConfigValue::object({{"firmwareId", record.id},
                                        {"name", record.name},
                                        {"version", record.version},
                                        {"sha256", record.sha256},
                                        {"sizeBytes", record.sizeBytes},
                                        {"signature", record.signature},
                                        {"signingKeyId", record.signingKeyId},
                                        {"signatureAlgorithm", record.signatureAlgorithm},
                                        {"downloadPath", "/api/agent/firmware/" + std::to_string(record.id) + "/download?nodeId=" + std::to_string(nodeId)}})
                    .dump();
        }

        /**
         * @brief 注册固件灰度路由
         * @param router 公开路由器
         * @param loop 服务器事件循环
         * @param dependencies 固件依赖，rollouts 为空时整体跳过
         */
        void registerRolloutRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const FirmwareDependencies &dependencies)
        {
            if (dependencies.rollouts == nullptr)
            {
                return;
            }

            router.post("/api/firmware/:id/rollouts",
                        [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                        {
                            const std::string traceId = Web::resolveRequestId(request);
                            const auto        session = co_await Web::requireSession(*dependencies.auth, loop, request, response, "device:firmware:upload", traceId);
                            if (!session.has_value())
                            {
                                co_return;
                            }
                            const auto id   = Common::parsePositiveIdentifier(request.param("id"));
                            const auto body = request.jsonBody();
                            if (!id.has_value() || !body.has_value() || !body->is_object())
                            {
                                fail(response, Common::kCodeBadRequest, "固件标识或请求正文不合法", traceId);
                                co_return;
                            }
                            const std::string name       = Json::stringField(*body, "name");
                            const std::string policyText = policyTextFrom(*body);
                            const auto        nodeIds    = Json::integerArrayField(*body, "nodeIds");
                            if (name.empty())
                            {
                                fail(response, Common::kCodeBadRequest, "发布名称不能为空", traceId);
                                co_return;
                            }
                            if (policyText.empty())
                            {
                                fail(response, Common::kCodeBadRequest, "缺少 policy 或 policyJson", traceId);
                                co_return;
                            }
                            const auto firmware = co_await dependencies.firmware->scoped(session->tenantScope())->find(loop, *id);
                            if (!firmware.has_value())
                            {
                                fail(response, Common::kCodeNotFound, "固件不存在", traceId);
                                co_return;
                            }
                            const Auth::DataScope dataScope = co_await resolveDataScope(dependencies.dataScopes, loop, *session);
                            const auto           invalidTargets =
                                    co_await invalidFirmwareTargets(loop, *dependencies.nodes, session->tenantScope(), nodeIds, firmware->tenantId, dataScope);
                            if (!invalidTargets.empty())
                            {
                                fail(response, Common::kCodeBadRequest, "目标节点不存在、不可见、不在数据范围内或不属于固件所在租户：" + joinedIdentifiers(invalidTargets), traceId);
                                co_return;
                            }
                            const auto created = co_await dependencies.rollouts->create(loop, session->tenantScope(), session->userId, *id, name, policyText, nodeIds);
                            if (!created.has_value())
                            {
                                fail(response, rolloutHttpStatus(created.error().kind), created.error().message, traceId);
                                co_return;
                            }
                            const auto aggregate = co_await dependencies.rollouts->find(loop, session->tenantScope(), *created);
                            if (!aggregate.has_value())
                            {
                                fail(response, Common::kCodeInternalServerError, "灰度发布创建后读取失败", traceId);
                                co_return;
                            }
                            Common::writeSuccess(response, rolloutAggregateJson(*aggregate), traceId);
                            co_return;
                        });

            router.get("/api/firmware/:id/rollouts",
                       [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                       {
                           const std::string traceId = Web::resolveRequestId(request);
                           const auto        session = co_await Web::requireSession(*dependencies.auth, loop, request, response, "device:firmware:list", traceId);
                           if (!session.has_value())
                           {
                               co_return;
                           }
                           const auto id = Common::parsePositiveIdentifier(request.param("id"));
                           if (!id.has_value())
                           {
                               fail(response, Common::kCodeBadRequest, "固件标识不合法", traceId);
                               co_return;
                           }
                           const Common::QueryMap  query      = request.queryParams();
                           const Common::PageParameters page   = Common::parsePageParameters(query);
                           const Auth::DataScope dataScope    = co_await resolveDataScope(dependencies.dataScopes, loop, *session);
                           const auto                   result = co_await dependencies.rollouts->list(loop, session->tenantScope(), *id, page, targetScopeFrom(dataScope));
                           ConfigValue                  rows   = ConfigValue::array();
                           for (const FirmwareRolloutRecord &record: result.rows)
                           {
                               rows.push_back(rolloutJson(record));
                           }
                           Common::writeSuccess(response, Common::makePageResult(page, result.total, std::move(rows)), traceId);
                           co_return;
                       });

            router.get("/api/firmware/rollouts/:id",
                       [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                       {
                           const std::string traceId = Web::resolveRequestId(request);
                           const auto        session = co_await Web::requireSession(*dependencies.auth, loop, request, response, "device:firmware:list", traceId);
                           if (!session.has_value())
                           {
                               co_return;
                           }
                           const auto id = Common::parsePositiveIdentifier(request.param("id"));
                           if (!id.has_value())
                           {
                               fail(response, Common::kCodeBadRequest, "灰度发布标识不合法", traceId);
                               co_return;
                           }
                           const auto aggregate = co_await dependencies.rollouts->find(loop, session->tenantScope(), *id);
                           if (!aggregate.has_value())
                           {
                               fail(response, Common::kCodeNotFound, "灰度发布不存在", traceId);
                               co_return;
                           }
                           const Auth::DataScope dataScope = co_await resolveDataScope(dependencies.dataScopes, loop, *session);
                           if (!dataScope.all)
                           {
                               const auto visible = co_await dependencies.rollouts->find(loop, session->tenantScope(), *id, targetScopeFrom(dataScope));
                               if (!visible.has_value() || visible->targets.empty())
                               {
                                   // 目标全部在数据范围外时不暴露发布头
                                   fail(response, Common::kCodeNotFound, "灰度发布不存在", traceId);
                                   co_return;
                               }
                               Common::writeSuccess(response, rolloutAggregateJson(*visible), traceId);
                               co_return;
                           }
                           Common::writeSuccess(response, rolloutAggregateJson(*aggregate), traceId);
                           co_return;
                       });

            FirmwareRolloutService *service = dependencies.rollouts;
            router.post("/api/firmware/rollouts/:id/pause",
                        makeRolloutActionHandler(
                                loop, dependencies,
                                [service](AsynGyanis::Core::EventLoop &actionLoop, const System::TenantScope scope, const std::int64_t rolloutId,
                                          const std::string &reason) -> AsynGyanis::Core::Task<FirmwareRolloutResult<bool>>
                                { co_return co_await service->pause(actionLoop, scope, rolloutId, reason); },
                                false));
            router.post("/api/firmware/rollouts/:id/resume",
                        makeRolloutActionHandler(
                                loop, dependencies,
                                [service](AsynGyanis::Core::EventLoop &actionLoop, const System::TenantScope scope, const std::int64_t rolloutId,
                                          const std::string &) -> AsynGyanis::Core::Task<FirmwareRolloutResult<bool>>
                                { co_return co_await service->resume(actionLoop, scope, rolloutId); },
                                false));
            router.post("/api/firmware/rollouts/:id/promote",
                        makeRolloutActionHandler(
                                loop, dependencies,
                                [service](AsynGyanis::Core::EventLoop &actionLoop, const System::TenantScope scope, const std::int64_t rolloutId,
                                          const std::string &) -> AsynGyanis::Core::Task<FirmwareRolloutResult<bool>>
                                { co_return co_await service->promote(actionLoop, scope, rolloutId); },
                                false));
            router.post("/api/firmware/rollouts/:id/force-promote",
                        makeRolloutActionHandler(
                                loop, dependencies,
                                [service](AsynGyanis::Core::EventLoop &actionLoop, const System::TenantScope scope, const std::int64_t rolloutId,
                                          const std::string &reason) -> AsynGyanis::Core::Task<FirmwareRolloutResult<bool>>
                                { co_return co_await service->forcePromote(actionLoop, scope, rolloutId, reason); },
                                true));
            router.post("/api/firmware/rollouts/:id/abort",
                        makeRolloutActionHandler(
                                loop, dependencies,
                                [service](AsynGyanis::Core::EventLoop &actionLoop, const System::TenantScope scope, const std::int64_t rolloutId,
                                          const std::string &) -> AsynGyanis::Core::Task<FirmwareRolloutResult<bool>>
                                { co_return co_await service->abort(actionLoop, scope, rolloutId); },
                                false));
            router.post("/api/firmware/rollouts/:id/rollback",
                        makeRolloutActionHandler(
                                loop, dependencies,
                                [service](AsynGyanis::Core::EventLoop &actionLoop, const System::TenantScope scope, const std::int64_t rolloutId,
                                          const std::string &) -> AsynGyanis::Core::Task<FirmwareRolloutResult<bool>>
                                { co_return co_await service->rollback(actionLoop, scope, rolloutId); },
                                false));
        }
    } // namespace

    void registerFirmwareRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const FirmwareDependencies &dependencyInput)
    {
        const auto deps = std::make_shared<FirmwareDependencies>(dependencyInput);

        router.get("/api/firmware",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:firmware:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap       query  = request.queryParams();
                       const Common::PageParameters page   = Common::parsePageParameters(query);
                       const auto                   result = co_await deps->firmware->scoped(session->tenantScope())->list(loop, Common::queryText(query, "keyword"), page);
                       ConfigValue                  rows   = ConfigValue::array();
                       for (const FirmwareRecord &record: result.rows)
                       {
                           rows.push_back(firmwareJson(record));
                       }
                       Common::writeSuccess(response, Common::makePageResult(page, result.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.get("/api/firmware/policy",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:firmware:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       ConfigValue trustedKeyIds = ConfigValue::array();
                       if (deps->signingPolicy != nullptr && deps->signingPolicy->verifier != nullptr)
                       {
                           for (const std::string &keyId: deps->signingPolicy->verifier->keyIds())
                           {
                               trustedKeyIds.push_back(keyId);
                           }
                       }
                       Common::writeSuccess(response, ConfigValue::object({{"requireSignature", signatureRequired(*deps)}, {"trustedKeyIds", std::move(trustedKeyIds)}}), traceId);
                       co_return;
                   });

        router.get("/api/firmware/:id/tasks",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:firmware:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto id = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "固件标识不合法", traceId);
                           co_return;
                       }
                       const auto  tasks = co_await deps->firmware->scoped(session->tenantScope())->tasks(loop, *id);
                       ConfigValue rows  = ConfigValue::array();
                       for (const FirmwareTaskRecord &task: tasks)
                       {
                           rows.push_back(taskJson(task));
                       }
                       Common::writeSuccess(response, std::move(rows), traceId);
                       co_return;
                   });

        router.postStreaming("/api/firmware",
                             [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                             {
                                 const std::string traceId = Web::resolveRequestId(request);
                                 const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:firmware:upload", traceId);
                                 if (!session.has_value())
                                 {
                                     co_return;
                                 }
                                 const Common::QueryMap query    = request.queryParams();
                                 const std::string      name     = Common::queryText(query, "name");
                                 const std::string      version  = Common::queryText(query, "version");
                                 std::string            fileName = Common::queryText(query, "fileName");
                                 if (fileName.empty())
                                 {
                                     fileName = request.getHeader("x-file-name").value_or("firmware.bin");
                                 }
                                 if (name.empty() || version.empty())
                                 {
                                     fail(response, Common::kCodeBadRequest, "缺少 name 或 version", traceId);
                                     co_return;
                                 }
                                 // 签名头在收正文前读出并保留：缺 key id 或策略不允许时不必把固件写盘
                                 const std::optional<std::string> signatureHeader = request.getHeader("x-firmware-signature");
                                 const std::optional<std::string> keyIdHeader     = request.getHeader("x-firmware-key-id");
                                 // 强制签名时在收正文前就拒绝，避免把注定丢弃的固件写满磁盘
                                 if (signatureRequired(*deps) && (!signatureHeader.has_value() || !keyIdHeader.has_value()))
                                 {
                                     fail(response, Common::kCodeBadRequest, "当前部署要求固件签名：请提供 X-Firmware-Signature 与 X-Firmware-Key-Id", traceId);
                                     co_return;
                                 }
                                 if (signatureHeader.has_value() != keyIdHeader.has_value())
                                 {
                                     fail(response, Common::kCodeBadRequest,
                                          signatureHeader.has_value() ? "提供了固件签名但缺少 X-Firmware-Key-Id" : "提供了 X-Firmware-Key-Id 但缺少 X-Firmware-Signature", traceId);
                                     co_return;
                                 }
                                 if (signatureHeader.has_value() && (deps->signingPolicy == nullptr || deps->signingPolicy->verifier == nullptr))
                                 {
                                     fail(response, Common::kCodeBadRequest, "当前部署未配置固件信任键，无法校验固件签名", traceId);
                                     co_return;
                                 }
                                 std::int64_t targetTenantId = session->tenantId;
                                 if (session->platformAdmin)
                                 {
                                     const auto requestedTenant = Common::queryInteger(query, "tenantId");
                                     if (requestedTenant.has_value() && *requestedTenant > 0)
                                     {
                                         if (deps->tenants != nullptr)
                                         {
                                             const auto tenant = co_await deps->tenants->findById(loop, *requestedTenant);
                                             if (!tenant.has_value() || tenant->status != 1)
                                             {
                                                 fail(response, Common::kCodeBadRequest, "目标租户不存在或已停用", traceId);
                                                 co_return;
                                             }
                                         }
                                         targetTenantId = *requestedTenant;
                                     }
                                 }
                                 const auto                        firmware   = deps->firmware->scoped(System::TenantScope{targetTenantId, false});
                                 AsynGyanis::Net::HttpRequestBody *bodyStream = request.bodyStream();
                                 if (bodyStream == nullptr)
                                 {
                                     fail(response, Common::kCodeBadRequest, "当前请求没有可读正文", traceId);
                                     co_return;
                                 }
                                 const std::int64_t                  id        = Common::nextIdentifier();
                                 const std::string                   safeName  = safeFileName(fileName);
                                 const std::filesystem::path         directory = deps->storageDirectory;
                                 const std::string                   finalPath = (directory / (std::to_string(id) + "_" + safeName)).string();
                                 std::shared_ptr<FirmwareFileWriter> writer;
                                 try
                                 {
                                     writer = co_await FirmwareFileWriter::open(loop, finalPath + ".part", finalPath);
                                 } catch (const std::exception &failure)
                                 {
                                     fail(response, Common::kCodeInternalServerError, failure.what(), traceId);
                                     co_return;
                                 }
                                 std::size_t totalBytes = 0;
                                 while (co_await bodyStream->readNext())
                                 {
                                     const std::string_view chunk = bodyStream->chunk();
                                     totalBytes += chunk.size();
                                     if (totalBytes > deps->maximumUploadBytes)
                                     {
                                         writer->abort();
                                         fail(response, 413, "固件超过允许的上传大小", traceId);
                                         co_return;
                                     }
                                     if (!(co_await writer->append(loop, std::string{chunk})))
                                     {
                                         writer->abort();
                                         fail(response, Common::kCodeInternalServerError, "固件写入失败", traceId);
                                         co_return;
                                     }
                                 }
                                 if (bodyStream->isTruncated())
                                 {
                                     writer->abort();
                                     fail(response, Common::kCodeBadRequest, "上传正文不完整", traceId);
                                     co_return;
                                 }
                                 const auto digest = co_await writer->finish(loop);
                                 if (!digest.has_value())
                                 {
                                     writer->abort();
                                     fail(response, Common::kCodeInternalServerError, "固件摘要或落盘失败", traceId);
                                     co_return;
                                 }
                                 if (signatureHeader.has_value())
                                 {
                                     const auto verified = deps->signingPolicy->verifier->verify(*keyIdHeader, *digest, *signatureHeader);
                                     if (!verified.has_value())
                                     {
                                         // 验签发生在成品重命名之后；删除要在工作线程上做，事件循环线程不做同步文件 IO
                                         co_await removeStoredFile(loop, finalPath);
                                         fail(response, Common::kCodeBadRequest, verified.error(), traceId);
                                         co_return;
                                     }
                                 }
                                 FirmwareRecord record;
                                 record.id                 = id;
                                 record.name               = name;
                                 record.version            = version;
                                 record.fileName           = safeName;
                                 record.contentType        = request.getHeader("content-type").value_or("application/octet-stream");
                                 record.sizeBytes          = static_cast<std::int64_t>(totalBytes);
                                 record.sha256             = *digest;
                                 record.signature          = signatureHeader.value_or("");
                                 record.signingKeyId       = keyIdHeader.value_or("");
                                 record.signatureAlgorithm = signatureHeader.has_value() ? "ed25519-sha256" : "";
                                 record.storagePath        = finalPath;
                                 record.createdBy          = session->userId;
                                 record.createdAt          = Common::nowDateTimeUtc();
                                 record.tenantId           = targetTenantId;
                                 // co_await 不能写在 catch 里，先把失败原因取出来，出了 catch 再删成品
                                 std::optional<std::string> createFailure;
                                 try
                                 {
                                     static_cast<void>(co_await firmware->create(loop, record));
                                 } catch (const std::exception &failure)
                                 {
                                     createFailure = failure.what();
                                 }
                                 if (createFailure.has_value())
                                 {
                                     co_await removeStoredFile(loop, finalPath);
                                     fail(response, Common::kCodeInternalServerError, *createFailure, traceId);
                                     co_return;
                                 }
                                 Common::writeSuccess(response, firmwareJson(record), traceId);
                                 co_return;
                             });

        router.post("/api/firmware/:id/deploy",
                    [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:firmware:upload", traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const auto id   = Common::parsePositiveIdentifier(request.param("id"));
                        const auto body = request.jsonBody();
                        if (!id.has_value() || !body.has_value() || !body->is_object())
                        {
                            fail(response, Common::kCodeBadRequest, "固件标识或请求正文不合法", traceId);
                            co_return;
                        }
                        const auto firmware = co_await deps->firmware->scoped(session->tenantScope())->find(loop, *id);
                        if (!firmware.has_value())
                        {
                            fail(response, Common::kCodeNotFound, "固件不存在", traceId);
                            co_return;
                        }
                        if (signatureRequired(*deps) && firmware->signature.empty())
                        {
                            fail(response, Common::kCodeBadRequest, "该固件未签名，当前部署要求签名固件", traceId);
                            co_return;
                        }
                        const auto nodeIds = Json::integerArrayField(*body, "nodeIds");
                        if (nodeIds.empty() || nodeIds.size() > 100)
                        {
                            fail(response, Common::kCodeBadRequest, "nodeIds 必须是 1 到 100 个节点", traceId);
                            co_return;
                        }
                        // 目标节点必须在会话租户内可见且与固件同租户：否则任务行会指向看不见的节点，任务列表还可能带出别家节点名
                        const auto invalidTargets = co_await invalidFirmwareTargets(loop, *deps->nodes, session->tenantScope(), nodeIds, firmware->tenantId, Auth::DataScope{true, false, session->userId, {}});
                        if (!invalidTargets.empty())
                        {
                            fail(response, Common::kCodeBadRequest, "目标节点不存在、不可见或不属于固件所在租户：" + joinedIdentifiers(invalidTargets), traceId);
                            co_return;
                        }
                        const auto batchSize = Json::optionalInteger(*body, "batchSize");
                        if (batchSize.has_value() && (*batchSize < 1 || *batchSize > 100))
                        {
                            fail(response, Common::kCodeBadRequest, "batchSize 必须在 1 到 100 之间", traceId);
                            co_return;
                        }
                        const std::size_t batchCount   = batchSize.has_value() ? std::min<std::size_t>(static_cast<std::size_t>(*batchSize), nodeIds.size()) : nodeIds.size();
                        const auto        firmwareRepo = deps->firmware->scoped(session->tenantScope());
                        const auto        commandRepo  = deps->commands->scoped(session->tenantScope());
                        ConfigValue       results      = ConfigValue::array();
                        ConfigValue       deferred     = ConfigValue::array();
                        for (std::size_t index = 0; index < nodeIds.size(); ++index)
                        {
                            const std::int64_t nodeId = nodeIds[index];
                            if (index >= batchCount)
                            {
                                deferred.push_back(nodeId);
                                continue;
                            }
                            const std::int64_t previousFirmwareId = co_await firmwareRepo->latestOtherFirmware(loop, nodeId, *id);
                            const std::int64_t taskId             = co_await firmwareRepo->createTask(loop, *id, nodeId, previousFirmwareId);
                            if (taskId == 0)
                            {
                                continue;
                            }
                            const auto command = co_await commandRepo->create(loop, nodeId, "firmware.update", downloadPayload(*firmware, nodeId), session->userId, 600);
                            if (!command.has_value())
                            {
                                continue;
                            }
                            // 任务先于命令建立，命令号必须在发布前回填；否则升级后的 Agent 带 commandId 回报会找不到任务
                            if (!co_await firmwareRepo->bindTaskCommand(loop, taskId, command->id))
                            {
                                LOG_WARN_FMT("固件任务 {} 绑定命令 {} 失败，跳过下发", taskId, command->id);
                                continue;
                            }
                            if (deps->realtimeBus != nullptr)
                            {
                                co_await deps->realtimeBus->publishAgent(loop, Command::makeAgentCommandMessage(*command));
                            }
                            if (deps->notifier)
                            {
                                static_cast<void>(co_await deps->notifier(*command));
                            }
                            results.push_back(ConfigValue::object({{"nodeId", nodeId}, {"taskId", taskId}, {"commandId", command->id}}));
                        }
                        if (batchSize.has_value())
                        {
                            Common::writeSuccess(response, ConfigValue::object({{"deployed", std::move(results)}, {"deferredNodeIds", std::move(deferred)}}), traceId);
                            co_return;
                        }
                        Common::writeSuccess(response, std::move(results), traceId);
                        co_return;
                    });

        router.post("/api/firmware/:id/rollback",
                    [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:firmware:upload", traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const auto id   = Common::parsePositiveIdentifier(request.param("id"));
                        const auto body = request.jsonBody();
                        if (!id.has_value() || !body.has_value() || !body->is_object())
                        {
                            fail(response, Common::kCodeBadRequest, "固件标识或请求正文不合法", traceId);
                            co_return;
                        }
                        const auto nodeIds = Json::integerArrayField(*body, "nodeIds");
                        if (nodeIds.empty() || nodeIds.size() > 100)
                        {
                            fail(response, Common::kCodeBadRequest, "nodeIds 必须是 1 到 100 个节点", traceId);
                            co_return;
                        }
                        const auto firmwareRepo = deps->firmware->scoped(session->tenantScope());
                        const auto firmware     = co_await firmwareRepo->find(loop, *id);
                        if (!firmware.has_value())
                        {
                            fail(response, Common::kCodeNotFound, "固件不存在", traceId);
                            co_return;
                        }
                        // 回滚目标必须是本租户可见节点，且与源固件同租户；校验放在解析历史版本与建任务之前
                        const auto invalidTargets = co_await invalidFirmwareTargets(loop, *deps->nodes, session->tenantScope(), nodeIds, firmware->tenantId, Auth::DataScope{true, false, session->userId, {}});
                        if (!invalidTargets.empty())
                        {
                            fail(response, Common::kCodeBadRequest, "目标节点不存在、不可见或不属于固件所在租户：" + joinedIdentifiers(invalidTargets), traceId);
                            co_return;
                        }
                        const auto commandRepo = deps->commands->scoped(session->tenantScope());
                        // 先解析全部回滚目标并在下发前统一校验签名，避免部分节点已建任务后才发现目标未签名
                        struct RollbackTarget
                        {
                            std::int64_t   nodeId{0};     ///< 目标节点
                            std::int64_t   firmwareId{0}; ///< 回滚到的固件
                            FirmwareRecord firmware;      ///< 回滚目标元数据
                        };
                        std::vector<RollbackTarget> targets;
                        for (const std::int64_t nodeId: nodeIds)
                        {
                            const std::int64_t targetId = co_await firmwareRepo->rollbackSource(loop, *id, nodeId);
                            if (targetId == 0)
                            {
                                continue;
                            }
                            auto target = co_await firmwareRepo->find(loop, targetId);
                            if (!target.has_value())
                            {
                                continue;
                            }
                            if (signatureRequired(*deps) && target->signature.empty())
                            {
                                fail(response, Common::kCodeBadRequest, "回滚目标未签名，当前部署要求签名固件", traceId);
                                co_return;
                            }
                            targets.push_back(RollbackTarget{nodeId, targetId, std::move(*target)});
                        }
                        if (targets.empty())
                        {
                            fail(response, Common::kCodeNotFound, "没有可回滚的历史版本", traceId);
                            co_return;
                        }
                        ConfigValue results = ConfigValue::array();
                        for (const RollbackTarget &rollbackTarget: targets)
                        {
                            const std::int64_t nodeId   = rollbackTarget.nodeId;
                            const std::int64_t targetId = rollbackTarget.firmwareId;
                            const std::int64_t taskId   = co_await firmwareRepo->createTask(loop, targetId, nodeId, *id);
                            if (taskId == 0)
                            {
                                continue;
                            }
                            const auto command =
                                    co_await commandRepo->create(loop, nodeId, "firmware.update", downloadPayload(rollbackTarget.firmware, nodeId), session->userId, 600);
                            if (!command.has_value())
                            {
                                continue;
                            }
                            // 回滚同样要在发布前绑定命令号，否则升级后的 Agent 回报会被当成不存在的任务
                            if (!co_await firmwareRepo->bindTaskCommand(loop, taskId, command->id))
                            {
                                LOG_WARN_FMT("固件任务 {} 绑定命令 {} 失败，跳过下发", taskId, command->id);
                                continue;
                            }
                            if (deps->realtimeBus != nullptr)
                            {
                                co_await deps->realtimeBus->publishAgent(loop, Command::makeAgentCommandMessage(*command));
                            }
                            if (deps->notifier)
                            {
                                static_cast<void>(co_await deps->notifier(*command));
                            }
                            results.push_back(
                                    ConfigValue::object({{"nodeId", nodeId}, {"fromFirmwareId", *id}, {"toFirmwareId", targetId}, {"taskId", taskId}, {"commandId", command->id}}));
                        }
                        if (results.empty())
                        {
                            fail(response, Common::kCodeNotFound, "没有可回滚的历史版本", traceId);
                            co_return;
                        }
                        Common::writeSuccess(response, std::move(results), traceId);
                        co_return;
                    });

        router.del("/api/firmware/:id",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:firmware:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto id = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "固件标识不合法", traceId);
                           co_return;
                       }
                       const auto removed = co_await deps->firmware->scoped(session->tenantScope())->remove(loop, *id);
                       if (!removed.has_value())
                       {
                           fail(response, Common::kCodeNotFound, "固件不存在", traceId);
                           co_return;
                       }
                       // 文件删除是阻塞 IO，放到工作线程；失败只记日志，响应仍按元数据删除成功处理
                       co_await removeStoredFile(loop, removed->storagePath);
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        registerRolloutRoutes(router, loop, dependencyInput);
    }

    void registerFirmwareAgentRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const FirmwareDependencies &dependencyInput)
    {
        const auto deps = std::make_shared<FirmwareDependencies>(dependencyInput);
        // Agent 没有会话：固件下载与任务回写按全租户视图查找，归属由固件自身决定
        const auto allFirmware = deps->firmware->scoped(System::TenantScope{0, true});

        router.get("/api/agent/firmware/:id/download",
                   [&loop, deps, allFirmware](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        id      = Common::parsePositiveIdentifier(request.param("id"));
                       const auto        nodeId  = Common::queryInteger(request.queryParams(), "nodeId");
                       if (!id.has_value() || !nodeId.has_value() || *nodeId <= 0)
                       {
                           fail(response, Common::kCodeBadRequest, "固件标识或 nodeId 不合法", traceId);
                           co_return;
                       }
                       const auto identity = co_await Device::requireAgentNode(loop, *deps->nodes, request, *nodeId);
                       if (!identity.has_value())
                       {
                           fail(response, identity.error().httpStatus, identity.error().message, traceId);
                           co_return;
                       }
                       const auto firmware = co_await allFirmware->find(loop, *id);
                       if (!firmware.has_value())
                       {
                           fail(response, Common::kCodeNotFound, "固件不存在", traceId);
                           co_return;
                       }
                       // 固件归属必须与节点一致，否则任一租户的 Agent 都能取走别家的镜像
                       if (firmware->tenantId != identity->tenantId)
                       {
                           fail(response, Common::kCodeForbidden, "固件不属于该节点所在租户", traceId);
                           co_return;
                       }
                       AsynGyanis::Platform::MemoryMappedFile mapped = AsynGyanis::Platform::MemoryMappedFile::open(firmware->storagePath);
                       if (!mapped.isValid())
                       {
                           fail(response, Common::kCodeNotFound, "固件文件不存在或不可读", traceId);
                           co_return;
                       }
                       const std::int64_t size = static_cast<std::int64_t>(mapped.bytes().size());
                       const std::string  etag = "\"" + firmware->sha256 + "\"";
                       response.setHeader("ETag", etag);
                       response.setHeader("Accept-Ranges", "bytes");
                       response.setHeader("Content-Type", firmware->contentType);
                       response.setHeader("Content-Disposition", "attachment; filename=\"" + firmware->fileName + "\"");
                       // 未签名固件照常下载，但不宣称签名；签名相关头部只在记录确实带签名时出现
                       if (!firmware->signature.empty())
                       {
                           response.setHeader("X-Firmware-Signature", firmware->signature);
                           response.setHeader("X-Firmware-Key-Id", firmware->signingKeyId);
                           response.setHeader("X-Firmware-Signature-Algorithm", firmware->signatureAlgorithm);
                       }
                       if (const auto ifNoneMatch = request.getHeader("if-none-match"); ifNoneMatch.has_value() && ifNoneMatch->find(firmware->sha256) != std::string::npos)
                       {
                           response.setStatus(304);
                           co_return;
                       }
                       std::optional<ByteRange> range;
                       if (const auto rangeHeader = request.getHeader("range"); rangeHeader.has_value())
                       {
                           range = parseRange(*rangeHeader, size);
                           if (!range.has_value())
                           {
                               response.setHeader("Content-Range", "bytes */" + std::to_string(size));
                               fail(response, 416, "Range 不合法或不可满足", traceId);
                               co_return;
                           }
                       }
                       const std::int64_t offset = range.has_value() ? range->start : 0;
                       const std::int64_t length = range.has_value() ? range->length : size;
                       response.setStatus(range.has_value() ? 206 : 200);
                       response.setHeader("Content-Length", std::to_string(length));
                       if (range.has_value())
                       {
                           response.setHeader("Content-Range", "bytes " + std::to_string(offset) + "-" + std::to_string(offset + length - 1) + "/" + std::to_string(size));
                       }
                       response.setMappedBody(std::move(mapped), static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
                       co_return;
                   });

        router.post("/api/agent/firmware/:id/status",
                    [&loop, deps, allFirmware](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        id      = Common::parsePositiveIdentifier(request.param("id"));
                        const auto        body    = request.jsonBody();
                        if (!id.has_value() || !body.has_value() || !body->is_object())
                        {
                            fail(response, Common::kCodeBadRequest, "固件标识或请求正文不合法", traceId);
                            co_return;
                        }
                        const auto        nodeId   = Json::optionalInteger(*body, "nodeId");
                        const std::string status   = Json::stringField(*body, "status");
                        const int         progress = static_cast<int>(std::clamp<std::int64_t>(Json::integerField(*body, "progress", 0), 0, 100));
                        const std::string message  = Json::stringField(*body, "message");
                        // commandId 是新增的可选归因字段：旧 Agent 不发送时按 0 处理，维持原来按固件与节点更新
                        const std::int64_t commandId = std::max<std::int64_t>(0, Json::optionalInteger(*body, "commandId").value_or(0));
                        if (!nodeId.has_value() || (status != "pending" && status != "downloading" && status != "installing" && status != "installed" && status != "failed"))
                        {
                            fail(response, Common::kCodeBadRequest, "nodeId 或状态不合法", traceId);
                            co_return;
                        }
                        const auto identity = co_await Device::requireAgentNode(loop, *deps->nodes, request, *nodeId);
                        if (!identity.has_value())
                        {
                            fail(response, identity.error().httpStatus, identity.error().message, traceId);
                            co_return;
                        }
                        const auto task = co_await allFirmware->updateTask(loop, *id, *nodeId, status, progress, message, commandId);
                        if (!task.has_value())
                        {
                            fail(response, Common::kCodeNotFound, "固件任务不存在", traceId);
                            co_return;
                        }
                        // 已处于终态的任务返回当前状态而不是 404：Agent 的迟到回报按幂等成功处理，不触发重试
                        Common::writeSuccess(response, taskJson(*task), traceId);
                        co_return;
                    });
    }
} // namespace EdgePulse::Firmware
