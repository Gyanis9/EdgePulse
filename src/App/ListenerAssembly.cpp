#include "App/ListenerAssembly.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Http/RequestId.h"
#include "Net/Http/Server/FileSender.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace EdgePulse::App
{
    namespace
    {
        constexpr std::string_view kOpenApiErrorHeader    = "x-openapi-error";
        constexpr std::string_view kOpenApiMissingValue   = "missing";
        constexpr std::string_view kOpenApiMissingMessage = "OpenAPI 契约文件缺失或不可读";

        /**
         * @brief 判断读入的生成物是否是一份完整的 OpenAPI JSON 对象
         * @param content 文件内容
         * @return 语法完整、顶层是对象且含字符串 openapi 与对象 paths 时为 true
         * @note 只检查契约骨架，不替代规范校验；被截断或改坏的生成物按缺失处理，避免把畸形内容当 200 发出去
         */
        [[nodiscard]] bool isCompleteOpenApiDocument(const std::string &content)
        {
            const std::optional<AsynGyanis::Base::ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(content);
            if (!parsed.has_value() || !parsed->is_object())
            {
                return false;
            }
            const auto openapi = parsed->find("openapi");
            const auto paths   = parsed->find("paths");
            return openapi != parsed->end() && openapi->is_string() && paths != parsed->end() && paths->is_object();
        }
    } // namespace

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

    void registerOpenApiRoute(AsynGyanis::Net::Router &router, const std::filesystem::path &webRoot)
    {
        std::shared_ptr<const std::string> document;
        std::ifstream                      stream(webRoot / "openapi.json", std::ios::binary);
        if (stream)
        {
            std::ostringstream buffer;
            buffer << stream.rdbuf();
            const std::string content = buffer.str();
            // 空文件、被截断或语法不完整的生成物按缺失处理；服务启动时解析一次，运行期只发原始字节
            if (isCompleteOpenApiDocument(content))
            {
                document = std::make_shared<const std::string>(content);
            }
        }

        router.get("/api/openapi.json",
                   [document](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       if (!document)
                       {
                           // 与 SPA 兜底的普通 404 区分：调用方据此判断契约文件本身缺失或不可读
                           static_cast<void>(response.setHeader(kOpenApiErrorHeader, kOpenApiMissingValue));
                           Common::writeFailure(response, Common::kCodeNotFound, Common::kCodeNotFound, kOpenApiMissingMessage, traceId);
                           co_return;
                       }
                       response.setStatus(200);
                       response.setHeader("Content-Type", "application/json");
                       // 契约随版本发布更新，浏览器与网关都应每次回源取最新文档
                       response.setHeader("Cache-Control", "no-cache");
                       if (!traceId.empty())
                       {
                           response.setHeader(Common::kRequestIdHeader, traceId);
                       }
                       response.setOwnedBody(*document);
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
