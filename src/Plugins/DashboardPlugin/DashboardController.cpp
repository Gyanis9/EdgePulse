#include "Plugins/DashboardPlugin/DashboardController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/QueryParameters.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/DataScopeService.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Core/System/LogRepository.h"
#include "Core/System/RoleRepository.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Dashboard
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;
        using EdgePulse::System::LoginLogRecord;
        using EdgePulse::System::LoginStats;
        using EdgePulse::System::OperStats;
        using EdgePulse::System::RoleRecord;
        using EdgePulse::System::TimeBucket;

        [[nodiscard]] std::string timeText(const std::int64_t epoch)
        {
            return Common::formatDateTimeUtc(std::chrono::system_clock::time_point{std::chrono::seconds{epoch}});
        }

        [[nodiscard]] std::int64_t rangeSeconds(const std::string &range)
        {
            if (range == "7d")
            {
                return 7 * 86400;
            }
            if (range == "30d")
            {
                return 30 * 86400;
            }
            return 86400;
        }

        [[nodiscard]] ConfigValue seriesJson(const std::vector<TimeBucket> &series)
        {
            ConfigValue array = ConfigValue::array();
            for (const TimeBucket &bucket: series)
            {
                array.push_back(ConfigValue::object({{"bucket", bucket.label}, {"total", bucket.total}, {"failed", bucket.failed}}));
            }
            return array;
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
    } // namespace

    void registerDashboardRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const DashboardDependencies &dependencyInput)
    {
        const auto deps = std::make_shared<DashboardDependencies>(dependencyInput);
        router.get("/api/dashboard/summary",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       std::string range = Common::queryText(request.queryParams(), "range");
                       if (range != "7d" && range != "30d")
                       {
                           range = "24h";
                       }
                       const std::int64_t     now      = static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
                       const std::int64_t     length   = rangeSeconds(range);
                       const std::int64_t     start    = now - length;
                       const std::int64_t     previous = start - length;
                       const System::LogScope scope    = co_await resolveLogScope(deps->dataScopes, loop, *session);

                       const LoginStats   loginStats     = co_await deps->logs->loginStats(loop, range, scope);
                       const OperStats    operStats      = co_await deps->logs->operStats(loop, range, scope);
                       const std::int64_t previousLogins = co_await deps->logs->loginCountBetween(loop, timeText(previous), timeText(start), scope);
                       const std::int64_t previousOps    = co_await deps->logs->operCountBetween(loop, timeText(previous), timeText(start), scope);
                       const std::int64_t onlineUsers    = co_await deps->logs->distinctLoginUsersSince(loop, timeText(now - 3600), scope);
                       const auto         roles          = co_await deps->roles->scoped(session->tenantScope())->listOptions(loop);
                       const auto         recent         = co_await deps->logs->recentLogins(loop, 6, scope);
                       const auto         dailyOps       = co_await deps->logs->dailyOper(loop, 7, scope);

                       ConfigValue trend = ConfigValue::array();
                       for (const TimeBucket &bucket: loginStats.series)
                       {
                           trend.push_back(bucket.total);
                       }
                       ConfigValue  stats    = ConfigValue::array();
                       const double apiDelta = previousOps > 0 ? static_cast<double>(operStats.total) * 100.0 / static_cast<double>(previousOps) - 100.0 : 0.0;
                       stats.push_back(ConfigValue::object(
                               {{"key", "online"}, {"label", "在线用户"}, {"value", onlineUsers}, {"delta", 0}, {"up", true}, {"fresh", true}, {"sub", "最近 1 小时登录"}}));
                       stats.push_back(ConfigValue::object({{"key", "api"},
                                                            {"label", "接口调用"},
                                                            {"value", operStats.total},
                                                            {"delta", apiDelta},
                                                            {"up", apiDelta >= 0},
                                                            {"fresh", previousOps == 0},
                                                            {"sub", range == "24h" ? "最近 24 小时操作" : "最近 " + range + " 操作"}}));
                       ConfigValue activities = ConfigValue::array();
                       for (const LoginLogRecord &record: recent)
                       {
                           activities.push_back(ConfigValue::object({{"who", record.username},
                                                                     {"target", record.ip.empty() ? record.message : "来自 " + record.ip},
                                                                     {"time", record.loginTime},
                                                                     {"tint", record.status == 1 ? "ok" : "danger"}}));
                       }
                       std::map<std::string, std::int64_t> scopeCounts;
                       for (const RoleRecord &role: roles)
                       {
                           scopeCounts[role.dataScope] += role.userCount;
                       }
                       ConfigValue roleDist = ConfigValue::array();
                       for (const auto &[dataScopeKey, count]: scopeCounts)
                       {
                           roleDist.push_back(ConfigValue::object({{"label", dataScopeKey}, {"value", count}}));
                       }
                       ConfigValue daily = ConfigValue::array();
                       for (const TimeBucket &bucket: dailyOps)
                       {
                           daily.push_back(ConfigValue::object({{"date", bucket.label}, {"total", bucket.total}, {"failed", bucket.failed}}));
                       }
                       const double successRate =
                               operStats.total > 0 ? static_cast<double>(operStats.total - operStats.failed) * 100.0 / static_cast<double>(operStats.total) : 100.0;

                       Common::writeSuccess(response,
                                            ConfigValue::object({{"stats", std::move(stats)},
                                                                 {"trend", std::move(trend)},
                                                                 {"activities", std::move(activities)},
                                                                 {"roleDist", std::move(roleDist)},
                                                                 {"trendTotal", operStats.total},
                                                                 {"successRate", successRate},
                                                                 {"trendPrev", previousLogins},
                                                                 {"dailyOps", std::move(daily)}}),
                                            traceId);
                       co_return;
                   });
    }
} // namespace EdgePulse::Dashboard
