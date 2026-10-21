#include "Plugins/SystemPlugin/NotificationController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/Pagination.h"
#include "Base/Api/QueryParameters.h"
#include "Base/Api/RequestParsing.h"
#include "Base/Json/JsonFields.h"
#include "Core/Auth/AuthService.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Core/Notification/NotificationRepository.h"
#include "Core/System/TenantRepository.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace EdgePulse::Notification
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        [[nodiscard]] ConfigValue channelJson(const ChannelRecord &channel)
        {
            return ConfigValue::object({{"id", channel.id},
                                        {"tenantId", channel.tenantId},
                                        {"name", channel.name},
                                        {"type", channel.type},
                                        {"endpoint", channel.endpoint},
                                        {"recipients", channel.recipients},
                                        {"username", channel.username},
                                        {"sender", channel.sender},
                                        {"caFile", channel.caFile},
                                        {"minLevel", channel.minLevel},
                                        {"enabled", channel.enabled},
                                        {"secretConfigured", !channel.secret.empty()},
                                        {"createdAt", channel.createdAt}});
        }

        void fail(AsynGyanis::Net::HttpResponse &response, const int status, const std::string &message, const std::string_view traceId)
        {
            Common::writeFailure(response, status, status, message, traceId);
        }

        [[nodiscard]] bool validLevel(const std::string &level)
        {
            return level == "info" || level == "warning" || level == "critical";
        }

        [[nodiscard]] std::optional<ChannelRecord> parseChannel(const ConfigValue &body, ChannelRecord record, std::string &error)
        {
            record.name       = Json::stringField(body, "name");
            record.type       = Json::stringField(body, "type");
            record.endpoint   = Json::stringField(body, "endpoint");
            record.recipients = Json::stringField(body, "recipients");
            record.username   = Json::stringField(body, "username");
            record.secret     = Json::stringField(body, "secret");
            record.sender     = Json::stringField(body, "sender");
            record.caFile     = Json::stringField(body, "caFile");
            record.minLevel   = Json::stringField(body, "minLevel", "info");
            record.enabled    = Json::boolField(body, "enabled", true);
            if (record.name.empty() || record.name.size() > 64)
            {
                error = "渠道名称必须是 1 到 64 个字符";
                return std::nullopt;
            }
            if (record.type != "webhook" && record.type != "email")
            {
                error = "渠道类型只能是 webhook 或 email";
                return std::nullopt;
            }
            if (!validLevel(record.minLevel))
            {
                error = "minLevel 只能是 info、warning 或 critical";
                return std::nullopt;
            }
            if (record.type == "webhook")
            {
                if (!record.endpoint.starts_with("https://") && !record.endpoint.starts_with("http://"))
                {
                    error = "Webhook 地址必须以 http:// 或 https:// 开头";
                    return std::nullopt;
                }
                if (record.endpoint.size() > 512)
                {
                    error = "Webhook 地址不能超过 512 个字符";
                    return std::nullopt;
                }
            } else
            {
                const auto hasNewline = [](const std::string &text) { return text.find('\r') != std::string::npos || text.find('\n') != std::string::npos; };
                if (hasNewline(record.recipients) || hasNewline(record.sender) || hasNewline(record.username))
                {
                    error = "收件人、发件人与用户名不能包含换行";
                    return std::nullopt;
                }
                if (!record.endpoint.starts_with("smtp://") && !record.endpoint.starts_with("smtps://") && !record.endpoint.starts_with("starttls://"))
                {
                    error = "SMTP 地址必须以 smtp://、smtps:// 或 starttls:// 开头";
                    return std::nullopt;
                }
                if (record.recipients.empty() || record.recipients.size() > 512)
                {
                    error = "邮件渠道必须填写收件人，且不超过 512 个字符";
                    return std::nullopt;
                }
                if (record.sender.empty() && record.username.empty())
                {
                    error = "邮件渠道必须填写发件人或认证用户名";
                    return std::nullopt;
                }
            }
            return record;
        }
    } // namespace

    void registerNotificationRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const NotificationDependencies &dependencyInput)
    {
        const auto deps = std::make_shared<NotificationDependencies>(dependencyInput);

        router.get("/api/system/notifications",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "system:notification:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap       query  = request.queryParams();
                       const Common::PageParameters page   = Common::parsePageParameters(query);
                       const auto                   result = co_await deps->channels->scoped(session->tenantScope())->list(loop, Common::queryText(query, "keyword"), page);
                       ConfigValue                  rows   = ConfigValue::array();
                       for (const ChannelRecord &channel: result.rows)
                       {
                           rows.push_back(channelJson(channel));
                       }
                       Common::writeSuccess(response, Common::makePageResult(page, result.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.post("/api/system/notifications",
                    [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "system:notification:add", traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const auto body = request.jsonBody();
                        if (!body.has_value() || !body->is_object())
                        {
                            fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                            co_return;
                        }
                        std::string error;
                        auto        record = parseChannel(*body, ChannelRecord{}, error);
                        if (!record.has_value())
                        {
                            fail(response, Common::kCodeBadRequest, error, traceId);
                            co_return;
                        }
                        std::int64_t targetTenant = session->tenantId;
                        if (session->platformAdmin && body->contains("tenantId"))
                        {
                            const auto tenantId = Json::optionalInteger(*body, "tenantId");
                            const auto tenant   = tenantId.has_value() && *tenantId > 0 ? co_await deps->tenants->findById(loop, *tenantId) : std::nullopt;
                            if (!tenant.has_value() || tenant->status != 1)
                            {
                                fail(response, Common::kCodeBadRequest, "目标租户不存在或已停用", traceId);
                                co_return;
                            }
                            targetTenant = *tenantId;
                        }
                        record->createdBy     = session->userId;
                        const std::int64_t id = co_await deps->channels->scoped({targetTenant, false})->create(loop, std::move(*record));
                        Common::writeSuccess(response, ConfigValue::object({{"id", id}}), traceId);
                        co_return;
                    });

        router.put("/api/system/notifications/:id",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "system:notification:edit", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto id   = Common::parsePositiveIdentifier(request.param("id"));
                       const auto body = request.jsonBody();
                       if (!id.has_value() || !body.has_value() || !body->is_object())
                       {
                           fail(response, Common::kCodeBadRequest, "渠道标识或请求正文不合法", traceId);
                           co_return;
                       }
                       const auto scoped   = deps->channels->scoped(session->tenantScope());
                       const auto existing = co_await scoped->find(loop, *id);
                       if (!existing.has_value())
                       {
                           fail(response, Common::kCodeNotFound, "渠道不存在", traceId);
                           co_return;
                       }
                       std::string error;
                       auto        record = parseChannel(*body, *existing, error);
                       if (!record.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, error, traceId);
                           co_return;
                       }
                       if (!co_await scoped->update(loop, *id, std::move(*record)))
                       {
                           fail(response, Common::kCodeNotFound, "渠道不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.del("/api/system/notifications/:id",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "system:notification:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto id = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "渠道标识不合法", traceId);
                           co_return;
                       }
                       if (!co_await deps->channels->scoped(session->tenantScope())->remove(loop, *id))
                       {
                           fail(response, Common::kCodeNotFound, "渠道不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });
    }
} // namespace EdgePulse::Notification
