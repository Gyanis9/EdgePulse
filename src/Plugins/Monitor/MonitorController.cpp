#include "Plugins/Monitor/MonitorController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/CsvWriter.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/Pagination.h"
#include "Base/Api/QueryParameters.h"
#include "Base/Api/RequestParsing.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/DataScopeService.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Core/System/LogRepository.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::Monitor
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;
        using EdgePulse::System::LoginHeatmap;
        using EdgePulse::System::LoginLogRecord;
        using EdgePulse::System::LoginStats;
        using EdgePulse::System::OperLogRecord;
        using EdgePulse::System::OperStats;
        using EdgePulse::System::OperTimeline;

        [[nodiscard]] ConfigValue seriesJson(const std::vector<System::TimeBucket> &series)
        {
            ConfigValue array = ConfigValue::array();
            for (const System::TimeBucket &bucket: series)
            {
                array.push_back(ConfigValue::object({{"bucket", bucket.label}, {"total", bucket.total}, {"failed", bucket.failed}}));
            }
            return array;
        }

        [[nodiscard]] ConfigValue operLogJson(const OperLogRecord &record)
        {
            return ConfigValue::object({{"id", record.id},
                                        {"title", record.title},
                                        {"module", record.module},
                                        {"action", record.action},
                                        {"operName", record.operName},
                                        {"dept", record.deptName},
                                        {"method", record.method},
                                        {"url", record.url},
                                        {"ip", record.ip},
                                        {"params", record.params},
                                        {"result", record.result},
                                        {"errorMsg", record.errorMsg},
                                        {"cost", record.costMs},
                                        {"operTime", record.operTime}});
        }

        [[nodiscard]] ConfigValue loginLogJson(const LoginLogRecord &record)
        {
            return ConfigValue::object({{"id", record.id},
                                        {"username", record.username},
                                        {"status", record.status},
                                        {"msg", record.message},
                                        {"ip", record.ip},
                                        {"location", record.location},
                                        {"browser", record.browser},
                                        {"os", record.os},
                                        {"loginTime", record.loginTime}});
        }

        [[nodiscard]] ConfigValue operStatsJson(const OperStats &stats, const std::string &range)
        {
            const double successRate = stats.total > 0 ? static_cast<double>(stats.total - stats.failed) * 100.0 / static_cast<double>(stats.total) : 100.0;
            return ConfigValue::object({{"range", range},
                                        {"total", stats.total},
                                        {"failed", stats.failed},
                                        {"successRate", successRate},
                                        {"avgCost", stats.avgCost},
                                        {"p95Cost", stats.p95Cost},
                                        {"activeUsers", stats.activeUsers},
                                        {"series", seriesJson(stats.series)}});
        }

        [[nodiscard]] ConfigValue loginStatsJson(const LoginStats &stats, const std::string &range)
        {
            const double successRate = stats.total > 0 ? static_cast<double>(stats.total - stats.failed) * 100.0 / static_cast<double>(stats.total) : 100.0;
            return ConfigValue::object({{"range", range},
                                        {"total", stats.total},
                                        {"failed", stats.failed},
                                        {"successRate", successRate},
                                        {"users", stats.users},
                                        {"ips", stats.ips},
                                        {"series", seriesJson(stats.series)}});
        }

        [[nodiscard]] ConfigValue timelineJson(const OperTimeline &timeline)
        {
            ConfigValue lanes = ConfigValue::array();
            for (const System::TimelineLane &lane: timeline.lanes)
            {
                ConfigValue blocks = ConfigValue::array();
                for (const System::TimelineBlock &block: lane.blocks)
                {
                    ConfigValue ids = ConfigValue::array();
                    for (const std::int64_t id: block.ids)
                    {
                        ids.push_back(id);
                    }
                    blocks.push_back(ConfigValue::object({{"startSec", block.startSec},
                                                          {"endSec", block.endSec},
                                                          {"count", block.count},
                                                          {"failed", block.failed},
                                                          {"maxCost", block.maxCost},
                                                          {"ids", std::move(ids)}}));
                }
                lanes.push_back(ConfigValue::object({{"module", lane.module}, {"count", lane.count}, {"blocks", std::move(blocks)}}));
            }
            ConfigValue result = ConfigValue::object({{"date", timeline.date}, {"lanes", std::move(lanes)}});
            result["nowSec"]   = timeline.nowSec.has_value() ? ConfigValue{*timeline.nowSec} : ConfigValue{};
            return result;
        }

        [[nodiscard]] ConfigValue heatmapJson(const LoginHeatmap &heatmap)
        {
            ConfigValue cells = ConfigValue::array();
            for (const System::HeatCell &cell: heatmap.cells)
            {
                cells.push_back(ConfigValue::object({{"dow", cell.dayOfWeek}, {"hour", cell.hour}, {"total", cell.total}, {"failed", cell.failed}}));
            }
            ConfigValue anomalies = ConfigValue::array();
            for (const System::HeatAnomaly &anomaly: heatmap.anomalies)
            {
                anomalies.push_back(ConfigValue::object({{"kind", anomaly.kind},
                                                         {"key", anomaly.key},
                                                         {"failed", anomaly.failed},
                                                         {"total", anomaly.total},
                                                         {"spread", anomaly.spread},
                                                         {"firstTime", anomaly.firstTime},
                                                         {"lastTime", anomaly.lastTime}}));
            }
            return ConfigValue::object(
                    {{"days", heatmap.days}, {"since", heatmap.since}, {"threshold", heatmap.threshold}, {"cells", std::move(cells)}, {"anomalies", std::move(anomalies)}});
        }

        void writeCsv(AsynGyanis::Net::HttpResponse &response, const Common::CsvWriter &writer, const std::string_view filename, const std::string_view traceId)
        {
            response.setStatus(200);
            response.setHeader("Content-Type", "text/csv; charset=utf-8");
            response.setHeader("Content-Disposition", "attachment; filename=\"" + std::string{filename} + "\"");
            if (!traceId.empty())
            {
                response.setHeader(Common::kRequestIdHeader, traceId);
            }
            response.setOwnedBody(writer.take());
        }

        [[nodiscard]] AsynGyanis::Core::Task<System::LogScope> resolveLogScope(Auth::DataScopeService *dataScopes, AsynGyanis::Core::EventLoop &loop,
                                                                               const EdgePulse::Auth::SessionRecord &session)
        {
            if (dataScopes == nullptr)
            {
                co_return System::LogScope{};
            }
            const EdgePulse::Auth::DataScope scope = co_await dataScopes->resolve(loop, session.userId);
            co_return System::LogScope{scope.all, scope.selfOnly, scope.userId, scope.deptIds, session.tenantId, session.platformAdmin};
        }

        void fail(AsynGyanis::Net::HttpResponse &response, const int status, const std::string &message, const std::string_view traceId)
        {
            Common::writeFailure(response, status, status, message, traceId);
        }
    } // namespace

    void registerMonitorRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const MonitorDependencies &dependencyInput)
    {
        const auto deps   = std::make_shared<MonitorDependencies>(dependencyInput);
        const auto authOf = [deps]() -> Auth::AuthService & { return *deps->auth; };

        router.get("/api/monitor/oper-logs",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "monitor:operlog:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap       query = request.queryParams();
                       const Common::PageParameters page  = Common::parsePageParameters(query);
                       const System::LogScope       scope = co_await resolveLogScope(deps->dataScopes, loop, *session);
                       const auto  result = co_await deps->logs->listOperLogs(loop, Common::queryText(query, "keyword"), Common::queryText(query, "result"), page, scope);
                       ConfigValue rows   = ConfigValue::array();
                       for (const OperLogRecord &record: result.rows)
                       {
                           rows.push_back(operLogJson(record));
                       }
                       Common::writeSuccess(response, Common::makePageResult(page, result.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.get("/api/monitor/oper-logs/stats",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "monitor:operlog:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       std::string range = Common::queryText(request.queryParams(), "range");
                       if (range != "7d" && range != "30d")
                       {
                           range = "24h";
                       }
                       const System::LogScope scope = co_await resolveLogScope(deps->dataScopes, loop, *session);
                       const OperStats        stats = co_await deps->logs->operStats(loop, range, scope);
                       Common::writeSuccess(response, operStatsJson(stats, range), traceId);
                       co_return;
                   });

        router.get("/api/monitor/oper-logs/timeline",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "monitor:operlog:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const System::LogScope scope    = co_await resolveLogScope(deps->dataScopes, loop, *session);
                       const OperTimeline     timeline = co_await deps->logs->operTimeline(loop, Common::queryText(request.queryParams(), "date"), scope);
                       Common::writeSuccess(response, timelineJson(timeline), traceId);
                       co_return;
                   });

        router.get("/api/monitor/oper-logs/export",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "monitor:operlog:export", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap query = request.queryParams();
                       const System::LogScope scope = co_await resolveLogScope(deps->dataScopes, loop, *session);
                       const auto             rows  = co_await deps->logs->exportOperLogs(loop, Common::queryText(query, "keyword"), Common::queryText(query, "result"), scope);
                       Common::CsvWriter      writer;
                       writer.addRow({"日志ID", "模块", "动作", "操作人", "部门", "方法", "请求", "来源", "参数", "结果", "错误", "耗时ms", "操作时间"});
                       for (const OperLogRecord &record: rows)
                       {
                           writer.addRow({std::to_string(record.id), record.module, record.action, record.operName, record.deptName, record.method, record.url, record.ip,
                                          record.params, record.result == 1 ? "成功" : "失败", record.errorMsg, std::to_string(record.costMs), record.operTime});
                       }
                       writeCsv(response, writer, "oper-logs.csv", traceId);
                       co_return;
                   });

        router.del("/api/monitor/oper-logs/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "monitor:operlog:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto id = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "日志标识不合法", traceId);
                           co_return;
                       }
                       const auto scope = co_await resolveLogScope(deps->dataScopes, loop, *session);
                       if (!co_await deps->logs->removeOperLog(loop, *id, scope))
                       {
                           fail(response, Common::kCodeNotFound, "日志不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.del("/api/monitor/oper-logs",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "monitor:operlog:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto scope = co_await resolveLogScope(deps->dataScopes, loop, *session);
                       static_cast<void>(co_await deps->logs->clearOperLogs(loop, scope));
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.get("/api/monitor/login-logs",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "monitor:loginlog:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap       query = request.queryParams();
                       const Common::PageParameters page  = Common::parsePageParameters(query);
                       const System::LogScope       scope = co_await resolveLogScope(deps->dataScopes, loop, *session);
                       const auto  result = co_await deps->logs->listLoginLogs(loop, Common::queryText(query, "keyword"), Common::queryText(query, "status"), page, scope);
                       ConfigValue rows   = ConfigValue::array();
                       for (const LoginLogRecord &record: result.rows)
                       {
                           rows.push_back(loginLogJson(record));
                       }
                       Common::writeSuccess(response, Common::makePageResult(page, result.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.get("/api/monitor/login-logs/stats",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "monitor:loginlog:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       std::string range = Common::queryText(request.queryParams(), "range");
                       if (range != "7d" && range != "30d")
                       {
                           range = "24h";
                       }
                       const System::LogScope scope = co_await resolveLogScope(deps->dataScopes, loop, *session);
                       const LoginStats       stats = co_await deps->logs->loginStats(loop, range, scope);
                       Common::writeSuccess(response, loginStatsJson(stats, range), traceId);
                       co_return;
                   });

        router.get("/api/monitor/login-logs/heatmap",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "monitor:loginlog:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const std::int64_t     days    = Common::queryInteger(request.queryParams(), "days").value_or(7);
                       const System::LogScope scope   = co_await resolveLogScope(deps->dataScopes, loop, *session);
                       const LoginHeatmap     heatmap = co_await deps->logs->loginHeatmap(loop, days == 30 ? 30 : 7, scope);
                       Common::writeSuccess(response, heatmapJson(heatmap), traceId);
                       co_return;
                   });

        router.get("/api/monitor/login-logs/export",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "monitor:loginlog:export", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap query = request.queryParams();
                       const System::LogScope scope = co_await resolveLogScope(deps->dataScopes, loop, *session);
                       const auto             rows  = co_await deps->logs->exportLoginLogs(loop, Common::queryText(query, "keyword"), Common::queryText(query, "status"), scope);
                       Common::CsvWriter      writer;
                       writer.addRow({"日志ID", "登录账号", "结果", "说明", "来源", "归属地", "浏览器", "操作系统", "登录时间"});
                       for (const LoginLogRecord &record: rows)
                       {
                           writer.addRow({std::to_string(record.id), record.username, record.status == 1 ? "成功" : "失败", record.message, record.ip, record.location,
                                          record.browser, record.os, record.loginTime});
                       }
                       writeCsv(response, writer, "login-logs.csv", traceId);
                       co_return;
                   });

        router.del("/api/monitor/login-logs/:id",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "monitor:loginlog:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto id = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "日志标识不合法", traceId);
                           co_return;
                       }
                       const auto scope = co_await resolveLogScope(deps->dataScopes, loop, *session);
                       if (!co_await deps->logs->removeLoginLog(loop, *id, scope))
                       {
                           fail(response, Common::kCodeNotFound, "日志不存在", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });

        router.del("/api/monitor/login-logs",
                   [&loop, deps, authOf](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(authOf(), loop, request, response, "monitor:loginlog:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto scope = co_await resolveLogScope(deps->dataScopes, loop, *session);
                       static_cast<void>(co_await deps->logs->clearLoginLogs(loop, scope));
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });
    }
} // namespace EdgePulse::Monitor
