#include "Common/ApiResponse.h"

#include "Common/ErrorCatalog.h"
#include "Net/Http/HttpResponse.h"

#include <string>
#include <utility>

namespace EdgePulse::Common
{
    void writeSuccess(AsynGyanis::Net::HttpResponse &response, AsynGyanis::Base::ConfigValue data, const std::string_view traceId)
    {
        // 四个字段与前端 http.ts 的 ApiResponse 一一对应，data 允许是对象、数组或 null
        AsynGyanis::Base::ConfigValue envelope =
                AsynGyanis::Base::ConfigValue::object({{"code", kCodeOk}, {"message", "ok"}, {"data", std::move(data)}, {"traceId", std::string{traceId}}});
        response.setStatus(kCodeOk);
        // 回写请求标识，前端与网关据此把响应日志与请求日志串起来
        if (!traceId.empty())
        {
            static_cast<void>(response.setHeader(kRequestIdHeader, traceId));
        }
        static_cast<void>(response.setJsonBody(envelope));
    }

    void writeFailure(AsynGyanis::Net::HttpResponse &response, const int httpStatus, const int businessCode, const std::string_view message, const std::string_view traceId)
    {
        // 失败时 data 固定为 null：前端只读 data，包络形状保持一致
        AsynGyanis::Base::ConfigValue envelope =
                AsynGyanis::Base::ConfigValue::object({{"code", businessCode}, {"message", std::string{message}}, {"data", nullptr}, {"traceId", std::string{traceId}}});
        response.setStatus(httpStatus);
        if (!traceId.empty())
        {
            static_cast<void>(response.setHeader(kRequestIdHeader, traceId));
        }
        static_cast<void>(response.setJsonBody(envelope));
    }
} // namespace EdgePulse::Common
