#include "Core/Http/AuditMiddleware.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/SessionStore.h"
#include "Core/Http/Authorization.h"
#include "Core/System/LogRepository.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"

#include <Core/EventLoop/EventLoop.h>

#include <chrono>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Web
{
    namespace
    {
        /**
         * @brief 路径对应的中文模块名
         */
        [[nodiscard]] std::string moduleOf(const std::string_view path)
        {
            if (path.starts_with("/api/system/users"))
            {
                return "用户管理";
            }
            if (path.starts_with("/api/system/roles") || path.starts_with("/api/system/permissions"))
            {
                return "角色权限";
            }
            if (path.starts_with("/api/system/menus"))
            {
                return "菜单管理";
            }
            if (path.starts_with("/api/system/depts"))
            {
                return "部门管理";
            }
            if (path.starts_with("/api/system/posts"))
            {
                return "岗位管理";
            }
            if (path.starts_with("/api/system/dict"))
            {
                return "字典管理";
            }
            if (path.starts_with("/api/system/configs"))
            {
                return "参数配置";
            }
            if (path.starts_with("/api/monitor/"))
            {
                return "监控日志";
            }
            if (path.starts_with("/api/node-groups"))
            {
                return "节点分组";
            }
            if (path.starts_with("/api/nodes"))
            {
                return "节点管理";
            }
            if (path.starts_with("/api/alarm-rules") || path.starts_with("/api/alarms"))
            {
                return "告警中心";
            }
            if (path.starts_with("/api/firmware"))
            {
                return "固件管理";
            }
            if (path.starts_with("/api/commands"))
            {
                return "命令中心";
            }
            return "其他";
        }

        [[nodiscard]] std::string actionOf(const std::string_view method, const std::string_view path)
        {
            if (path.ends_with("/status"))
            {
                return "状态变更";
            }
            if (path.ends_with("/password"))
            {
                return "重置密码";
            }
            if (path.ends_with("/cancel"))
            {
                return "取消";
            }
            if (path.ends_with("/ack"))
            {
                return "确认告警";
            }
            if (path.ends_with("/resolve"))
            {
                return "恢复告警";
            }
            if (method == "POST")
            {
                return "新增";
            }
            if (method == "PUT" || method == "PATCH")
            {
                return "修改";
            }
            if (method == "DELETE")
            {
                return "删除";
            }
            return std::string{method};
        }

        [[nodiscard]] std::string sanitizedParams(const AsynGyanis::Net::HttpRequest &request)
        {
            std::string body{request.body()};
            if (body.size() > 2000)
            {
                body.resize(2000);
                body += "...";
            }
            const auto parsed = AsynGyanis::Base::parseConfigValue(body);
            if (!parsed.has_value() || !parsed->is_object())
            {
                return body;
            }
            AsynGyanis::Base::ConfigValue sanitized = *parsed;
            for (const std::string_view key: {"password", "oldPassword", "newPassword", "token", "captcha", "captchaUuid", "secret"})
            {
                sanitized.erase(std::string{key});
            }
            return sanitized.dump();
        }

        [[nodiscard]] bool shouldAudit(const AsynGyanis::Net::HttpRequest &request)
        {
            const std::string_view method = request.method() == AsynGyanis::Net::HttpMethod::POST     ? "POST"
                                            : request.method() == AsynGyanis::Net::HttpMethod::PUT    ? "PUT"
                                            : request.method() == AsynGyanis::Net::HttpMethod::PATCH  ? "PATCH"
                                            : request.method() == AsynGyanis::Net::HttpMethod::DELETE ? "DELETE"
                                                                                                      : "";
            if (method.empty())
            {
                return false;
            }
            const std::string_view path = request.path();
            // 认证入口不记审计：登录结果由登录日志承担，口令与验证码不应进操作日志
            return !(path.starts_with("/api/auth/") || path.starts_with("/api/agent/") || path == "/api/ws" || path == "/api/events/");
        }
    } // namespace

    void registerAuditMiddleware(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, Auth::AuthService &auth, System::LogRepository &logs)
    {
        router.addMiddleware(
                [&loop, &auth, &logs](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response,
                                      std::function<AsynGyanis::Core::Task<void>()> next) -> AsynGyanis::Core::Task<>
                {
                    if (!shouldAudit(request))
                    {
                        co_await next();
                        co_return;
                    }
                    const auto                         started = std::chrono::steady_clock::now();
                    std::optional<Auth::SessionRecord> session;
                    if (const auto token = bearerToken(request); token.has_value())
                    {
                        session = co_await auth.authenticate(loop, *token);
                    }
                    bool        failed = false;
                    std::string errorMessage;
                    try
                    {
                        co_await next();
                    } catch (const std::exception &failure)
                    {
                        failed       = true;
                        errorMessage = failure.what();
                        throw;
                    } catch (...)
                    {
                        failed       = true;
                        errorMessage = "未识别的异常";
                        throw;
                    }
                    if (!session.has_value())
                    {
                        co_return;
                    }
                    const auto            cost = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                    System::OperLogRecord record;
                    record.module   = moduleOf(request.path());
                    record.action   = actionOf(request.method() == AsynGyanis::Net::HttpMethod::POST    ? "POST"
                                               : request.method() == AsynGyanis::Net::HttpMethod::PUT   ? "PUT"
                                               : request.method() == AsynGyanis::Net::HttpMethod::PATCH ? "PATCH"
                                                                                                        : "DELETE",
                                               request.path());
                    record.title    = record.module + " · " + record.action;
                    record.method   = request.method() == AsynGyanis::Net::HttpMethod::POST    ? "POST"
                                      : request.method() == AsynGyanis::Net::HttpMethod::PUT   ? "PUT"
                                      : request.method() == AsynGyanis::Net::HttpMethod::PATCH ? "PATCH"
                                                                                               : "DELETE";
                    record.url      = std::string{request.path()};
                    record.ip       = request.remoteIp();
                    record.operName = session->username;
                    record.deptName = session->departmentName;
                    record.params   = sanitizedParams(request);
                    record.result   = (failed || response.status() >= 400) ? 0 : 1;
                    record.errorMsg = failed ? errorMessage : (response.status() >= 400 ? "请求失败，HTTP " + std::to_string(response.status()) : "");
                    record.costMs   = cost;
                    record.traceId  = std::string{request.requestId()};
                    try
                    {
                        static_cast<void>(co_await logs.insertOperLog(loop, std::move(record)));
                    } catch (const std::exception &failure)
                    {
                        // 审计写不进去不能反过来改变业务响应，但必须在服务端留下原因
                        LOG_ERROR_FMT("操作审计写入失败：{}", failure.what());
                    }
                    co_return;
                });
    }
} // namespace EdgePulse::Web
