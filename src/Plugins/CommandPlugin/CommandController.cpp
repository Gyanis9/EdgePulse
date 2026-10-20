#include "Plugins/CommandPlugin/CommandController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/Pagination.h"
#include "Base/Api/QueryParameters.h"
#include "Base/Api/RequestParsing.h"
#include "Base/Json/JsonFields.h"
#include "Core/Auth/AuthService.h"
#include "Core/Command/AgentChannelRegistry.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Command
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        [[nodiscard]] ConfigValue commandJson(const CommandRecord &command)
        {
            return ConfigValue::object({{"id", command.id},
                                        {"nodeId", command.nodeId},
                                        {"nodeName", command.nodeName},
                                        {"type", command.type},
                                        {"payload", command.payload},
                                        {"status", command.status},
                                        {"result", command.result},
                                        {"message", command.message},
                                        {"createdAt", command.createdAt},
                                        {"dispatchedAt", command.dispatchedAt},
                                        {"finishedAt", command.finishedAt},
                                        {"timeoutSeconds", command.timeoutSeconds}});
        }

        void fail(AsynGyanis::Net::HttpResponse &response, const int status, const std::string &message, const std::string_view traceId)
        {
            Common::writeFailure(response, status, status, message, traceId);
        }
    } // namespace

    void registerCommandRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const CommandDependencies &dependencyInput)
    {
        const auto deps = std::make_shared<CommandDependencies>(dependencyInput);

        router.get("/api/commands",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:command:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap       query    = request.queryParams();
                       const Common::PageParameters page     = Common::parsePageParameters(query);
                       const auto                   commands = deps->commands->scoped(session->tenantScope());
                       const auto  result = co_await commands->list(loop, Common::queryText(query, "status"), Common::queryInteger(query, "nodeId").value_or(0), page);
                       ConfigValue rows   = ConfigValue::array();
                       for (const CommandRecord &command: result.rows)
                       {
                           rows.push_back(commandJson(command));
                       }
                       Common::writeSuccess(response, Common::makePageResult(page, result.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.post("/api/commands",
                    [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:command:send", traceId);
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
                        const auto        nodeId         = Json::optionalInteger(*body, "nodeId");
                        const std::string type           = Json::stringField(*body, "type");
                        const std::string payload        = Json::stringField(*body, "payload", "{}");
                        const int         timeoutSeconds = static_cast<int>(Json::integerField(*body, "timeoutSeconds", 60));
                        if (!nodeId.has_value() || *nodeId <= 0 || type.empty() || timeoutSeconds < 1 || timeoutSeconds > 3600)
                        {
                            fail(response, Common::kCodeBadRequest, "nodeId、type 或 timeoutSeconds 不合法", traceId);
                            co_return;
                        }
                        const auto commands = deps->commands->scoped(session->tenantScope());
                        auto       created  = co_await commands->create(loop, *nodeId, type, payload, session->userId, timeoutSeconds);
                        if (!created.has_value())
                        {
                            fail(response, Common::kCodeNotFound, "目标节点不存在", traceId);
                            co_return;
                        }
                        if (deps->channels != nullptr && deps->channels->isConnected(*nodeId))
                        {
                            const ConfigValue commandValue = ConfigValue::object({{"id", created->id},
                                                                                  {"nodeId", created->nodeId},
                                                                                  {"type", created->type},
                                                                                  {"payload", created->payload},
                                                                                  {"timeoutSeconds", created->timeoutSeconds}});
                            const std::string message      = ConfigValue::object({{"type", "command"}, {"command", commandValue}}).dump();
                            if (co_await deps->channels->send(*nodeId, message))
                            {
                                if (co_await commands->markDispatched(loop, created->id))
                                {
                                    created = co_await commands->find(loop, created->id);
                                }
                            }
                        }
                        if (deps->notifier)
                        {
                            static_cast<void>(co_await deps->notifier(*created));
                        }
                        Common::writeSuccess(response, commandJson(*created), traceId);
                        co_return;
                    });

        router.put("/api/commands/:id/cancel",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:command:send", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto id = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "命令标识不合法", traceId);
                           co_return;
                       }
                       const auto command = co_await deps->commands->scoped(session->tenantScope())->cancel(loop, *id);
                       if (!command.has_value())
                       {
                           fail(response, Common::kCodeConflict, "命令不存在或已结束", traceId);
                           co_return;
                       }
                       if (deps->notifier)
                       {
                           static_cast<void>(co_await deps->notifier(*command));
                       }
                       Common::writeSuccess(response, commandJson(*command), traceId);
                       co_return;
                   });
    }

    void registerCommandAgentRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const CommandDependencies &dependencyInput)
    {
        const auto deps = std::make_shared<CommandDependencies>(dependencyInput);
        // Agent 没有会话：按全租户视图操作，归属由节点自身决定
        const auto allCommands = deps->commands->scoped(System::TenantScope{0, true});

        router.get("/api/agent/commands/next",
                   [&loop, deps, allCommands](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        nodeId  = Common::queryInteger(request.queryParams(), "nodeId");
                       if (!nodeId.has_value() || *nodeId <= 0)
                       {
                           fail(response, Common::kCodeBadRequest, "缺少 nodeId", traceId);
                           co_return;
                       }
                       const auto command = co_await allCommands->takeNext(loop, *nodeId);
                       if (!command.has_value())
                       {
                           Common::writeSuccess(response, nullptr, traceId);
                           co_return;
                       }
                       if (deps->notifier)
                       {
                           static_cast<void>(co_await deps->notifier(*command));
                       }
                       Common::writeSuccess(response, commandJson(*command), traceId);
                       co_return;
                   });

        router.post("/api/agent/commands/:id/result",
                    [&loop, deps, allCommands](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        id      = Common::parsePositiveIdentifier(request.param("id"));
                        const auto        body    = request.jsonBody();
                        if (!id.has_value() || !body.has_value() || !body->is_object())
                        {
                            fail(response, Common::kCodeBadRequest, "命令标识或请求正文不合法", traceId);
                            co_return;
                        }
                        const auto        nodeId  = Json::optionalInteger(*body, "nodeId");
                        const std::string status  = Json::stringField(*body, "status", "failed");
                        const std::string result  = Json::stringField(*body, "result", "{}");
                        const std::string message = Json::stringField(*body, "message");
                        if (!nodeId.has_value())
                        {
                            fail(response, Common::kCodeBadRequest, "缺少 nodeId", traceId);
                            co_return;
                        }
                        const auto existing = co_await allCommands->find(loop, *id);
                        if (!existing.has_value() || existing->nodeId != *nodeId)
                        {
                            fail(response, Common::kCodeForbidden, "命令不属于该节点", traceId);
                            co_return;
                        }
                        const auto finished = co_await allCommands->finish(loop, *id, status, result, message);
                        if (!finished.has_value())
                        {
                            fail(response, Common::kCodeConflict, "命令不存在或已结束", traceId);
                            co_return;
                        }
                        if (deps->notifier)
                        {
                            static_cast<void>(co_await deps->notifier(*finished));
                        }
                        Common::writeSuccess(response, commandJson(*finished), traceId);
                        co_return;
                    });
    }
} // namespace EdgePulse::Command
