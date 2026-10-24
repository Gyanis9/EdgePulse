#include "Core/Http/IdempotencyMiddleware.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Log/LogMacros.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Crypto/Digest.h"
#include "Core/Http/Authorization.h"
#include "Core/Http/IdempotencyRepository.h"
#include "Core/Http/RequestId.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Web
{
    namespace
    {
        constexpr std::string_view   kIdempotencyKeyHeader = "Idempotency-Key";     ///< 客户端给出的幂等键
        constexpr std::string_view   kReplayedHeader       = "Idempotent-Replayed"; ///< 命中重放时置 true
        constexpr std::size_t        kMinimumKeyLength     = 8;                     ///< 幂等键最短字符数
        constexpr std::size_t        kMaximumKeyLength     = 128;                   ///< 幂等键最长字符数
        constexpr std::size_t        kMaximumCachedBody    = 256 * 1024;            ///< 可缓存响应正文上限
        constexpr std::chrono::hours kRetention{24};                                ///< 幂等记录保留时长

        /**
         * @brief 判断方法是否会改变服务端状态
         * @param method 请求方法
         * @return bool true 表示需要幂等保护
         */
        [[nodiscard]] bool isMutatingMethod(const AsynGyanis::Net::HttpMethod method) noexcept
        {
            return method == AsynGyanis::Net::HttpMethod::POST || method == AsynGyanis::Net::HttpMethod::PUT || method == AsynGyanis::Net::HttpMethod::PATCH ||
                   method == AsynGyanis::Net::HttpMethod::DELETE;
        }

        /**
         * @brief 判断路径是否整段排除
         * @param path 请求路径
         * @return bool true 表示不进入幂等处理
         * @note 令牌接口会产生只展示一次的明文，不能进缓存；固件上传是流式正文，中间件拿不到完整请求体
         */
        [[nodiscard]] bool isExcludedPath(const std::string_view path) noexcept
        {
            return path.starts_with("/api/auth/") || path.starts_with("/api/agent/") || path == "/api/ws" || path.starts_with("/api/events/") || path == "/api/firmware" ||
                   path.starts_with("/api/system/api-tokens") || path.starts_with("/api/enrollment-tokens");
        }

        /**
         * @brief 由方法、路径与正文算出请求指纹
         * @param request 当前请求
         * @return std::string SHA-256 十六进制
         */
        [[nodiscard]] std::string requestFingerprint(const AsynGyanis::Net::HttpRequest &request)
        {
            std::string material{AsynGyanis::Net::methodKeyword(request.method())};
            material += '\n';
            material += request.path();
            material += '\n';
            material.append(request.body());
            return AsynGyanis::Core::Digest::sha256Hex(material);
        }
    } // namespace

    AsynGyanis::Net::MiddlewareFunc idempotencyMiddleware(AsynGyanis::Core::EventLoop &loop, std::shared_ptr<IdempotencyRepository> repository)
    {
        return [&loop, repository = std::move(repository)](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response,
                                                           std::function<AsynGyanis::Core::Task<>()> next) -> AsynGyanis::Core::Task<>
        {
            const std::optional<std::string> token = bearerToken(request);
            const std::optional<std::string> key   = request.getHeader(kIdempotencyKeyHeader);
            if (!isMutatingMethod(request.method()) || isExcludedPath(request.path()) || !token.has_value() || !key.has_value())
            {
                co_await next();
                co_return;
            }
            if (key->size() < kMinimumKeyLength || key->size() > kMaximumKeyLength)
            {
                Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "幂等键长度必须在 8 到 128 个字符之间", resolveRequestId(request));
                co_return;
            }

            const std::string tokenHash   = AsynGyanis::Core::Digest::sha256Hex(*token);
            const std::string keyHash     = AsynGyanis::Core::Digest::sha256Hex(*key);
            const std::string requestHash = requestFingerprint(request);
            const std::string expiresAt   = Common::formatDateTimeUtc(std::chrono::system_clock::now() + kRetention);

            IdempotencyBeginResult beginResult;
            try
            {
                beginResult = co_await repository->begin(loop, tokenHash, keyHash, requestHash, std::string{AsynGyanis::Net::methodKeyword(request.method())},
                                                         std::string{request.path()}, expiresAt);
            } catch (const std::exception &failure)
            {
                LOG_ERROR_FMT("幂等键占位失败：{}", failure.what());
                Common::writeFailure(response, Common::kCodeServiceUnavailable, Common::kCodeServiceUnavailable, "幂等键服务暂不可用，请稍后重试", resolveRequestId(request));
                co_return;
            }

            switch (beginResult.state)
            {
                case IdempotencyState::Replay:
                {
                    response.setStatus(beginResult.replay.status);
                    if (!beginResult.replay.contentType.empty())
                    {
                        static_cast<void>(response.setHeader("content-type", beginResult.replay.contentType));
                    }
                    if (!beginResult.replay.body.empty())
                    {
                        response.setBody(beginResult.replay.body);
                    }
                    if (!beginResult.replay.traceId.empty())
                    {
                        static_cast<void>(response.setHeader(Common::kRequestIdHeader, beginResult.replay.traceId));
                    }
                    static_cast<void>(response.setHeader(kReplayedHeader, "true"));
                    co_return;
                }
                case IdempotencyState::Conflict:
                    Common::writeFailure(response, Common::kCodeConflict, Common::kCodeConflict, "幂等键已被不同的请求内容使用", resolveRequestId(request));
                    co_return;
                case IdempotencyState::InProgress:
                    Common::writeFailure(response, Common::kCodeConflict, Common::kCodeConflict, "相同幂等键的请求正在处理中", resolveRequestId(request));
                    co_return;
                case IdempotencyState::Started:
                    break;
            }

            // 处理器异常必须先清掉占位再原样上抛：否则同一键在 TTL 内会一直停在处理中
            std::exception_ptr handlerFailure;
            try
            {
                co_await next();
            } catch (...)
            {
                handlerFailure = std::current_exception();
            }
            if (handlerFailure != nullptr)
            {
                try
                {
                    co_await repository->abort(loop, tokenHash, keyHash);
                } catch (const std::exception &failure)
                {
                    LOG_ERROR_FMT("幂等键占位清理失败：{}", failure.what());
                }
                std::rethrow_exception(handlerFailure);
            }

            const bool mustAbort = response.status() >= Common::kCodeInternalServerError || response.body().size() > kMaximumCachedBody;
            if (mustAbort)
            {
                // 5xx 与超大响应都不该被重放：删掉占位让客户端能真正重试
                try
                {
                    co_await repository->abort(loop, tokenHash, keyHash);
                } catch (const std::exception &failure)
                {
                    LOG_ERROR_FMT("幂等键占位清理失败：{}", failure.what());
                }
                co_return;
            }

            // 以响应头里的链路标识为准：处理器可能用自己解析出的标识写包络，与 request.requestId() 不同源
            const std::string traceId =
                    response.getHeader(Common::kRequestIdHeader).value_or(request.requestId().empty() ? resolveRequestId(request) : std::string{request.requestId()});
            try
            {
                co_await repository->complete(loop, tokenHash, keyHash, response.status(), traceId, response.getHeader("content-type").value_or(""), std::string{response.body()});
            } catch (const std::exception &failure)
            {
                // 业务已经成功，保存失败只能记日志：把成功的写操作改成 500 会让客户端重试造出重复数据
                LOG_ERROR_FMT("幂等键响应保存失败：{}", failure.what());
            }
            co_return;
        };
    }
} // namespace EdgePulse::Web
