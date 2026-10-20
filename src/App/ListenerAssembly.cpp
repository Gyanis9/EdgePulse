#include "App/ListenerAssembly.h"

#include "Base/Config/ConfigValue.h"
#include "Common/ApiResponse.h"
#include "Common/ErrorCatalog.h"
#include "Common/TimeFormat.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"
#include "Web/RequestId.h"

#include <string>

namespace EdgePulse::App
{
    void registerHealthRoutes(AsynGyanis::Net::Router &router)
    {
        router.get("/api/health",
                   [](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       // 健康检查只报存活与服务器时间，不暴露数据库/Redis 的内部状态
                       AsynGyanis::Base::ConfigValue data = AsynGyanis::Base::ConfigValue::object({{"status", "UP"}, {"time", EdgePulse::Common::nowIso8601Utc()}});
                       EdgePulse::Common::writeSuccess(response, std::move(data), EdgePulse::Web::resolveRequestId(request));
                       co_return;
                   });
    }

    void registerFallbackRoute(AsynGyanis::Net::Router &router)
    {
        router.any("*",
                   [](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       // 静态站点接管非 API 路径后，这条兜底只负责给 API 调用方一个统一包络
                       EdgePulse::Common::writeFailure(response, EdgePulse::Common::kCodeNotFound, EdgePulse::Common::kCodeNotFound, "not found",
                                                       EdgePulse::Web::resolveRequestId(request));
                       co_return;
                   });
    }
} // namespace EdgePulse::App
