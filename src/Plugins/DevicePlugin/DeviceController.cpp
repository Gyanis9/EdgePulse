#include "Plugins/DevicePlugin/DeviceController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/Pagination.h"
#include "Plugins/DevicePlugin/NodeJson.h"
#include "Core/Device/NodeService.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"
#include "Core/Http/RequestId.h"

#include <Core/EventLoop/EventLoop.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::Device
{
    namespace
    {
        constexpr std::string_view kListPermission  = "device:node:list";
        constexpr std::string_view kWritePermission = "device:node:write";

        [[nodiscard]] bool hasPermission(const AccessContext &access, const std::string_view permission)
        {
            return std::find(access.permissions.begin(), access.permissions.end(), permission) != access.permissions.end();
        }

        [[nodiscard]] std::optional<std::int64_t> parseIdentifier(const std::optional<std::string> &text)
        {
            if (!text.has_value() || text->empty())
            {
                return std::nullopt;
            }
            std::int64_t value      = 0;
            const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
            if (error != std::errc{} || end != text->data() + text->size() || value <= 0)
            {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]] std::optional<std::string> stringField(const AsynGyanis::Base::ConfigValue &body, const std::string_view key)
        {
            if (!body.is_object())
            {
                return std::nullopt;
            }
            const auto iterator = body.find(key);
            if (iterator == body.end() || !iterator->is_string())
            {
                return std::nullopt;
            }
            return iterator->get<std::string>();
        }

        /**
         * @brief 把 labels 字段归一成 JSON 文本
         * @return 字段不存在时为空；null 返回空串；其余返回紧凑 JSON
         */
        [[nodiscard]] std::optional<std::string> labelsField(const AsynGyanis::Base::ConfigValue &body)
        {
            if (!body.is_object())
            {
                return std::nullopt;
            }
            const auto iterator = body.find("labels");
            if (iterator == body.end() || iterator->is_null())
            {
                return std::string{};
            }
            if (iterator->is_object())
            {
                return iterator->dump();
            }
            if (iterator->is_string())
            {
                return iterator->get<std::string>();
            }
            return std::nullopt;
        }

        void writeNodeError(AsynGyanis::Net::HttpResponse &response, const NodeServiceError &error, const std::string_view traceId)
        {
            switch (error.reason)
            {
                case NodeServiceError::Reason::Invalid:
                    Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, error.message, traceId);
                    break;
                case NodeServiceError::Reason::NotFound:
                    Common::writeFailure(response, Common::kCodeNotFound, Common::kCodeNotFound, error.message, traceId);
                    break;
                case NodeServiceError::Reason::Conflict:
                    Common::writeFailure(response, Common::kCodeConflict, Common::kCodeConflict, error.message, traceId);
                    break;
            }
        }

        /**
         * @brief 解析身份与权限；失败时已经写好响应，调用方直接返回
         */
        [[nodiscard]] bool requirePermission(const AccessResolver &resolver, AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response,
                                             const std::string_view permission, const std::string_view traceId)
        {
            const std::optional<AccessContext> access = resolver(request);
            // 未认证与无权限是两件事：401 让前端回登录页，403 让前端提示无权访问
            if (!access.has_value())
            {
                Common::writeFailure(response, Common::kCodeUnauthorized, Common::kCodeUnauthorized, "未登录或会话已过期", traceId);
                return false;
            }
            if (!hasPermission(*access, permission))
            {
                Common::writeFailure(response, Common::kCodeForbidden, Common::kCodeForbidden, "无权执行该操作", traceId);
                return false;
            }
            return true;
        }
    } // namespace

    void registerRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, NodeService &nodeService, AccessResolver accessResolver)
    {
        router.get("/api/nodes",
                   [&loop, &nodeService, accessResolver](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       if (!requirePermission(accessResolver, request, response, kListPermission, traceId))
                       {
                           co_return;
                       }
                       const std::unordered_map<std::string, std::string> query   = request.queryParams();
                       const std::string                                  keyword = query.contains("keyword") ? query.at("keyword") : std::string{};
                       const std::string                                  status  = query.contains("status") ? query.at("status") : std::string{};
                       const NodePage                                     page    = co_await nodeService.list(loop, Common::parsePageParameters(query), keyword, status);
                       AsynGyanis::Base::ConfigValue                      rows    = AsynGyanis::Base::ConfigValue::array();
                       for (const NodeRecord &node: page.rows)
                       {
                           rows.push_back(nodeToJson(node));
                       }
                       const Common::PageParameters parameters = Common::parsePageParameters(query);
                       Common::writeSuccess(response, Common::makePageResult(parameters, page.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.post("/api/nodes",
                    [&loop, &nodeService, accessResolver](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        if (!requirePermission(accessResolver, request, response, kWritePermission, traceId))
                        {
                            co_return;
                        }
                        const std::optional<AsynGyanis::Base::ConfigValue> body = request.jsonBody();
                        if (!body.has_value())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                            co_return;
                        }
                        const std::optional<std::string> name   = stringField(*body, "name");
                        const std::optional<std::string> labels = labelsField(*body);
                        if (!name.has_value() || !labels.has_value())
                        {
                            Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少 name，或 labels 既不是对象也不是字符串", traceId);
                            co_return;
                        }
                        const auto result = co_await nodeService.create(loop, *name, *labels);
                        if (!result.has_value())
                        {
                            writeNodeError(response, result.error(), traceId);
                            co_return;
                        }
                        Common::writeSuccess(response, nodeToJson(*result), traceId);
                        co_return;
                    });

        router.put("/api/nodes/:id",
                   [&loop, &nodeService, accessResolver](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       if (!requirePermission(accessResolver, request, response, kWritePermission, traceId))
                       {
                           co_return;
                       }
                       const std::optional<std::int64_t> nodeId = parseIdentifier(request.param("id"));
                       if (!nodeId.has_value())
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "节点标识不合法", traceId);
                           co_return;
                       }
                       const std::optional<AsynGyanis::Base::ConfigValue> body = request.jsonBody();
                       if (!body.has_value())
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "请求正文必须是 JSON 对象", traceId);
                           co_return;
                       }
                       const auto versionIterator = body->is_object() ? body->find("version") : body->end();
                       if (versionIterator == body->end() || !versionIterator->is_number_integer())
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "缺少整数 version", traceId);
                           co_return;
                       }
                       const std::int64_t         version = versionIterator->get<std::int64_t>();
                       std::optional<std::string> name    = stringField(*body, "name");
                       std::optional<std::string> labels;
                       if (body->is_object() && body->contains("labels"))
                       {
                           labels = labelsField(*body);
                           // 字段存在但既不是对象也不是字符串：不能当成“没改”继续更新
                           if (!labels.has_value())
                           {
                               Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "labels 必须是对象或 JSON 字符串", traceId);
                               co_return;
                           }
                       }
                       if (!name.has_value() && !labels.has_value())
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "name 与 labels 至少给一个", traceId);
                           co_return;
                       }
                       const auto result = co_await nodeService.update(loop, *nodeId, std::move(name), labels, version);
                       if (!result.has_value())
                       {
                           writeNodeError(response, result.error(), traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nodeToJson(*result), traceId);
                       co_return;
                   });

        router.del("/api/nodes/:id",
                   [&loop, &nodeService, accessResolver](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       if (!requirePermission(accessResolver, request, response, kWritePermission, traceId))
                       {
                           co_return;
                       }
                       const std::optional<std::int64_t> nodeId = parseIdentifier(request.param("id"));
                       if (!nodeId.has_value())
                       {
                           Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "节点标识不合法", traceId);
                           co_return;
                       }
                       if (!co_await nodeService.remove(loop, *nodeId))
                       {
                           Common::writeFailure(response, Common::kCodeNotFound, Common::kCodeNotFound, "节点不存在或已删除", traceId);
                           co_return;
                       }
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });
    }
} // namespace EdgePulse::Device
