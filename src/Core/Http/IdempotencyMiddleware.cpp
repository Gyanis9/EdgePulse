#include "Core/Http/IdempotencyMiddleware.h"

#include "Base/Api/ApiResponse.h"
#include "Base/Api/ErrorCatalog.h"
#include "Base/Log/LogMacros.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/SessionStore.h"
#include "Core/Crypto/Digest.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Http/Authorization.h"
#include "Core/Http/IdempotencyRepository.h"
#include "Core/Http/RequestId.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
        constexpr int                kCompleteAttempts   = 3;                       ///< 响应保存的有界重试次数
        constexpr std::string_view   kFingerprintVersion = "idem-v2";               ///< 指纹格式版本，跨版本缓存一律不重放

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
         * @brief 按 UTF-8 码点计数，而不是按字节
         * @param value 待计数文本
         * @return std::size_t 码点个数；非法字节各算一个
         */
        [[nodiscard]] std::size_t characterCount(const std::string_view value) noexcept
        {
            std::size_t count = 0;
            for (const char raw: value)
            {
                // 续接字节 10xxxxxx 属于上一个码点，只有首字节才计数
                if ((static_cast<unsigned char>(raw) & 0xC0U) != 0x80U)
                {
                    ++count;
                }
            }
            return count;
        }

        /**
         * @brief 判断路径是否整段排除
         * @param path 请求路径
         * @return bool true 表示不进入幂等处理
         * @note 令牌接口会产生只展示一次的明文，不能进缓存；固件上传是流式正文，中间件拿不到完整请求体；
         *       用户与租户管理接口的正文带明文口令，落库会把口令抄进幂等表
         */
        [[nodiscard]] bool isExcludedPath(const std::string_view path) noexcept
        {
            return path.starts_with("/api/auth/") || path.starts_with("/api/agent/") || path == "/api/ws" || path.starts_with("/api/events/") || path == "/api/firmware" ||
                   path.starts_with("/api/system/api-tokens") || path.starts_with("/api/enrollment-tokens") || path.starts_with("/api/system/users") ||
                   path.starts_with("/api/system/tenants");
        }

        /**
         * @brief 判断路径是否落在反向代理前缀下
         * @param prefix 配置的反向代理前缀；空表示不排除
         * @param path 请求路径
         * @return bool true 表示整段放行
         * @note 与代理路由的匹配口径一致：等于前缀，或前缀加斜杠开头，避免 /edge 误伤 /edgecase
         */
        [[nodiscard]] bool isExcludedPrefix(const std::string_view prefix, const std::string_view path) noexcept
        {
            if (prefix.empty())
            {
                return false;
            }
            if (path.size() < prefix.size() || !path.starts_with(prefix))
            {
                return false;
            }
            return path.size() == prefix.size() || path[prefix.size()] == '/';
        }

        /**
         * @brief 把身份权限集合折叠成一个稳定摘要
         * @param session 已认证身份
         * @return std::string 排序后权限码的 SHA-256
         * @note 权限降级后指纹随之变化，同键重放不会再把旧的已授权响应交回
         */
        [[nodiscard]] std::string permissionFingerprint(const Auth::SessionRecord &session)
        {
            std::vector<std::string> sorted = session.permissions;
            std::sort(sorted.begin(), sorted.end());
            std::string material;
            for (const std::string &permission: sorted)
            {
                material += permission;
                material += '\n';
            }
            return AsynGyanis::Core::Digest::sha256Hex(material);
        }

        /**
         * @brief 由方法、完整 URI、表示头、身份权限与正文算出请求指纹
         * @param request 当前请求
         * @param permissionHash 身份权限摘要
         * @return std::string SHA-256 十六进制
         * @note 用 uri() 而不是 path()：查询串参与去重，同键换过滤条件会被判冲突
         */
        [[nodiscard]] std::string requestFingerprint(const AsynGyanis::Net::HttpRequest &request, const std::string_view permissionHash)
        {
            std::string material{kFingerprintVersion};
            material += '\n';
            material += AsynGyanis::Net::methodKeyword(request.method());
            material += '\n';
            material += request.uri();
            material += '\n';
            material += request.getHeader("content-type").value_or("");
            material += '\n';
            material += request.getHeader("content-encoding").value_or("");
            material += '\n';
            material.append(permissionHash);
            material += '\n';
            material.append(request.body());
            return AsynGyanis::Core::Digest::sha256Hex(material);
        }
    } // namespace

    AsynGyanis::Net::MiddlewareFunc idempotencyMiddleware(AsynGyanis::Core::EventLoop &loop, std::shared_ptr<IdempotencyRepository> repository, Auth::AuthService &auth,
                                                          std::string excludedPrefix)
    {
        return [&loop, repository = std::move(repository), &auth, excludedPrefix = std::move(excludedPrefix)](
                       AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response, std::function<AsynGyanis::Core::Task<>()> next) -> AsynGyanis::Core::Task<>
        {
            const std::optional<std::string> token = bearerToken(request);
            const std::optional<std::string> key   = request.getHeader(kIdempotencyKeyHeader);
            if (!isMutatingMethod(request.method()) || isExcludedPath(request.path()) || isExcludedPrefix(excludedPrefix, request.path()) || !token.has_value() || !key.has_value())
            {
                co_await next();
                co_return;
            }

            // 鉴权先于占位：吊销或过期的令牌不能靠缓存里的完成态拿到响应，也不能写出任何行
            std::optional<Auth::SessionRecord> session;
            bool                               authenticationBroken = false;
            try
            {
                session = co_await auth.authenticate(loop, *token);
            } catch (const std::exception &failure)
            {
                // 认证后端故障时不落占位：交给处理器按同一故障给出响应，避免留下无主的 in_progress 行
                LOG_ERROR_FMT("幂等键鉴权失败：{}", failure.what());
                authenticationBroken = true;
            }
            if (authenticationBroken || !session.has_value())
            {
                // 处理器会给出 401；这里既不建占位也不重放
                co_await next();
                co_return;
            }

            const std::size_t keyLength = characterCount(*key);
            if (keyLength < kMinimumKeyLength || keyLength > kMaximumKeyLength)
            {
                Common::writeFailure(response, Common::kCodeBadRequest, Common::kCodeBadRequest, "幂等键长度必须在 8 到 128 个字符之间", resolveRequestId(request));
                co_return;
            }

            const std::string tokenHash      = AsynGyanis::Core::Digest::sha256Hex(*token);
            const std::string keyHash        = AsynGyanis::Core::Digest::sha256Hex(*key);
            const std::string permissionHash = permissionFingerprint(*session);
            const std::string requestHash    = requestFingerprint(request, permissionHash);
            const std::string expiresAt      = Common::formatDateTimeUtc(std::chrono::system_clock::now() + kRetention);

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
                    // 当前在册的写接口只回 JSON 正文，不设置 Location、Set-Cookie 与 ETag；
                    // 头部白名单重放需要新的持久列，留待真正出现这类接口时再补
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
                    co_await repository->abort(loop, tokenHash, keyHash, requestHash);
                } catch (const std::exception &failure)
                {
                    LOG_ERROR_FMT("幂等键占位清理失败：{}", failure.what());
                }
                std::rethrow_exception(handlerFailure);
            }

            const int  status    = response.status();
            const bool mustAbort = status >= Common::kCodeInternalServerError || response.body().size() > kMaximumCachedBody || status == Common::kCodeUnauthorized ||
                                   status == Common::kCodeForbidden;
            if (mustAbort)
            {
                // 5xx、401、403 与超大响应都不该被重放：删掉占位让客户端能真正重试
                try
                {
                    co_await repository->abort(loop, tokenHash, keyHash, requestHash);
                } catch (const std::exception &failure)
                {
                    LOG_ERROR_FMT("幂等键占位清理失败：{}", failure.what());
                }
                co_return;
            }

            // 以响应头里的链路标识为准：处理器可能用自己解析出的标识写包络，与 request.requestId() 不同源
            const std::string traceId =
                    response.getHeader(Common::kRequestIdHeader).value_or(request.requestId().empty() ? resolveRequestId(request) : std::string{request.requestId()});
            for (int attempt = 1; attempt <= kCompleteAttempts; ++attempt)
            {
                bool saved = false;
                try
                {
                    co_await repository->complete(loop, tokenHash, keyHash, requestHash, status, traceId, response.getHeader("content-type").value_or(""),
                                                  std::string{response.body()});
                    saved = true;
                } catch (const std::exception &failure)
                {
                    // 业务已经成功，保存失败只能重试而不是改写状态码：把成功的写操作改成 500 会让客户端重试造出重复数据
                    LOG_WARN_FMT("幂等键响应保存失败（第 {} 次）：{}", attempt, failure.what());
                }
                if (saved)
                {
                    break;
                }
                if (attempt < kCompleteAttempts)
                {
                    AsynGyanis::Core::Timer retryDelay(loop);
                    co_await retryDelay.waitFor(std::chrono::milliseconds{50});
                }
            }
            co_return;
        };
    }
} // namespace EdgePulse::Web
