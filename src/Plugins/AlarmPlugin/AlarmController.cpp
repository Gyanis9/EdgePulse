#include "Plugins/AlarmPlugin/AlarmController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/Pagination.h"
#include "Base/Api/QueryParameters.h"
#include "Base/Api/RequestParsing.h"
#include "Base/Json/JsonFields.h"
#include "Core/Alarm/AlarmService.h"
#include "Core/Auth/AuthService.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Alarm
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        [[nodiscard]] ConfigValue alarmJson(const AlarmRecord &alarm)
        {
            return ConfigValue::object({{"id", alarm.id},
                                        {"ruleId", alarm.ruleId},
                                        {"nodeId", alarm.nodeId},
                                        {"nodeName", alarm.nodeName},
                                        {"level", alarm.level},
                                        {"metric", alarm.metric},
                                        {"value", alarm.value},
                                        {"threshold", alarm.threshold},
                                        {"status", alarm.status},
                                        {"message", alarm.message},
                                        {"triggeredAt", alarm.triggeredAt},
                                        {"acknowledgedAt", alarm.acknowledgedAt},
                                        {"resolvedAt", alarm.resolvedAt}});
        }

        [[nodiscard]] ConfigValue ruleJson(const AlarmRuleRecord &rule)
        {
            return ConfigValue::object({{"id", rule.id},
                                        {"name", rule.name},
                                        {"nodeId", rule.nodeId},
                                        {"metric", rule.metric},
                                        {"comparison", rule.comparison},
                                        {"threshold", rule.threshold},
                                        {"level", rule.level},
                                        {"enabled", rule.enabled},
                                        {"cooldownSeconds", rule.cooldownSeconds},
                                        {"createTime", rule.createTime}});
        }

        [[nodiscard]] bool isValidComparison(const std::string_view comparison)
        {
            return comparison == ">" || comparison == ">=" || comparison == "<" || comparison == "<=" || comparison == "==";
        }

        [[nodiscard]] bool isValidLevel(const std::string_view level)
        {
            return level == "info" || level == "warning" || level == "critical";
        }

        void fail(AsynGyanis::Net::HttpResponse &response, const int status, const std::string &message, const std::string_view traceId)
        {
            Common::writeFailure(response, status, status, message, traceId);
        }
    } // namespace

    void registerAlarmRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const AlarmDependencies &dependencyInput)
    {
        const auto deps = std::make_shared<AlarmDependencies>(dependencyInput);

        router.get("/api/alarms",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:alarm:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto alarms = deps->alarms->scoped(session->tenantScope());
                       const Common::QueryMap       query = request.queryParams();
                       const Common::PageParameters page  = Common::parsePageParameters(query);
                       const auto  result = co_await alarms->repository().listAlarms(loop, Common::queryText(query, "status"), Common::queryText(query, "level"),
                                                                                           Common::queryInteger(query, "nodeId").value_or(0), page);
                       ConfigValue rows   = ConfigValue::array();
                       for (const AlarmRecord &alarm: result.rows)
                       {
                           rows.push_back(alarmJson(alarm));
                       }
                       ConfigValue data  = Common::makePageResult(page, result.total, std::move(rows));
                       data["openCount"] = co_await alarms->repository().openCount(loop);
                       Common::writeSuccess(response, std::move(data), traceId);
                       co_return;
                   });

        router.get("/api/alarms/:id",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:alarm:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto alarms = deps->alarms->scoped(session->tenantScope());
                       const auto id = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "告警标识不合法", traceId);
                           co_return;
                       }
                       const auto alarm = co_await alarms->repository().findAlarm(loop, *id);
                       if (!alarm.has_value())
                       {
                           fail(response, Common::kCodeNotFound, "告警不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, alarmJson(*alarm), traceId);
                       co_return;
                   });

        router.put("/api/alarms/:id/ack",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:alarm:handle", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto alarms = deps->alarms->scoped(session->tenantScope());
                       const auto id = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "告警标识不合法", traceId);
                           co_return;
                       }
                       const auto alarm = co_await alarms->repository().acknowledge(loop, *id, session->userId);
                       if (!alarm.has_value())
                       {
                           fail(response, Common::kCodeConflict, "告警不存在或已确认/恢复", traceId);
                           co_return;
                       }
                       co_await alarms->notify(*alarm);
                       Common::writeSuccess(response, alarmJson(*alarm), traceId);
                       co_return;
                   });

        router.put("/api/alarms/:id/resolve",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:alarm:handle", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto alarms = deps->alarms->scoped(session->tenantScope());
                       const auto id = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "告警标识不合法", traceId);
                           co_return;
                       }
                       const auto alarm = co_await alarms->repository().resolve(loop, *id, session->userId);
                       if (!alarm.has_value())
                       {
                           fail(response, Common::kCodeConflict, "告警不存在或已恢复", traceId);
                           co_return;
                       }
                       co_await alarms->notify(*alarm);
                       Common::writeSuccess(response, alarmJson(*alarm), traceId);
                       co_return;
                   });

        router.get("/api/alarm-rules",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:alarm:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto alarms = deps->alarms->scoped(session->tenantScope());
                       const Common::QueryMap       query  = request.queryParams();
                       const Common::PageParameters page   = Common::parsePageParameters(query);
                       const auto                   result = co_await alarms->repository().listRules(loop, Common::queryText(query, "keyword"), page);
                       ConfigValue                  rows   = ConfigValue::array();
                       for (const AlarmRuleRecord &rule: result.rows)
                       {
                           rows.push_back(ruleJson(rule));
                       }
                       Common::writeSuccess(response, Common::makePageResult(page, result.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.post("/api/alarm-rules",
                    [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:alarm:add", traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const auto alarms = deps->alarms->scoped(session->tenantScope());
                        const auto body = request.jsonBody();
                        if (!body.has_value() || !body->is_object())
                        {
                            fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                            co_return;
                        }
                        AlarmRuleRecord rule;
                        rule.name            = Json::stringField(*body, "name");
                        rule.nodeId          = Json::integerField(*body, "nodeId", 0);
                        rule.metric          = Json::stringField(*body, "metric");
                        rule.comparison      = Json::stringField(*body, "comparison", ">");
                        rule.level           = Json::stringField(*body, "level", "warning");
                        rule.enabled         = Json::boolField(*body, "enabled", true);
                        rule.cooldownSeconds = static_cast<int>(Json::integerField(*body, "cooldownSeconds", 300));
                        const auto threshold = body->find("threshold");
                        if (rule.name.empty() || rule.metric.empty() || threshold == body->end() || !threshold->is_number() || !isValidComparison(rule.comparison) ||
                            !isValidLevel(rule.level) || rule.cooldownSeconds < 0)
                        {
                            fail(response, Common::kCodeBadRequest, "规则名称、指标、阈值、比较符或级别不合法", traceId);
                            co_return;
                        }
                        rule.threshold = threshold->get<double>();
                        if (!std::isfinite(rule.threshold))
                        {
                            fail(response, Common::kCodeBadRequest, "阈值必须是有限数值", traceId);
                            co_return;
                        }
                        const std::int64_t id = co_await alarms->repository().createRule(loop, rule, session->userId);
                        Common::writeSuccess(response, ConfigValue::object({{"id", id}}), traceId);
                        co_return;
                    });

        router.put("/api/alarm-rules/:id",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:alarm:edit", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto alarms = deps->alarms->scoped(session->tenantScope());
                       const auto id = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "规则标识不合法", traceId);
                           co_return;
                       }
                       const auto existing = co_await alarms->repository().findRule(loop, *id);
                       if (!existing.has_value())
                       {
                           fail(response, Common::kCodeNotFound, "规则不存在", traceId);
                           co_return;
                       }
                       const auto body = request.jsonBody();
                       if (!body.has_value() || !body->is_object())
                       {
                           fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                           co_return;
                       }
                       AlarmRuleRecord rule = *existing;
                       rule.name            = Json::stringField(*body, "name", rule.name);
                       rule.nodeId          = Json::integerField(*body, "nodeId", rule.nodeId);
                       rule.metric          = Json::stringField(*body, "metric", rule.metric);
                       rule.comparison      = Json::stringField(*body, "comparison", rule.comparison);
                       rule.level           = Json::stringField(*body, "level", rule.level);
                       rule.enabled         = Json::boolField(*body, "enabled", rule.enabled);
                       rule.cooldownSeconds = static_cast<int>(Json::integerField(*body, "cooldownSeconds", rule.cooldownSeconds));
                       if (const auto threshold = body->find("threshold"); threshold != body->end() && threshold->is_number())
                       {
                           rule.threshold = threshold->get<double>();
                       }
                       if (!isValidComparison(rule.comparison) || !isValidLevel(rule.level) || rule.cooldownSeconds < 0 || !std::isfinite(rule.threshold))
                       {
                           fail(response, Common::kCodeBadRequest, "规则字段不合法", traceId);
                           co_return;
                       }
                       if (!co_await alarms->repository().updateRule(loop, *id, rule, session->userId))
                       {
                           fail(response, Common::kCodeNotFound, "规则不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.del("/api/alarm-rules/:id",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:alarm:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto alarms = deps->alarms->scoped(session->tenantScope());
                       const auto id = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "规则标识不合法", traceId);
                           co_return;
                       }
                       if (!co_await alarms->repository().removeRule(loop, *id))
                       {
                           fail(response, Common::kCodeNotFound, "规则不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });
    }
} // namespace EdgePulse::Alarm
