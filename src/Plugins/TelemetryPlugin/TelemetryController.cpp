#include "Plugins/TelemetryPlugin/TelemetryController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/QueryParameters.h"
#include "Base/Api/RequestParsing.h"
#include "Base/Json/JsonFields.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Alarm/AlarmService.h"
#include "Core/Auth/AuthService.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Core/Telemetry/TelemetryRepository.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Telemetry
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        [[nodiscard]] ConfigValue pointJson(const TelemetryPoint &point)
        {
            return ConfigValue::object({{"metric", point.metric}, {"value", point.value}, {"unit", point.unit}, {"collectedAt", point.collectedAt}});
        }

        void fail(AsynGyanis::Net::HttpResponse &response, const int status, const std::string &message, const std::string_view traceId)
        {
            Common::writeFailure(response, status, status, message, traceId);
        }
    } // namespace

    void registerTelemetryBrowserRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const TelemetryDependencies &dependencyInput)
    {
        const auto deps = std::make_shared<TelemetryDependencies>(dependencyInput);

        router.get("/api/telemetry/latest",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:telemetry:view", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto nodeId = Common::queryInteger(request.queryParams(), "nodeId");
                       if (!nodeId.has_value() || *nodeId <= 0)
                       {
                           fail(response, Common::kCodeBadRequest, "缺少 nodeId", traceId);
                           co_return;
                       }
                       if (!(co_await deps->nodes->findById(loop, *nodeId)).has_value())
                       {
                           fail(response, Common::kCodeNotFound, "节点不存在", traceId);
                           co_return;
                       }
                       const auto  points = co_await deps->telemetry->latest(loop, *nodeId);
                       ConfigValue rows   = ConfigValue::array();
                       for (const TelemetryPoint &point: points)
                       {
                           rows.push_back(pointJson(point));
                       }
                       Common::writeSuccess(response, std::move(rows), traceId);
                       co_return;
                   });

        router.get("/api/telemetry/series",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:telemetry:view", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap query  = request.queryParams();
                       const auto             nodeId = Common::queryInteger(query, "nodeId");
                       const std::string      metric = Common::queryText(query, "metric");
                       if (!nodeId.has_value() || *nodeId <= 0 || metric.empty())
                       {
                           fail(response, Common::kCodeBadRequest, "缺少 nodeId 或 metric", traceId);
                           co_return;
                       }
                       std::string from = Common::queryText(query, "from");
                       std::string to   = Common::queryText(query, "to");
                       if (from.empty())
                       {
                           from = Common::formatDateTimeUtc(std::chrono::system_clock::now() - std::chrono::hours{24});
                       }
                       if (to.empty())
                       {
                           to = Common::nowDateTimeUtc();
                       }
                       const auto  points = co_await deps->telemetry->series(loop, *nodeId, metric, from, to, 2000);
                       ConfigValue rows   = ConfigValue::array();
                       for (const TelemetryPoint &point: points)
                       {
                           rows.push_back(pointJson(point));
                       }
                       Common::writeSuccess(response, std::move(rows), traceId);
                       co_return;
                   });
    }

    void registerTelemetryAgentRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const TelemetryDependencies &dependencyInput)
    {
        const auto deps = std::make_shared<TelemetryDependencies>(dependencyInput);

        router.post("/api/agent/telemetry",
                    [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        body    = request.jsonBody();
                        if (!body.has_value() || !body->is_object())
                        {
                            fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                            co_return;
                        }
                        const auto nodeId  = Json::optionalInteger(*body, "nodeId");
                        const auto samples = body->find("samples");
                        if (!nodeId.has_value() || *nodeId <= 0 || samples == body->end() || !samples->is_array() || samples->empty() || samples->size() > 500)
                        {
                            fail(response, Common::kCodeBadRequest, "nodeId 或 samples 不合法（1 到 500 条）", traceId);
                            co_return;
                        }
                        if (!(co_await deps->nodes->findById(loop, *nodeId)).has_value())
                        {
                            fail(response, Common::kCodeNotFound, "节点不存在，请先注册", traceId);
                            co_return;
                        }
                        std::vector<TelemetrySample> parsed;
                        parsed.reserve(samples->size());
                        for (const ConfigValue &sample: *samples)
                        {
                            if (!sample.is_object())
                            {
                                fail(response, Common::kCodeBadRequest, "samples 的每一项必须是对象", traceId);
                                co_return;
                            }
                            TelemetrySample value;
                            value.metric      = Json::stringField(sample, "metric");
                            value.unit        = Json::stringField(sample, "unit");
                            value.labelsJson  = Json::stringField(sample, "labels");
                            value.collectedAt = Json::stringField(sample, "collectedAt");
                            const auto number = sample.find("value");
                            if (value.metric.empty() || value.metric.size() > 64 || number == sample.end() || !number->is_number())
                            {
                                fail(response, Common::kCodeBadRequest, "样本缺少 metric 或数值 value", traceId);
                                co_return;
                            }
                            value.value = number->get<double>();
                            if (!std::isfinite(value.value))
                            {
                                fail(response, Common::kCodeBadRequest, "value 必须是有限数值", traceId);
                                co_return;
                            }
                            parsed.push_back(std::move(value));
                        }
                        const bool stored = co_await deps->telemetry->insertBatch(loop, *nodeId, parsed);
                        if (!stored)
                        {
                            fail(response, Common::kCodeInternalServerError, "遥测写入失败", traceId);
                            co_return;
                        }
                        const auto  alarms    = co_await deps->alarms->evaluate(loop, *nodeId, parsed);
                        ConfigValue alarmRows = ConfigValue::array();
                        for (const Alarm::AlarmRecord &alarm: alarms)
                        {
                            alarmRows.push_back(ConfigValue::object({{"id", alarm.id},
                                                                     {"nodeId", alarm.nodeId},
                                                                     {"level", alarm.level},
                                                                     {"metric", alarm.metric},
                                                                     {"status", alarm.status},
                                                                     {"message", alarm.message}}));
                        }
                        Common::writeSuccess(response, ConfigValue::object({{"accepted", static_cast<std::int64_t>(samples->size())}, {"alarms", std::move(alarmRows)}}), traceId);
                        co_return;
                    });
    }
} // namespace EdgePulse::Telemetry
