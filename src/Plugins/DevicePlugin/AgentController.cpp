#include "Plugins/DevicePlugin/AgentController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Device/AgentAuthorization.h"
#include "Core/Device/AgentCertificateAuthority.h"
#include "Core/Device/AgentRegistry.h"
#include "Core/Device/NodeConfigRepository.h"
#include "Core/System/TenantRepository.h"
#include "Plugins/DevicePlugin/NodeJson.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "Core/Http/RequestId.h"

#include <Core/EventLoop/EventLoop.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
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
                case AgentError::Reason::Conflict:
                    Common::writeFailure(response, Common::kCodeConflict, Common::kCodeConflict, error.message, traceId);
                    break;
            }
        }

        /**
         * @brief 取指纹前 12 位，供日志定位而不回显完整凭据
         * @param fingerprint 证书指纹
         * @return std::string 前缀；空指纹返回 none
         */
        [[nodiscard]] std::string fingerprintPrefix(const std::string_view fingerprint)
        {
            if (fingerprint.empty())
            {
                return "none";
            }
            return std::string{fingerprint.substr(0, std::min<std::size_t>(12, fingerprint.size()))};
        }

        /**
         * @brief 计算旧证书退出重叠窗口的时间
         * @param node 轮换前的节点
         * @param overlapHours 重叠窗口小时数
         * @return std::string 旧证书到期时间与 now 加窗口两者中较早的一个
         */
        [[nodiscard]] std::string overlapDeadline(const NodeRecord &node, const int overlapHours)
        {
            const std::string cutoff = Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::hours{overlapHours});
            // 旧证书自身到期更早时不能借重叠窗口续命
            return node.certNotAfter.empty() || node.certNotAfter > cutoff ? cutoff : node.certNotAfter;
        }

        /**
         * @brief 把一次签发结果拼成仓储的轮换输入
         * @param node 轮换前的节点
         * @param issued 新签发的证书
         * @param overlapHours 重叠窗口小时数
         * @return AgentCertificateRotation 轮换字段
         */
        [[nodiscard]] AgentCertificateRotation rotationFor(const NodeRecord &node, const IssuedAgentCertificate &issued, const int overlapHours)
        {
            AgentCertificateRotation rotation;
            rotation.fingerprint         = issued.fingerprint;
            rotation.notAfter            = issued.notAfter;
            rotation.previousFingerprint = node.certFingerprint;
            rotation.previousNotAfter    = overlapDeadline(node, overlapHours);
            rotation.serial              = issued.serial;
            rotation.subject             = issued.subject;
            rotation.notBefore           = issued.notBefore;
            return rotation;
        }

        /**
         * @brief 判断签发失败是否由请求方材料引起
         * @param message 签发器返回的中文原因
         * @return bool CSR 相关失败按 400，其余按 500
         */
        [[nodiscard]] bool clientCertificateError(const std::string &message)
        {
            return message.find("CSR") != std::string::npos;
        }

        /**
         * @brief 以节点快照的版本为条件写入一次证书轮换
         * @param loop 恢复协程用的事件循环
         * @param nodes 节点仓储
         * @param snapshot 读取到的节点快照
         * @param rotation 轮换字段
         * @return 成功为 true；版本冲突、吊销或删除为 false
         */
        AsynGyanis::Core::Task<std::expected<bool, std::string>> rotateNodeCertificate(AsynGyanis::Core::EventLoop &loop, NodeRepository &nodes, const NodeRecord &snapshot,
                                                                                       AgentCertificateRotation rotation)
        {
            co_return co_await nodes.rotateCertificate(loop, System::TenantScope{snapshot.tenantId, false}, snapshot.id, snapshot.version, std::move(rotation));
        }
    } // namespace

    void registerRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, AgentRegistry &registry, NodeRepository &nodes, System::TenantRepository &tenants,
                        const int heartbeatIntervalSeconds, NodeConfigRepository *nodeConfig, AgentCertificateAuthority *certificateAuthority,
                        std::function<void(std::int64_t)> beforeCertificateRotation)
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

        router.post("/api/agent/certificate/renew",
                    [&loop, &nodes, &tenants, certificateAuthority, beforeCertificateRotation = std::move(beforeCertificateRotation)](
                            AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        // 续期响应含一次性证书 PEM，失败响应也可能带节点信息，统一禁止缓存
                        static_cast<void>(response.setHeader("Cache-Control", "no-store"));
                        const std::optional<AsynGyanis::Base::ConfigValue> body = request.jsonBody();
                        if (!body.has_value() || !body->is_object())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                            co_return;
                        }
                        const auto                       nodeIdIterator = body->find("nodeId");
                        const std::optional<std::string> csrPem         = stringField(*body, "csrPem");
                        if (nodeIdIterator == body->end() || !nodeIdIterator->is_number_integer() || !csrPem.has_value() || csrPem->empty())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少整数 nodeId 或非空 csrPem", traceId);
                            co_return;
                        }
                        const std::int64_t nodeId   = nodeIdIterator->get<std::int64_t>();
                        const auto         identity = co_await requireAgentNode(loop, nodes, request, nodeId);
                        if (!identity.has_value())
                        {
                            Common::writeFailure(response, identity.error().httpStatus, identity.error().httpStatus, identity.error().message, traceId);
                            co_return;
                        }
                        // 停用租户不能借续期继续维持 Agent 通道
                        const auto tenant = co_await tenants.findById(loop, identity->tenantId);
                        if (!tenant.has_value() || tenant->status != 1)
                        {
                            Common::writeFailure(response, Common::kCodeForbidden, Common::kCodeForbidden, "租户已停用，无法续期证书", traceId);
                            co_return;
                        }
                        if (certificateAuthority == nullptr || !certificateAuthority->enabled())
                        {
                            Common::writeFailure(response, Common::kCodeServiceUnavailable, Common::kCodeServiceUnavailable, "Agent 内部 CA 未启用，无法续期证书", traceId);
                            co_return;
                        }

                        auto issued = certificateAuthority->issue(*csrPem, identity->id, identity->name);
                        if (!issued.has_value())
                        {
                            if (clientCertificateError(issued.error()))
                            {
                                Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, issued.error(), traceId);
                                co_return;
                            }
                            LOG_ERROR_FMT("Agent 证书签发失败：node={} reason={}", nodeId, issued.error());
                            Common::writeFailure(response, Common::kCodeInternalServerError, Common::kCodeInternalServerError, "证书签发失败，请稍后重试", traceId);
                            co_return;
                        }

                        NodeRecord current = *identity;
                        if (beforeCertificateRotation)
                        {
                            beforeCertificateRotation(current.id);
                        }
                        auto rotated = co_await rotateNodeCertificate(loop, nodes, current, rotationFor(current, *issued, certificateAuthority->overlapHours()));
                        if (!rotated.has_value())
                        {
                            LOG_ERROR_FMT("Agent 证书续期写入失败：node={} reason={}", nodeId, rotated.error());
                            Common::writeFailure(response, Common::kCodeInternalServerError, Common::kCodeInternalServerError, "证书续期写入失败，请稍后重试", traceId);
                            co_return;
                        }
                        if (!*rotated)
                        {
                            // 版本条件落空：重读一次，区分并发更新、吊销与删除，再给一次重试机会
                            const auto refreshed = co_await nodes.scoped(System::TenantScope{0, true})->findById(loop, nodeId);
                            if (!refreshed.has_value())
                            {
                                Common::writeFailure(response, Common::kCodeNotFound, Common::kCodeNotFound, "节点不存在或已删除，请先完成注册", traceId);
                                co_return;
                            }
                            if (!refreshed->certRevokedAt.empty())
                            {
                                Common::writeFailure(response, Common::kCodeForbidden, Common::kCodeForbidden, "节点证书已吊销，请重新申请注册令牌并完成注册", traceId);
                                co_return;
                            }
                            current = *refreshed;
                            if (beforeCertificateRotation)
                            {
                                beforeCertificateRotation(current.id);
                            }
                            rotated = co_await rotateNodeCertificate(loop, nodes, current, rotationFor(current, *issued, certificateAuthority->overlapHours()));
                            if (!rotated.has_value())
                            {
                                LOG_ERROR_FMT("Agent 证书续期写入失败：node={} reason={}", nodeId, rotated.error());
                                Common::writeFailure(response, Common::kCodeInternalServerError, Common::kCodeInternalServerError, "证书续期写入失败，请稍后重试", traceId);
                                co_return;
                            }
                            if (!*rotated)
                            {
                                // 第二次仍撞版本：收手，让 Agent 重新读取后再发起，避免无限重试与互相覆盖
                                const auto latest = co_await nodes.scoped(System::TenantScope{0, true})->findById(loop, nodeId);
                                if (!latest.has_value())
                                {
                                    Common::writeFailure(response, Common::kCodeNotFound, Common::kCodeNotFound, "节点不存在或已删除，请先完成注册", traceId);
                                    co_return;
                                }
                                if (!latest->certRevokedAt.empty())
                                {
                                    Common::writeFailure(response, Common::kCodeForbidden, Common::kCodeForbidden, "节点证书已吊销，请重新申请注册令牌并完成注册", traceId);
                                    co_return;
                                }
                                Common::writeFailure(response, Common::kCodeConflict, Common::kCodeConflict, "节点证书已被并发更新，请重试", traceId);
                                co_return;
                            }
                        }

                        LOG_INFO_FMT("Agent 证书续期：node={} old={} new={} serial={} notAfter={}", current.id, fingerprintPrefix(current.certFingerprint),
                                     fingerprintPrefix(issued->fingerprint), issued->serial, issued->notAfter);
                        Common::writeSuccess(response,
                                             AsynGyanis::Base::ConfigValue::object({{"certificatePem", issued->certificatePem},
                                                                                    {"fingerprint", issued->fingerprint},
                                                                                    {"notBefore", issued->notBefore},
                                                                                    {"notAfter", issued->notAfter},
                                                                                    {"serial", issued->serial}}),
                                             traceId);
                        co_return;
                    });

        router.post("/api/agent/heartbeat",
                    [&loop, &registry, &nodes, heartbeatIntervalSeconds, nodeConfig](AsynGyanis::Net::HttpRequest  &request,
                                                                                     AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
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
                        // Agent 没有会话，配置版本按全租户视图查询；节点归属已由证书绑定确定
                        std::int64_t desiredConfigVersion = 0;
                        if (nodeConfig != nullptr)
                        {
                            desiredConfigVersion = co_await nodeConfig->scoped(System::TenantScope{0, true})->desiredVersion(loop, identity->id);
                        }
                        payload["desiredConfigVersion"]     = desiredConfigVersion;
                        payload["heartbeatIntervalSeconds"] = heartbeatIntervalSeconds;
                        Common::writeSuccess(response, std::move(payload), traceId);
                        co_return;
                    });
    }
} // namespace EdgePulse::Device
