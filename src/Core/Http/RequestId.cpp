#include "Core/Http/RequestId.h"

#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpRequestId.h"

namespace EdgePulse::Web
{
    std::string resolveRequestId(AsynGyanis::Net::HttpRequest &request)
    {
        static AsynGyanis::Net::HttpRequestIdGenerator generator;
        std::string                                    requestId = generator.resolve(request);
        if (request.requestId().empty())
        {
            request.setRequestId(requestId);
        }
        return requestId;
    }
} // namespace EdgePulse::Web
