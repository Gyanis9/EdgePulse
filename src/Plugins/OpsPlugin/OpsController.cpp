#include "Plugins/OpsPlugin/OpsController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Core/Auth/AuthService.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Core/Ops/OpsRepository.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <memory>
#include <utility>

namespace EdgePulse::Ops
{
    void registerOpsRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const OpsDependencies &dependencyInput)
    {
        const auto deps = std::make_shared<OpsDependencies>(dependencyInput);

        router.get("/api/ops/summary",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "monitor:operlog:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const OpsSummary                    summary  = co_await deps->repository->scoped(session->tenantScope())->summary(loop);
                       const AsynGyanis::Base::ConfigValue nodes    = AsynGyanis::Base::ConfigValue::object({{"total", summary.nodeTotal}, {"online", summary.nodeOnline}});
                       const AsynGyanis::Base::ConfigValue alarms   = AsynGyanis::Base::ConfigValue::object({{"open", summary.alarmOpen}});
                       const AsynGyanis::Base::ConfigValue commands = AsynGyanis::Base::ConfigValue::object({{"total", summary.commandTotal}, {"pending", summary.commandPending}});
                       Common::writeSuccess(
                               response,
                               AsynGyanis::Base::ConfigValue::object({{"nodes", nodes}, {"alarms", alarms}, {"telemetry24h", summary.telemetry24h}, {"commands", commands}}),
                               traceId);
                       co_return;
                   });

        router.get("/api/ops/protocols",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "monitor:operlog:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       Common::writeSuccess(response,
                                            AsynGyanis::Base::ConfigValue::object({{"http", true},
                                                                                   {"httpPort", deps->httpPort},
                                                                                   {"https", deps->httpsPort != 0},
                                                                                   {"http2", deps->httpsPort != 0},
                                                                                   {"http3", deps->httpsPort != 0},
                                                                                   {"mtls", deps->agentPort != 0},
                                                                                   {"agentPort", deps->agentPort},
                                                                                   {"opsPort", deps->opsPort}}),
                                            traceId);
                       co_return;
                   });
    }
} // namespace EdgePulse::Ops
