#include "Plugins/DevicePlugin/AgentController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Core/Device/AgentAuthorization.h"
#include "Core/Device/AgentRegistry.h"
#include "Core/System/TenantRepository.h"
#include "Plugins/DevicePlugin/NodeJson.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "Core/Http/RequestId.h"

#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Device
{
    namespace
    {
        [[nodiscard]] std::optional<std::string> stringField(const AsynGyanis::Base::ConfigValue &body, const std::string_view key)
        {
            if (!body.is_object())
            {
                return std::nullopt;
            }
            const auto iterator = body.find(key);
            if (iterator == body.end() || !iterator->is_string())
            {
                return std::nullopt;
            }
            return iterator->get<std::string>();
        }

        [[nodiscard]] std::optional<std::string> labelsField(const AsynGyanis::Base::ConfigValue &body)
        {
            if (!body.is_object())
            {
                return std::nullopt;
            }
            const auto iterator = body.find("labels");
            if (iterator == body.end() || iterator->is_null())
            {
                return std::string{};
            }
            if (iterator->is_object())
            {
                return iterator->dump();
            }
            if (iterator->is_string())
            {
                return iterator->get<std::string>();
            }
            return std::nullopt;
        }

        void writeAgentError(AsynGyanis::Net::HttpResponse &response, const AgentError &error, const std::string_view traceId)
        {
            switch (error.reason)
            {
                case AgentError::Reason::InvalidRequest:
                    Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, error.message, traceId);
                    break;
                case AgentError::Reason::FingerprintRejected:
                    Common::writeFailure(response, Common::kCodeForbidden, Common::kCodeForbidden, error.message, traceId);
                    break;
                case AgentError::Reason::NotFound:
                    Common::writeFailure(response, Common::kCodeNotFound, Common::kCodeNotFound, error.message, traceId);
                    break;
            }
        }
    } // namespace

    void registerRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, AgentRegistry &registry, NodeRepository &nodes, System::TenantRepository &tenants,
                        const int heartbeatIntervalSeconds)
    {
        router.post("/api/agent/register",
                    [&loop, &registry, &tenants](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string                                  traceId = Web::resolveRequestId(request);
                        const std::optional<AsynGyanis::Base::ConfigValue> body    = request.jsonBody();
                        if (!body.has_value())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                            co_return;
                        }
                        const std::optional<std::string> name        = stringField(*body, "name");
                        const std::optional<std::string> version     = stringField(*body, "agentVersion");
                        const std::optional<std::string> fingerprint = stringField(*body, "certFingerprint");
                        const std::optional<std::string> labels      = labelsField(*body);
                        if (!name.has_value() || !version.has_value() || !fingerprint.has_value() || !labels.has_value())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少 name、agentVersion、certFingerprint，或 labels 格式不合法",
                                                 traceId);
                            co_return;
                        }
                        // 令牌字段可省略；一旦出现就必须是字符串，不能把数字或对象当成“没有令牌”
                        std::string enrollmentToken;
                        if (body->is_object() && body->contains("enrollmentToken"))
                        {
                            const auto tokenIterator = body->find("enrollmentToken");
                            if (tokenIterator == body->end() || !tokenIterator->is_string())
                            {
                                Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "enrollmentToken 必须是字符串", traceId);
                                co_return;
                            }
                            enrollmentToken = tokenIterator->get<std::string>();
                        }
                        // 注册请求用租户编码声明归属；缺省落在默认租户
                        const std::string tenantCode = stringField(*body, "tenantCode").value_or("default");
                        const auto        tenant     = co_await tenants.findByCode(loop, tenantCode);
                        if (!tenant.has_value())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "租户编码不存在", traceId);
                            co_return;
                        }
                        if (tenant->status != 1)
                        {
                            Common::writeFailure(response, Common::kCodeForbidden, Common::kCodeForbidden, "租户已停用", traceId);
                            co_return;
                        }
                        AgentRegistration registration{*name, *version, *fingerprint, *labels, tenant->id};
                        registration.connectionIdentity = request.tlsPeerIdentity();
                        registration.enrollmentToken    = std::move(enrollmentToken);
                        const auto result               = co_await registry.registerAgent(loop, std::move(registration));
                        if (!result.has_value())
                        {
                            writeAgentError(response, result.error(), traceId);
                            co_return;
                        }
                        Common::writeSuccess(response, nodeToJson(*result), traceId);
                        co_return;
                    });

        router.post("/api/agent/heartbeat",
                    [&loop, &registry, &nodes, heartbeatIntervalSeconds](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string                                  traceId = Web::resolveRequestId(request);
                        const std::optional<AsynGyanis::Base::ConfigValue> body    = request.jsonBody();
                        if (!body.has_value() || !body->is_object())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                            co_return;
                        }
                        const auto                       nodeIdIterator = body->find("nodeId");
                        const std::optional<std::string> agentVersion   = stringField(*body, "agentVersion");
                        if (nodeIdIterator == body->end() || !nodeIdIterator->is_number_integer() || !agentVersion.has_value())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少整数 nodeId 或字符串 agentVersion", traceId);
                            co_return;
                        }
                        const auto identity = co_await requireAgentNode(loop, nodes, request, nodeIdIterator->get<std::int64_t>());
                        if (!identity.has_value())
                        {
                            Common::writeFailure(response, identity.error().httpStatus, identity.error().httpStatus, identity.error().message, traceId);
                            co_return;
                        }
                        const auto result = co_await registry.heartbeat(loop, identity->id, *agentVersion);
                        if (!result.has_value())
                        {
                            writeAgentError(response, result.error(), traceId);
                            co_return;
                        }
                        // 心跳响应同时充当 Agent 的节奏协商：interval 由服务端配置下发，节点无需自定
                        AsynGyanis::Base::ConfigValue payload = nodeToJson(result->node);
                        payload["heartbeatIntervalSeconds"]   = heartbeatIntervalSeconds;
                        Common::writeSuccess(response, std::move(payload), traceId);
                        co_return;
                    });
    }
} // namespace EdgePulse::Device
