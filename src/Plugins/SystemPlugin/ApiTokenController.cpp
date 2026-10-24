#include "Plugins/SystemPlugin/ApiTokenController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/RequestParsing.h"
#include "Base/Json/JsonFields.h"
#include "Core/Auth/ApiTokenService.h"
#include "Core/Auth/AuthService.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::System
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;
        using EdgePulse::Auth::ApiTokenCreated;
        using EdgePulse::Auth::ApiTokenInfo;

        void fail(AsynGyanis::Net::HttpResponse &response, const int status, const std::string &message, const std::string_view traceId)
        {
            Common::writeFailure(response, status, status, message, traceId);
        }

        /**
         * @brief 输出令牌摘要；哈希与重叠窗口列不进入响应
         * @param token 服务返回的对外摘要
         * @return ConfigValue 仅含 id、name、prefix、scopes、expiresAt、lastUsedAt、revokedAt、createdAt
         */
        [[nodiscard]] ConfigValue tokenJson(const ApiTokenInfo &token)
        {
            return ConfigValue::object({{"id", token.id},
                                        {"name", token.name},
                                        {"prefix", token.prefix},
                                        {"scopes", Json::stringArray(token.scopes)},
                                        {"expiresAt", token.expiresAt},
                                        {"lastUsedAt", token.lastUsedAt},
                                        {"revokedAt", token.revokedAt},
                                        {"createdAt", token.createdAt}});
        }

        /**
         * @brief 输出创建或轮换结果；明文只在这里出现一次
         * @param created 服务返回的创建产物
         * @return ConfigValue 摘要加 token，轮换时再加 previousValidUntil
         */
        [[nodiscard]] ConfigValue createdJson(const ApiTokenCreated &created)
        {
            ConfigValue payload = tokenJson(created.token);
            payload["token"]    = created.plaintext;
            if (!created.previousValidUntil.empty())
            {
                payload["previousValidUntil"] = created.previousValidUntil;
            }
            return payload;
        }

        /**
         * @brief 严格读取 scopes 字符串数组
         * @param body 请求体对象
         * @return std::optional<std::vector<std::string>> 缺失、非数组或含非字符串元素时为空
         */
        [[nodiscard]] std::optional<std::vector<std::string>> scopesField(const ConfigValue &body)
        {
            const auto iterator = body.find("scopes");
            if (iterator == body.end() || !iterator->is_array())
            {
                return std::nullopt;
            }
            std::vector<std::string> scopes;
            scopes.reserve(iterator->size());
            for (const ConfigValue &item: *iterator)
            {
                if (!item.is_string())
                {
                    return std::nullopt;
                }
                scopes.push_back(item.get<std::string>());
            }
            return scopes;
        }

        /**
         * @brief 把请求体里的有效期收窄到 int，避免超范围数值在转换时回绕
         * @param body 请求体对象
         * @param value 解析结果
         * @return bool 字段缺失或落在 int 范围内为 true
         */
        [[nodiscard]] bool parseExpiresInSeconds(const ConfigValue &body, int &value)
        {
            value = 0;
            if (!body.contains("expiresInSeconds"))
            {
                return true;
            }
            const std::optional<std::int64_t> parsed = Json::optionalInteger(body, "expiresInSeconds");
            if (!parsed.has_value() || *parsed < std::numeric_limits<int>::min() || *parsed > std::numeric_limits<int>::max())
            {
                return false;
            }
            value = static_cast<int>(*parsed);
            return true;
        }
    } // namespace

    void registerApiTokenRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const ApiTokenDependencies &dependencyInput)
    {
        const auto dependencies = std::make_shared<ApiTokenDependencies>(dependencyInput);

        router.get("/api/system/api-tokens",
                   [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*dependencies->auth, loop, request, response, "system:token:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       ConfigValue rows = ConfigValue::array();
                       for (const ApiTokenInfo &token: co_await dependencies->tokens->list(loop, *session))
                       {
                           rows.push_back(tokenJson(token));
                       }
                       Common::writeSuccess(response, std::move(rows), traceId);
                       co_return;
                   });

        router.post("/api/system/api-tokens",
                    [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(*dependencies->auth, loop, request, response, "system:token:add", traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const auto body = request.jsonBody();
                        if (!body.has_value() || !body->is_object())
                        {
                            fail(response, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                            co_return;
                        }
                        std::optional<std::vector<std::string>> scopes = scopesField(*body);
                        if (!scopes.has_value())
                        {
                            fail(response, Common::kCodeBadRequest, "scopes 必须是字符串数组", traceId);
                            co_return;
                        }
                        int expiresInSeconds = 0;
                        if (!parseExpiresInSeconds(*body, expiresInSeconds))
                        {
                            fail(response, Common::kCodeBadRequest, "expiresInSeconds 必须是整数", traceId);
                            co_return;
                        }
                        const auto created = co_await dependencies->tokens->create(loop, *session, Json::stringField(*body, "name"), std::move(*scopes), expiresInSeconds);
                        if (!created.has_value())
                        {
                            fail(response, created.error().httpStatus, created.error().message, traceId);
                            co_return;
                        }
                        // 响应含一次性明文，禁止任何中间层缓存
                        response.setHeader("Cache-Control", "no-store");
                        Common::writeSuccess(response, createdJson(*created), traceId);
                        co_return;
                    });

        router.post("/api/system/api-tokens/:id/rotate",
                    [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(*dependencies->auth, loop, request, response, "system:token:edit", traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const std::optional<std::int64_t> id = Common::parsePositiveIdentifier(request.param("id"));
                        if (!id.has_value())
                        {
                            fail(response, Common::kCodeBadRequest, "令牌标识不合法", traceId);
                            co_return;
                        }
                        const auto rotated = co_await dependencies->tokens->rotate(loop, *session, *id);
                        if (!rotated.has_value())
                        {
                            fail(response, rotated.error().httpStatus, rotated.error().message, traceId);
                            co_return;
                        }
                        // 响应含一次性明文，禁止任何中间层缓存
                        response.setHeader("Cache-Control", "no-store");
                        Common::writeSuccess(response, createdJson(*rotated), traceId);
                        co_return;
                    });

        router.del("/api/system/api-tokens/:id",
                   [&loop, dependencies](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*dependencies->auth, loop, request, response, "system:token:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const std::optional<std::int64_t> id = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "令牌标识不合法", traceId);
                           co_return;
                       }
                       if (!co_await dependencies->tokens->revoke(loop, *session, *id))
                       {
                           fail(response, Common::kCodeNotFound, "令牌不存在或已吊销", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });
    }
} // namespace EdgePulse::System
