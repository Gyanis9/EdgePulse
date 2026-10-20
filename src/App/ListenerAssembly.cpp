#include "App/ListenerAssembly.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Http/RequestId.h"
#include "Net/Http/FileSender.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

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

    void registerSiteFallback(AsynGyanis::Net::Router &router, const std::filesystem::path &webRoot)
    {
        std::error_code                    error;
        const std::filesystem::path        canonicalRoot = std::filesystem::weakly_canonical(webRoot, error);
        std::shared_ptr<const std::string> index;
        if (!error)
        {
            std::ifstream stream(canonicalRoot / "index.html", std::ios::binary);
            if (stream)
            {
                std::ostringstream buffer;
                buffer << stream.rdbuf();
                index = std::make_shared<const std::string>(buffer.str());
            }
        }

        router.any("*",
                   [canonicalRoot, index](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string      traceId = Web::resolveRequestId(request);
                       const std::string_view path    = request.path();
                       if (path == "/api" || path.starts_with("/api/"))
                       {
                           Common::writeFailure(response, Common::kCodeNotFound, Common::kCodeNotFound, "not found", traceId);
                           co_return;
                       }
                       if (index && (request.method() == AsynGyanis::Net::HttpMethod::GET || request.method() == AsynGyanis::Net::HttpMethod::HEAD))
                       {
                           std::string relative{path};
                           while (!relative.empty() && relative.front() == '/')
                           {
                               relative.erase(relative.begin());
                           }
                           // 只接受普通相对路径：反斜杠、NUL 与父目录段一律不参与拼路径
                           const bool pathIsSafe = relative.find('\\') == std::string::npos && relative.find('\0') == std::string::npos && relative.find("..") == std::string::npos;
                           if (!relative.empty() && pathIsSafe)
                           {
                               std::error_code             fileError;
                               const std::filesystem::path candidate  = std::filesystem::weakly_canonical(canonicalRoot / relative, fileError);
                               const std::filesystem::path insideRoot = candidate.lexically_relative(canonicalRoot);
                               // 符号链接可能把弱规范路径带出根目录；相对段以 .. 开头时拒绝
                               const bool staysInside = !insideRoot.empty() && insideRoot.generic_string().rfind("..", 0) != 0;
                               if (!fileError && staysInside && std::filesystem::is_regular_file(candidate, fileError))
                               {
                                   std::ifstream stream(candidate, std::ios::binary);
                                   if (stream)
                                   {
                                       std::ostringstream buffer;
                                       buffer << stream.rdbuf();
                                       response.setStatus(200);
                                       response.setHeader("Content-Type", AsynGyanis::Net::FileSender::contentTypeForFile(candidate.string()));
                                       response.setHeader("Cache-Control", relative.starts_with("assets/") ? "public, max-age=31536000, immutable" : "no-cache");
                                       response.setOwnedBody(buffer.str());
                                       if (!traceId.empty())
                                       {
                                           response.setHeader(Common::kRequestIdHeader, traceId);
                                       }
                                       co_return;
                                   }
                               }
                           }
                           response.setStatus(200);
                           response.setHeader("Content-Type", "text/html; charset=utf-8");
                           response.setHeader("Cache-Control", "no-cache");
                           response.setOwnedBody(*index);
                           co_return;
                       }
                       Common::writeFailure(response, Common::kCodeNotFound, Common::kCodeNotFound, "not found", traceId);
                       co_return;
                   });
    }
} // namespace EdgePulse::App
