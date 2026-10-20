#include "Core/Http/AlarmEventStream.h"

#include "Base/Config/ConfigValue.h"
#include "Core/Auth/AuthService.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Core/Http/SseBroadcaster.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"
#include "Net/Http/SseStream.h"

#include <Core/EventLoop/EventLoop.h>
#include <Core/EventLoop/Timer.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace EdgePulse::Web
{
    void registerAlarmEventStream(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, Auth::AuthService &auth, SseBroadcaster &broadcaster)
    {
        router.get("/api/events/alarms",
                   [&loop, &auth, &broadcaster](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(auth, loop, request, response, "", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       AsynGyanis::Net::SseStream stream(response);
                       if (!stream.isOpen())
                       {
                           co_return;
                       }
                       const std::uint64_t subscription = broadcaster.subscribe(session->tenantId, session->platformAdmin);
                       struct SubscriptionGuard
                       {
                           SseBroadcaster &broadcaster;
                           std::uint64_t   id;
                           ~SubscriptionGuard()
                           {
                               broadcaster.unsubscribe(id);
                           }
                       } guard{broadcaster, subscription};

                       // 先送一条 ready：既让客户端确认订阅已生效，也把响应头立刻刷出去
                       if (!co_await stream.sendEvent("{\"status\":\"ready\"}", "ready"))
                       {
                           co_return;
                       }

                       AsynGyanis::Core::Timer ticker(loop);
                       int                     idleTicks = 0;
                       while (stream.isOpen())
                       {
                           co_await ticker.waitFor(std::chrono::milliseconds{200});
                           for (std::string &event: broadcaster.drain(subscription, 20))
                           {
                               if (!co_await stream.sendEvent(event, "alarm"))
                               {
                                   co_return;
                               }
                           }
                           if (++idleTicks >= 75)
                           {
                               idleTicks = 0;
                               if (!co_await stream.sendComment("keepalive"))
                               {
                                   co_return;
                               }
                           }
                       }
                       co_return;
                   });
    }
} // namespace EdgePulse::Web
