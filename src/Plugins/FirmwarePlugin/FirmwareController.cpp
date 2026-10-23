#include "Plugins/FirmwarePlugin/FirmwareController.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Api/Pagination.h"
#include "Base/Api/QueryParameters.h"
#include "Base/Api/RequestParsing.h"
#include "Base/Json/JsonFields.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Auth/AuthService.h"
#include "Core/Command/AgentChannelRegistry.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Firmware/FirmwareFileWriter.h"
#include "Core/Firmware/FirmwareRepository.h"
#include "Core/Http/RequestAuth.h"
#include "Core/Http/RequestId.h"
#include "Core/System/TenantRepository.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpRequestBody.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "Platform/IO/MemoryMappedFile.h"

#include <Core/EventLoop/EventLoop.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <exception>
#include <system_error>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::Firmware
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        [[nodiscard]] ConfigValue firmwareJson(const FirmwareRecord &record)
        {
            return ConfigValue::object({{"id", record.id},
                                        {"tenantId", record.tenantId},
                                        {"name", record.name},
                                        {"version", record.version},
                                        {"fileName", record.fileName},
                                        {"contentType", record.contentType},
                                        {"sizeBytes", record.sizeBytes},
                                        {"sha256", record.sha256},
                                        {"createdAt", record.createdAt},
                                        {"createdBy", record.createdBy}});
        }

        [[nodiscard]] ConfigValue taskJson(const FirmwareTaskRecord &task)
        {
            return ConfigValue::object({{"id", task.id},
                                        {"firmwareId", task.firmwareId},
                                        {"nodeId", task.nodeId},
                                        {"nodeName", task.nodeName},
                                        {"status", task.status},
                                        {"progress", task.progress},
                                        {"message", task.message},
                                        {"createdAt", task.createdAt},
                                        {"updatedAt", task.updatedAt}});
        }

        void fail(AsynGyanis::Net::HttpResponse &response, const int status, const std::string &message, const std::string_view traceId)
        {
            Common::writeFailure(response, status, status, message, traceId);
        }

        [[nodiscard]] std::string safeFileName(const std::string &fileName)
        {
            std::string name = std::filesystem::path(fileName).filename().string();
            if (name.empty())
            {
                name = "firmware.bin";
            }
            for (char &character: name)
            {
                const unsigned char code = static_cast<unsigned char>(character);
                if (!(std::isalnum(code) != 0 || character == '.' || character == '-' || character == '_'))
                {
                    character = '_';
                }
            }
            if (name.size() > 200)
            {
                name.resize(200);
            }
            return name;
        }

        struct ByteRange
        {
            std::int64_t start{0};  ///< 起始偏移
            std::int64_t length{0}; ///< 长度
        };

        [[nodiscard]] std::optional<ByteRange> parseRange(const std::string_view header, const std::int64_t size)
        {
            if (!header.starts_with("bytes="))
            {
                return std::nullopt;
            }
            const std::string_view value = header.substr(6);
            if (value.find(',') != std::string_view::npos)
            {
                return std::nullopt;
            }
            const std::size_t dash = value.find('-');
            if (dash == std::string_view::npos)
            {
                return std::nullopt;
            }
            const std::string_view startText = value.substr(0, dash);
            const std::string_view endText   = value.substr(dash + 1);
            std::int64_t           start     = 0;
            std::int64_t           end       = size - 1;
            if (startText.empty())
            {
                // bytes=-N：最后 N 字节
                std::int64_t suffix            = 0;
                const auto [endPointer, error] = std::from_chars(endText.data(), endText.data() + endText.size(), suffix);
                if (error != std::errc{} || endPointer != endText.data() + endText.size() || suffix <= 0)
                {
                    return std::nullopt;
                }
                start = std::max<std::int64_t>(0, size - suffix);
            } else
            {
                const auto [startPointer, startError] = std::from_chars(startText.data(), startText.data() + startText.size(), start);
                if (startError != std::errc{} || startPointer != startText.data() + startText.size() || start < 0 || start >= size)
                {
                    return std::nullopt;
                }
                if (!endText.empty())
                {
                    const auto [endPointer, endError] = std::from_chars(endText.data(), endText.data() + endText.size(), end);
                    if (endError != std::errc{} || endPointer != endText.data() + endText.size() || end < start)
                    {
                        return std::nullopt;
                    }
                    end = std::min(end, size - 1);
                }
            }
            return ByteRange{start, end - start + 1};
        }

        [[nodiscard]] std::string downloadPayload(const FirmwareRecord &record)
        {
            return ConfigValue::object({{"firmwareId", record.id},
                                        {"name", record.name},
                                        {"version", record.version},
                                        {"sha256", record.sha256},
                                        {"sizeBytes", record.sizeBytes},
                                        {"downloadPath", "/api/agent/firmware/" + std::to_string(record.id) + "/download"}})
                    .dump();
        }
    } // namespace

    void registerFirmwareRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const FirmwareDependencies &dependencyInput)
    {
        const auto deps = std::make_shared<FirmwareDependencies>(dependencyInput);

        router.get("/api/firmware",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:firmware:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const Common::QueryMap       query  = request.queryParams();
                       const Common::PageParameters page   = Common::parsePageParameters(query);
                       const auto                   result = co_await deps->firmware->scoped(session->tenantScope())->list(loop, Common::queryText(query, "keyword"), page);
                       ConfigValue                  rows   = ConfigValue::array();
                       for (const FirmwareRecord &record: result.rows)
                       {
                           rows.push_back(firmwareJson(record));
                       }
                       Common::writeSuccess(response, Common::makePageResult(page, result.total, std::move(rows)), traceId);
                       co_return;
                   });

        router.get("/api/firmware/:id/tasks",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:firmware:list", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto id = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "固件标识不合法", traceId);
                           co_return;
                       }
                       const auto  tasks = co_await deps->firmware->scoped(session->tenantScope())->tasks(loop, *id);
                       ConfigValue rows  = ConfigValue::array();
                       for (const FirmwareTaskRecord &task: tasks)
                       {
                           rows.push_back(taskJson(task));
                       }
                       Common::writeSuccess(response, std::move(rows), traceId);
                       co_return;
                   });

        router.postStreaming("/api/firmware",
                             [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                             {
                                 const std::string traceId = Web::resolveRequestId(request);
                                 const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:firmware:upload", traceId);
                                 if (!session.has_value())
                                 {
                                     co_return;
                                 }
                                 const Common::QueryMap query    = request.queryParams();
                                 const std::string      name     = Common::queryText(query, "name");
                                 const std::string      version  = Common::queryText(query, "version");
                                 std::string            fileName = Common::queryText(query, "fileName");
                                 if (fileName.empty())
                                 {
                                     fileName = request.getHeader("x-file-name").value_or("firmware.bin");
                                 }
                                 if (name.empty() || version.empty())
                                 {
                                     fail(response, Common::kCodeBadRequest, "缺少 name 或 version", traceId);
                                     co_return;
                                 }
                                 std::int64_t targetTenantId = session->tenantId;
                                 if (session->platformAdmin)
                                 {
                                     const auto requestedTenant = Common::queryInteger(query, "tenantId");
                                     if (requestedTenant.has_value() && *requestedTenant > 0)
                                     {
                                         if (deps->tenants != nullptr)
                                         {
                                             const auto tenant = co_await deps->tenants->findById(loop, *requestedTenant);
                                             if (!tenant.has_value() || tenant->status != 1)
                                             {
                                                 fail(response, Common::kCodeBadRequest, "目标租户不存在或已停用", traceId);
                                                 co_return;
                                             }
                                         }
                                         targetTenantId = *requestedTenant;
                                     }
                                 }
                                 const auto                        firmware   = deps->firmware->scoped(System::TenantScope{targetTenantId, false});
                                 AsynGyanis::Net::HttpRequestBody *bodyStream = request.bodyStream();
                                 if (bodyStream == nullptr)
                                 {
                                     fail(response, Common::kCodeBadRequest, "当前请求没有可读正文", traceId);
                                     co_return;
                                 }
                                 const std::int64_t                  id        = Common::nextIdentifier();
                                 const std::string                   safeName  = safeFileName(fileName);
                                 const std::filesystem::path         directory = deps->storageDirectory;
                                 const std::string                   finalPath = (directory / (std::to_string(id) + "_" + safeName)).string();
                                 std::shared_ptr<FirmwareFileWriter> writer;
                                 try
                                 {
                                     writer = co_await FirmwareFileWriter::open(loop, finalPath + ".part", finalPath);
                                 } catch (const std::exception &failure)
                                 {
                                     fail(response, Common::kCodeInternalServerError, failure.what(), traceId);
                                     co_return;
                                 }
                                 std::size_t totalBytes = 0;
                                 while (co_await bodyStream->readNext())
                                 {
                                     const std::string_view chunk = bodyStream->chunk();
                                     totalBytes += chunk.size();
                                     if (totalBytes > deps->maximumUploadBytes)
                                     {
                                         writer->abort();
                                         fail(response, 413, "固件超过允许的上传大小", traceId);
                                         co_return;
                                     }
                                     if (!(co_await writer->append(loop, std::string{chunk})))
                                     {
                                         writer->abort();
                                         fail(response, Common::kCodeInternalServerError, "固件写入失败", traceId);
                                         co_return;
                                     }
                                 }
                                 if (bodyStream->isTruncated())
                                 {
                                     writer->abort();
                                     fail(response, Common::kCodeBadRequest, "上传正文不完整", traceId);
                                     co_return;
                                 }
                                 const auto digest = co_await writer->finish(loop);
                                 if (!digest.has_value())
                                 {
                                     writer->abort();
                                     fail(response, Common::kCodeInternalServerError, "固件摘要或落盘失败", traceId);
                                     co_return;
                                 }
                                 FirmwareRecord record;
                                 record.id          = id;
                                 record.name        = name;
                                 record.version     = version;
                                 record.fileName    = safeName;
                                 record.contentType = request.getHeader("content-type").value_or("application/octet-stream");
                                 record.sizeBytes   = static_cast<std::int64_t>(totalBytes);
                                 record.sha256      = *digest;
                                 record.storagePath = finalPath;
                                 record.createdBy   = session->userId;
                                 record.createdAt   = Common::nowDateTimeUtc();
                                 record.tenantId    = targetTenantId;
                                 try
                                 {
                                     static_cast<void>(co_await firmware->create(loop, record));
                                 } catch (const std::exception &failure)
                                 {
                                     std::error_code error;
                                     std::filesystem::remove(finalPath, error);
                                     fail(response, Common::kCodeInternalServerError, failure.what(), traceId);
                                     co_return;
                                 }
                                 Common::writeSuccess(response, firmwareJson(record), traceId);
                                 co_return;
                             });

        router.post("/api/firmware/:id/deploy",
                    [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:firmware:upload", traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const auto id   = Common::parsePositiveIdentifier(request.param("id"));
                        const auto body = request.jsonBody();
                        if (!id.has_value() || !body.has_value() || !body->is_object())
                        {
                            fail(response, Common::kCodeBadRequest, "固件标识或请求正文不合法", traceId);
                            co_return;
                        }
                        const auto firmware = co_await deps->firmware->scoped(session->tenantScope())->find(loop, *id);
                        if (!firmware.has_value())
                        {
                            fail(response, Common::kCodeNotFound, "固件不存在", traceId);
                            co_return;
                        }
                        const auto nodeIds = Json::integerArrayField(*body, "nodeIds");
                        if (nodeIds.empty())
                        {
                            fail(response, Common::kCodeBadRequest, "nodeIds 不能为空", traceId);
                            co_return;
                        }
                        const auto batchSize = Json::optionalInteger(*body, "batchSize");
                        if (batchSize.has_value() && (*batchSize < 1 || *batchSize > 100))
                        {
                            fail(response, Common::kCodeBadRequest, "batchSize 必须在 1 到 100 之间", traceId);
                            co_return;
                        }
                        const std::size_t batchCount   = batchSize.has_value() ? std::min<std::size_t>(static_cast<std::size_t>(*batchSize), nodeIds.size()) : nodeIds.size();
                        const auto        firmwareRepo = deps->firmware->scoped(session->tenantScope());
                        const auto        commandRepo  = deps->commands->scoped(session->tenantScope());
                        ConfigValue       results      = ConfigValue::array();
                        ConfigValue       deferred     = ConfigValue::array();
                        for (std::size_t index = 0; index < nodeIds.size(); ++index)
                        {
                            const std::int64_t nodeId = nodeIds[index];
                            if (index >= batchCount)
                            {
                                deferred.push_back(nodeId);
                                continue;
                            }
                            const std::int64_t previousFirmwareId = co_await firmwareRepo->latestOtherFirmware(loop, nodeId, *id);
                            const std::int64_t taskId             = co_await firmwareRepo->createTask(loop, *id, nodeId, previousFirmwareId);
                            if (taskId == 0)
                            {
                                continue;
                            }
                            const auto command = co_await commandRepo->create(loop, nodeId, "firmware.update", downloadPayload(*firmware), session->userId, 600);
                            if (!command.has_value())
                            {
                                continue;
                            }
                            if (deps->channels != nullptr && deps->channels->isConnected(nodeId))
                            {
                                const ConfigValue commandValue = ConfigValue::object({{"id", command->id},
                                                                                      {"nodeId", command->nodeId},
                                                                                      {"type", command->type},
                                                                                      {"payload", command->payload},
                                                                                      {"timeoutSeconds", command->timeoutSeconds}});
                                const std::string message      = ConfigValue::object({{"type", "command"}, {"command", commandValue}}).dump();
                                if (co_await deps->channels->send(nodeId, message))
                                {
                                    static_cast<void>(co_await commandRepo->markDispatched(loop, command->id));
                                }
                            }
                            if (deps->notifier)
                            {
                                static_cast<void>(co_await deps->notifier(*command));
                            }
                            results.push_back(ConfigValue::object({{"nodeId", nodeId}, {"taskId", taskId}, {"commandId", command->id}}));
                        }
                        if (batchSize.has_value())
                        {
                            Common::writeSuccess(response, ConfigValue::object({{"deployed", std::move(results)}, {"deferredNodeIds", std::move(deferred)}}), traceId);
                            co_return;
                        }
                        Common::writeSuccess(response, std::move(results), traceId);
                        co_return;
                    });

        router.post("/api/firmware/:id/rollback",
                    [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:firmware:upload", traceId);
                        if (!session.has_value())
                        {
                            co_return;
                        }
                        const auto id   = Common::parsePositiveIdentifier(request.param("id"));
                        const auto body = request.jsonBody();
                        if (!id.has_value() || !body.has_value() || !body->is_object())
                        {
                            fail(response, Common::kCodeBadRequest, "固件标识或请求正文不合法", traceId);
                            co_return;
                        }
                        const auto nodeIds = Json::integerArrayField(*body, "nodeIds");
                        if (nodeIds.empty() || nodeIds.size() > 100)
                        {
                            fail(response, Common::kCodeBadRequest, "nodeIds 必须是 1 到 100 个节点", traceId);
                            co_return;
                        }
                        const auto  firmwareRepo = deps->firmware->scoped(session->tenantScope());
                        const auto  commandRepo  = deps->commands->scoped(session->tenantScope());
                        ConfigValue results      = ConfigValue::array();
                        for (const std::int64_t nodeId: nodeIds)
                        {
                            const std::int64_t targetId = co_await firmwareRepo->rollbackSource(loop, *id, nodeId);
                            if (targetId == 0)
                            {
                                continue;
                            }
                            const auto target = co_await firmwareRepo->find(loop, targetId);
                            if (!target.has_value())
                            {
                                continue;
                            }
                            const std::int64_t taskId = co_await firmwareRepo->createTask(loop, targetId, nodeId, *id);
                            if (taskId == 0)
                            {
                                continue;
                            }
                            const auto command = co_await commandRepo->create(loop, nodeId, "firmware.update", downloadPayload(*target), session->userId, 600);
                            if (!command.has_value())
                            {
                                continue;
                            }
                            if (deps->channels != nullptr && deps->channels->isConnected(nodeId))
                            {
                                const ConfigValue commandValue = ConfigValue::object({{"id", command->id},
                                                                                      {"nodeId", command->nodeId},
                                                                                      {"type", command->type},
                                                                                      {"payload", command->payload},
                                                                                      {"timeoutSeconds", command->timeoutSeconds}});
                                const std::string message      = ConfigValue::object({{"type", "command"}, {"command", commandValue}}).dump();
                                if (co_await deps->channels->send(nodeId, message))
                                {
                                    static_cast<void>(co_await commandRepo->markDispatched(loop, command->id));
                                }
                            }
                            if (deps->notifier)
                            {
                                static_cast<void>(co_await deps->notifier(*command));
                            }
                            results.push_back(
                                    ConfigValue::object({{"nodeId", nodeId}, {"fromFirmwareId", *id}, {"toFirmwareId", targetId}, {"taskId", taskId}, {"commandId", command->id}}));
                        }
                        if (results.empty())
                        {
                            fail(response, Common::kCodeNotFound, "没有可回滚的历史版本", traceId);
                            co_return;
                        }
                        Common::writeSuccess(response, std::move(results), traceId);
                        co_return;
                    });

        router.del("/api/firmware/:id",
                   [&loop, deps](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        session = co_await Web::requireSession(*deps->auth, loop, request, response, "device:firmware:remove", traceId);
                       if (!session.has_value())
                       {
                           co_return;
                       }
                       const auto id = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "固件标识不合法", traceId);
                           co_return;
                       }
                       const auto removed = co_await deps->firmware->scoped(session->tenantScope())->remove(loop, *id);
                       if (!removed.has_value())
                       {
                           fail(response, Common::kCodeNotFound, "固件不存在", traceId);
                           co_return;
                       }
                       std::error_code error;
                       std::filesystem::remove(removed->storagePath, error);
                       Common::writeSuccess(response, nullptr, traceId);
                       co_return;
                   });
    }

    void registerFirmwareAgentRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const FirmwareDependencies &dependencyInput)
    {
        const auto deps = std::make_shared<FirmwareDependencies>(dependencyInput);
        // Agent 没有会话：固件下载与任务回写按全租户视图查找，归属由固件自身决定
        const auto allFirmware = deps->firmware->scoped(System::TenantScope{0, true});

        router.get("/api/agent/firmware/:id/download",
                   [&loop, deps, allFirmware](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                   {
                       const std::string traceId = Web::resolveRequestId(request);
                       const auto        id      = Common::parsePositiveIdentifier(request.param("id"));
                       if (!id.has_value())
                       {
                           fail(response, Common::kCodeBadRequest, "固件标识不合法", traceId);
                           co_return;
                       }
                       const auto firmware = co_await allFirmware->find(loop, *id);
                       if (!firmware.has_value())
                       {
                           fail(response, Common::kCodeNotFound, "固件不存在", traceId);
                           co_return;
                       }
                       AsynGyanis::Platform::MemoryMappedFile mapped = AsynGyanis::Platform::MemoryMappedFile::open(firmware->storagePath);
                       if (!mapped.isValid())
                       {
                           fail(response, Common::kCodeNotFound, "固件文件不存在或不可读", traceId);
                           co_return;
                       }
                       const std::int64_t size = static_cast<std::int64_t>(mapped.bytes().size());
                       const std::string  etag = "\"" + firmware->sha256 + "\"";
                       response.setHeader("ETag", etag);
                       response.setHeader("Accept-Ranges", "bytes");
                       response.setHeader("Content-Type", firmware->contentType);
                       response.setHeader("Content-Disposition", "attachment; filename=\"" + firmware->fileName + "\"");
                       if (const auto ifNoneMatch = request.getHeader("if-none-match"); ifNoneMatch.has_value() && ifNoneMatch->find(firmware->sha256) != std::string::npos)
                       {
                           response.setStatus(304);
                           co_return;
                       }
                       std::optional<ByteRange> range;
                       if (const auto rangeHeader = request.getHeader("range"); rangeHeader.has_value())
                       {
                           range = parseRange(*rangeHeader, size);
                           if (!range.has_value())
                           {
                               response.setHeader("Content-Range", "bytes */" + std::to_string(size));
                               fail(response, 416, "Range 不合法或不可满足", traceId);
                               co_return;
                           }
                       }
                       const std::int64_t offset = range.has_value() ? range->start : 0;
                       const std::int64_t length = range.has_value() ? range->length : size;
                       response.setStatus(range.has_value() ? 206 : 200);
                       response.setHeader("Content-Length", std::to_string(length));
                       if (range.has_value())
                       {
                           response.setHeader("Content-Range", "bytes " + std::to_string(offset) + "-" + std::to_string(offset + length - 1) + "/" + std::to_string(size));
                       }
                       response.setMappedBody(std::move(mapped), static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
                       co_return;
                   });

        router.post("/api/agent/firmware/:id/status",
                    [&loop, deps, allFirmware](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                    {
                        const std::string traceId = Web::resolveRequestId(request);
                        const auto        id      = Common::parsePositiveIdentifier(request.param("id"));
                        const auto        body    = request.jsonBody();
                        if (!id.has_value() || !body.has_value() || !body->is_object())
                        {
                            fail(response, Common::kCodeBadRequest, "固件标识或请求正文不合法", traceId);
                            co_return;
                        }
                        const auto        nodeId   = Json::optionalInteger(*body, "nodeId");
                        const std::string status   = Json::stringField(*body, "status");
                        const int         progress = static_cast<int>(std::clamp<std::int64_t>(Json::integerField(*body, "progress", 0), 0, 100));
                        const std::string message  = Json::stringField(*body, "message");
                        if (!nodeId.has_value() || (status != "pending" && status != "downloading" && status != "installing" && status != "installed" && status != "failed"))
                        {
                            fail(response, Common::kCodeBadRequest, "nodeId 或状态不合法", traceId);
                            co_return;
                        }
                        const auto task = co_await allFirmware->updateTask(loop, *id, *nodeId, status, progress, message);
                        if (!task.has_value())
                        {
                            fail(response, Common::kCodeNotFound, "固件任务不存在", traceId);
                            co_return;
                        }
                        Common::writeSuccess(response, taskJson(*task), traceId);
                        co_return;
                    });
    }
} // namespace EdgePulse::Firmware
