#include "AgentClient.h"

#include "TelemetryCollector.h"

#include "Base/Config/ConfigValue.h"

#include "Core/Tls/TlsPolicy.h"
#include "Net/Http/Client/HttpClient.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Agent
{
    namespace
    {
        constexpr std::chrono::seconds kFirmwareDownloadTimeout{300}; ///< 固件正文的下载时限

        /**
         * @brief 从失败响应里取出统一包络的中文 message
         * @param body 响应正文
         * @return std::string 服务端说明；无法解析时给固定文本
         */
        [[nodiscard]] std::string envelopeMessage(const std::string_view body)
        {
            const std::optional<AsynGyanis::Base::ConfigValue> envelope = AsynGyanis::Base::parseConfigValue(body);
            if (!envelope.has_value() || !envelope->is_object())
            {
                return "服务端未给出可解析的原因";
            }
            const auto message = envelope->find("message");
            if (message == envelope->end() || !message->is_string())
            {
                return "服务端未给出原因";
            }
            return message->get<std::string>();
        }

        /**
         * @brief 判断 HTTP 状态码是否值得退避重试
         * @param status HTTP 状态码
         * @return true 表示服务端暂时不可用或并发冲突
         */
        [[nodiscard]] bool isRetryableStatus(const int status)
        {
            return status == 408 || status == 409 || status == 425 || status == 429 || status >= 500;
        }

        /**
         * @brief 判断命令结果上报的状态码是否值得重试
         * @param status HTTP 状态码
         * @return true 表示服务端可能稍后接受该结果
         * @details 409、403 等终态响应表示命令已结束或归属其他节点，重试同一行只会永久阻塞队列
         */
        [[nodiscard]] bool isResultRetryableStatus(const int status)
        {
            return status == 408 || status == 425 || status == 429 || status >= 500;
        }

        /**
         * @brief 判断响应是否带成功包络
         * @param body 响应正文
         * @return 成功时为空；失败返回中文原因
         */
        [[nodiscard]] std::expected<void, std::string> checkSuccessEnvelope(const std::string_view body)
        {
            const std::optional<AsynGyanis::Base::ConfigValue> envelope = AsynGyanis::Base::parseConfigValue(body);
            if (!envelope.has_value() || !envelope->is_object())
            {
                return std::unexpected("响应不是 JSON 对象");
            }
            const auto code = envelope->find("code");
            if (code != envelope->end() && code->is_number_integer() && code->get<std::int64_t>() != 200)
            {
                return std::unexpected("响应业务码不是成功值：" + envelopeMessage(body));
            }
            return {};
        }

        /**
         * @brief 读统一包络里的 data 对象
         * @param body 响应正文
         * @return 成功返回 data 视图；缺失、不是对象或业务码失败时返回中文原因
         */
        [[nodiscard]] std::expected<AsynGyanis::Base::ConfigValue, std::string> successData(const std::string_view body)
        {
            const auto checked = checkSuccessEnvelope(body);
            if (!checked)
            {
                return std::unexpected(checked.error());
            }
            const auto envelope = AsynGyanis::Base::parseConfigValue(body);
            if (!envelope.has_value() || !envelope->is_object())
            {
                return std::unexpected("响应不是 JSON 对象");
            }
            const auto data = envelope->find("data");
            if (data == envelope->end() || !data->is_object())
            {
                return std::unexpected("响应缺少 data 对象");
            }
            return *data;
        }

        /**
         * @brief 统一拼出失败响应文案
         * @param action 动作名，写进中文原因
         * @param status HTTP 状态码
         * @param body 响应正文
         * @return std::string 中文原因
         */
        [[nodiscard]] std::string httpFailure(const std::string_view action, const int status, const std::string_view body)
        {
            return std::string{action} + "返回 HTTP " + std::to_string(status) + "：" + envelopeMessage(body);
        }

        /**
         * @brief 取 ASCII 小写副本
         * @param text 原文本
         * @return std::string 小写副本，非 ASCII 字节原样保留
         */
        [[nodiscard]] std::string asciiLower(const std::string_view text)
        {
            std::string lowered;
            lowered.reserve(text.size());
            for (const char character: text)
            {
                lowered.push_back(character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character);
            }
            return lowered;
        }

        /**
         * @brief 把命令给出的下载地址解析成绝对地址
         * @param downloadPath 服务端给出的下载地址
         * @param baseUrl 去掉结尾斜杠的服务端基地址
         * @param allowPlaintext 是否允许明文 http 绝对地址
         * @return 成功返回绝对地址；失败返回中文原因
         * @details 以 / 开头时相对 base_url 拼接；带协议的绝对地址只接受 https，http 仅在显式允许明文时放行
         */
        [[nodiscard]] std::expected<std::string, std::string> resolveDownloadUrl(const std::string_view downloadPath, const std::string &baseUrl, const bool allowPlaintext)
        {
            if (downloadPath.empty())
            {
                return std::unexpected("固件下载地址为空");
            }
            if (downloadPath.front() == '/')
            {
                return std::string{baseUrl} + std::string{downloadPath};
            }
            const std::size_t schemeEnd = downloadPath.find("://");
            if (schemeEnd == std::string_view::npos)
            {
                return std::unexpected("固件下载地址必须以 / 开头或为绝对 http(s) 地址");
            }
            const std::string scheme = asciiLower(downloadPath.substr(0, schemeEnd));
            if (scheme == "https")
            {
                return std::string{downloadPath};
            }
            if (scheme == "http")
            {
                if (!allowPlaintext)
                {
                    return std::unexpected("固件下载地址是明文 http，只有显式开启明文测试配置时才允许");
                }
                return std::string{downloadPath};
            }
            return std::unexpected("固件下载地址的协议 " + scheme + " 不受支持，只允许 https 或相对路径");
        }
    } // namespace

    AgentClient::AgentClient(AsynGyanis::Core::EventLoop &loop, const AgentConfiguration &configuration, AgentIdentity identity) :
        m_loop(loop), m_configuration(configuration), m_identity(std::move(identity)),
        m_requestTimeout(std::chrono::seconds{configuration.requestTimeoutSeconds > 0 ? configuration.requestTimeoutSeconds : 30})
    {
        m_baseUrl = configuration.baseUrl;
        while (m_baseUrl.size() > 1 && m_baseUrl.back() == '/')
        {
            m_baseUrl.pop_back();
        }
        m_registerUrl  = m_baseUrl + "/api/agent/register";
        m_heartbeatUrl = m_baseUrl + "/api/agent/heartbeat";
        m_telemetryUrl = m_baseUrl + "/api/agent/telemetry";
        m_commandsUrl  = m_baseUrl + "/api/agent/commands";
        m_agentNodesUrl = m_baseUrl + "/api/agent/nodes";
        m_firmwareUrl   = m_baseUrl + "/api/agent/firmware";

        try
        {
            static_cast<void>(AsynGyanis::Net::parseUrl(m_registerUrl));
        } catch (const std::exception &failure)
        {
            m_initializationError = "注册地址不合法：" + std::string{failure.what()};
            return;
        }

        try
        {
            AsynGyanis::Core::TlsPolicy policy;
            if (!configuration.caFile.empty())
            {
                policy.certificateAuthorityFile = configuration.caFile.string();
            }
            m_client = std::make_unique<AsynGyanis::Net::HttpClient>(m_loop, AsynGyanis::Net::HttpOutboundConnectionPool::Config{}, policy);
            if (configuration.baseUrl.starts_with("https://") && !m_client->setClientCertificate(configuration.certificateFile.string(), configuration.privateKeyFile.string()))
            {
                m_initializationError = "加载客户端证书与私钥失败：请确认两个文件都是 PEM 编码且相互配对（证书：" + configuration.certificateFile.string() + "，私钥：" +
                                        configuration.privateKeyFile.string() + "）";
                m_client.reset();
            }
        } catch (const std::exception &failure)
        {
            m_initializationError = "初始化 HTTPS 客户端失败：" + std::string{failure.what()};
            m_client.reset();
        }
    }

    AgentClient::~AgentClient() = default;

    AsynGyanis::Core::Task<std::expected<std::int64_t, RegistrationFailure>> AgentClient::registerAgent(std::string enrollmentToken, std::string agentVersion)
    {
        if (!m_initializationError.empty() || m_client == nullptr)
        {
            co_return std::unexpected(RegistrationFailure{m_initializationError, false, false});
        }

        AsynGyanis::Base::ConfigValue registration = AsynGyanis::Base::ConfigValue::object({
                {"name", m_configuration.name},
                {"agentVersion", std::move(agentVersion)},
                {"certFingerprint", m_identity.fingerprint},
                {"tenantCode", m_configuration.tenantCode},
                {"labels", "{}"},
        });
        if (!enrollmentToken.empty())
        {
            registration["enrollmentToken"] = std::move(enrollmentToken);
        }

        const std::string                  bodyText = registration.dump();
        AsynGyanis::Net::HttpClientRequest request;
        request.method      = "POST";
        request.body        = bodyText;
        request.contentType = "application/json";

        std::unique_ptr<AsynGyanis::Net::HttpClientResponse> response;
        try
        {
            response = co_await m_client->send(m_registerUrl, request, m_requestTimeout);
        } catch (const std::exception &failure)
        {
            co_return std::unexpected(RegistrationFailure{"注册请求异常：" + std::string{failure.what()}, false, false});
        }
        if (response == nullptr)
        {
            co_return std::unexpected(RegistrationFailure{"注册请求发送失败：连接、TLS 握手或超时，详细原因见引擎日志", true, false});
        }

        const int status = response->statusCode;
        // 403 表示令牌或指纹被拒：错误文本固定生成，绝不回显响应正文或请求里的令牌
        if (status == 403)
        {
            co_return std::unexpected(RegistrationFailure{"注册被服务端拒绝（HTTP 403）：注册令牌或证书指纹未通过校验，请联系管理员核对令牌与证书指纹", false, true});
        }
        if (status < 200 || status >= 300)
        {
            co_return std::unexpected(
                    RegistrationFailure{"注册返回 HTTP " + std::to_string(status) + "：" + envelopeMessage(response->body), isRetryableStatus(status), false, status});
        }

        const std::optional<AsynGyanis::Base::ConfigValue> envelope = AsynGyanis::Base::parseConfigValue(response->body);
        if (!envelope.has_value() || !envelope->is_object())
        {
            co_return std::unexpected(RegistrationFailure{"注册响应不是 JSON 对象", false, false});
        }
        const auto code = envelope->find("code");
        if (code != envelope->end() && code->is_number_integer() && code->get<std::int64_t>() != 200)
        {
            co_return std::unexpected(RegistrationFailure{"注册响应业务码不是成功值：" + envelopeMessage(response->body), false, false});
        }
        const auto data = envelope->find("data");
        if (data == envelope->end() || !data->is_object())
        {
            co_return std::unexpected(RegistrationFailure{"注册响应缺少 data 对象，无法读取节点标识", false, false});
        }
        const auto nodeId = data->find("id");
        if (nodeId == data->end() || !nodeId->is_number_integer() || nodeId->get<std::int64_t>() <= 0)
        {
            co_return std::unexpected(RegistrationFailure{"注册响应缺少正整数 data.id，无法保存节点标识", false, false});
        }
        co_return nodeId->get<std::int64_t>();
    }

    AsynGyanis::Core::Task<std::expected<HeartbeatReply, std::string>> AgentClient::heartbeat(std::int64_t nodeId, std::string agentVersion)
    {
        if (!m_initializationError.empty() || m_client == nullptr)
        {
            co_return std::unexpected(m_initializationError);
        }

        const AsynGyanis::Base::ConfigValue requestBody = AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"agentVersion", std::move(agentVersion)}});
        // HttpClientRequest::body 是 string_view：序列化结果必须落在协程帧里的具名局部量，
        // 直接绑定 dump() 的临时串会在 send 读取前释放
        const std::string                   bodyText = requestBody.dump();
        AsynGyanis::Net::HttpClientRequest  request;
        request.method      = "POST";
        request.body        = bodyText;
        request.contentType = "application/json";

        std::unique_ptr<AsynGyanis::Net::HttpClientResponse> response;
        try
        {
            response = co_await m_client->send(m_heartbeatUrl, request, m_requestTimeout);
        } catch (const std::exception &failure)
        {
            co_return std::unexpected("心跳请求异常：" + std::string{failure.what()});
        }
        if (response == nullptr)
        {
            co_return std::unexpected("心跳请求发送失败：连接、TLS 握手或超时，详细原因见引擎日志");
        }
        if (response->statusCode < 200 || response->statusCode >= 300)
        {
            co_return std::unexpected(httpFailure("心跳", response->statusCode, response->body));
        }

        const auto data = successData(response->body);
        if (!data)
        {
            co_return std::unexpected("心跳" + data.error());
        }

        HeartbeatReply reply;
        if (const auto desired = data->find("desiredConfigVersion"); desired != data->end())
        {
            reply.desiredConfigVersion = AsynGyanis::Base::configValueAs<std::int64_t>(*desired).value_or(-1);
        }
        if (const auto interval = data->find("heartbeatIntervalSeconds"); interval != data->end())
        {
            reply.heartbeatIntervalSeconds = AsynGyanis::Base::configValueAs<int>(*interval).value_or(0);
        }
        co_return reply;
    }

    AsynGyanis::Core::Task<std::expected<std::optional<AsynGyanis::Base::ConfigValue>, std::string>> AgentClient::takeNextCommand(const std::int64_t nodeId)
    {
        if (!m_initializationError.empty() || m_client == nullptr)
        {
            co_return std::unexpected(m_initializationError);
        }

        AsynGyanis::Net::HttpClientRequest request;
        request.method = "GET";

        std::unique_ptr<AsynGyanis::Net::HttpClientResponse> response;
        try
        {
            response = co_await m_client->send(m_commandsUrl + "/next?nodeId=" + std::to_string(nodeId), request, m_requestTimeout);
        } catch (const std::exception &failure)
        {
            co_return std::unexpected("拉取命令请求异常：" + std::string{failure.what()});
        }
        if (response == nullptr)
        {
            co_return std::unexpected("拉取命令请求发送失败：连接、TLS 握手或超时，详细原因见引擎日志");
        }
        if (response->statusCode < 200 || response->statusCode >= 300)
        {
            co_return std::unexpected(httpFailure("拉取命令", response->statusCode, response->body));
        }

        const auto checked = checkSuccessEnvelope(response->body);
        if (!checked)
        {
            co_return std::unexpected("拉取命令" + checked.error());
        }
        const auto envelope = AsynGyanis::Base::parseConfigValue(response->body);
        if (!envelope.has_value() || !envelope->is_object())
        {
            co_return std::unexpected("拉取命令响应不是 JSON 对象");
        }
        const auto data = envelope->find("data");
        if (data == envelope->end())
        {
            co_return std::unexpected("拉取命令响应缺少 data 字段");
        }
        if (data->is_null())
        {
            co_return std::optional<AsynGyanis::Base::ConfigValue>{};
        }
        if (!data->is_object())
        {
            co_return std::unexpected("拉取命令响应的 data 既不是对象也不是 null");
        }
        co_return std::optional<AsynGyanis::Base::ConfigValue>{*data};
    }

    AsynGyanis::Core::Task<ResultPostOutcome> AgentClient::reportCommandResult(const std::int64_t commandId, const std::int64_t nodeId, const std::string_view status,
                                                                               const std::string_view result, const std::string_view message)
    {
        if (!m_initializationError.empty() || m_client == nullptr)
        {
            co_return ResultPostOutcome{ResultPostDisposition::Retryable, 0, m_initializationError};
        }

        const AsynGyanis::Base::ConfigValue requestBody =
                AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"status", std::string{status}}, {"result", std::string{result}}, {"message", std::string{message}}});
        const auto serialized = AsynGyanis::Base::serializeConfigValue(requestBody);
        if (!serialized.has_value())
        {
            co_return ResultPostOutcome{ResultPostDisposition::Retryable, 0, "命令结果正文无法序列化为 JSON"};
        }

        AsynGyanis::Net::HttpClientRequest request;
        request.method      = "POST";
        request.body        = *serialized;
        request.contentType = "application/json";

        std::unique_ptr<AsynGyanis::Net::HttpClientResponse> response;
        try
        {
            response = co_await m_client->send(m_commandsUrl + "/" + std::to_string(commandId) + "/result", request, m_requestTimeout);
        } catch (const std::exception &failure)
        {
            co_return ResultPostOutcome{ResultPostDisposition::Retryable, 0, "上报命令结果请求异常：" + std::string{failure.what()}};
        }
        if (response == nullptr)
        {
            co_return ResultPostOutcome{ResultPostDisposition::Retryable, 0, "上报命令结果请求发送失败：连接、TLS 握手或超时，详细原因见引擎日志"};
        }
        // 只有 200 表示结果已被服务端接受；408、425、429 与 5xx 可重试，其余状态重试同一行只会永久阻塞队列
        if (response->statusCode == 200)
        {
            co_return ResultPostOutcome{ResultPostDisposition::Accepted, 200, {}};
        }
        const int                   httpStatus  = response->statusCode;
        const std::string           failure     = httpFailure("上报命令结果", httpStatus, response->body);
        const ResultPostDisposition disposition = isResultRetryableStatus(httpStatus) ? ResultPostDisposition::Retryable : ResultPostDisposition::Terminal;
        co_return ResultPostOutcome{disposition, httpStatus, failure};
    }

    AsynGyanis::Core::Task<std::expected<void, TelemetryFailure>> AgentClient::sendTelemetry(const std::int64_t nodeId,
                                                                                              const AsynGyanis::Base::ConfigValue &samples)
    {
        if (!m_initializationError.empty() || m_client == nullptr)
        {
            co_return std::unexpected(TelemetryFailure{m_initializationError, true, 0});
        }
        if (nodeId <= 0)
        {
            co_return std::unexpected(TelemetryFailure{"节点标识无效，无法上报遥测", false, 0});
        }
        if (!samples.is_array() || samples.empty() || samples.size() > kTelemetryBatchLimit)
        {
            co_return std::unexpected(TelemetryFailure{"遥测样本必须是 1 到 " + std::to_string(kTelemetryBatchLimit) + " 条的 JSON 数组", false, 0});
        }

        const AsynGyanis::Base::ConfigValue requestBody = AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"samples", samples}});
        // HttpClientRequest::body 是 string_view：序列化结果必须落在协程帧里的具名局部量
        const auto serialized = AsynGyanis::Base::serializeConfigValue(requestBody);
        if (!serialized.has_value())
        {
            co_return std::unexpected(TelemetryFailure{"遥测请求正文无法序列化为 JSON", false, 0});
        }
        const std::string bodyText = *serialized;

        AsynGyanis::Net::HttpClientRequest request;
        request.method      = "POST";
        request.body        = bodyText;
        request.contentType = "application/json";

        std::unique_ptr<AsynGyanis::Net::HttpClientResponse> response;
        try
        {
            response = co_await m_client->send(m_telemetryUrl, request, m_requestTimeout);
        } catch (const std::exception &failure)
        {
            co_return std::unexpected(TelemetryFailure{"遥测上报请求异常：" + std::string{failure.what()}, true, 0});
        }
        if (response == nullptr)
        {
            co_return std::unexpected(TelemetryFailure{"遥测上报请求发送失败：连接、TLS 握手或超时，详细原因见引擎日志", true, 0});
        }
        if (response->statusCode == 200)
        {
            co_return std::expected<void, TelemetryFailure>{};
        }
        const int  httpStatus = response->statusCode;
        const bool retryable  = isResultRetryableStatus(httpStatus);
        co_return std::unexpected(TelemetryFailure{httpFailure("遥测上报", httpStatus, response->body), retryable, httpStatus});
    }

    AsynGyanis::Core::Task<std::expected<AgentConfigSnapshot, std::string>> AgentClient::fetchConfig(const std::int64_t nodeId)
    {
        if (!m_initializationError.empty() || m_client == nullptr)
        {
            co_return std::unexpected(m_initializationError);
        }
        if (nodeId <= 0)
        {
            co_return std::unexpected("节点标识无效，无法拉取配置");
        }

        AsynGyanis::Net::HttpClientRequest request;
        request.method = "GET";

        std::unique_ptr<AsynGyanis::Net::HttpClientResponse> response;
        try
        {
            response = co_await m_client->send(m_agentNodesUrl + "/" + std::to_string(nodeId) + "/config", request, m_requestTimeout);
        } catch (const std::exception &failure)
        {
            co_return std::unexpected("拉取配置请求异常：" + std::string{failure.what()});
        }
        if (response == nullptr)
        {
            co_return std::unexpected("拉取配置请求发送失败：连接、TLS 握手或超时，详细原因见引擎日志");
        }
        if (response->statusCode < 200 || response->statusCode >= 300)
        {
            co_return std::unexpected(httpFailure("拉取配置", response->statusCode, response->body));
        }

        const auto data = successData(response->body);
        if (!data)
        {
            co_return std::unexpected("拉取配置" + data.error());
        }

        const auto version = data->find("version");
        if (version == data->end())
        {
            co_return std::unexpected("拉取配置响应缺少整数 version");
        }
        const auto parsedVersion = AsynGyanis::Base::configValueAs<std::int64_t>(*version);
        if (!parsedVersion.has_value())
        {
            co_return std::unexpected("拉取配置响应的 version 不是整数");
        }
        AgentConfigSnapshot snapshot;
        snapshot.version = *parsedVersion;
        if (snapshot.version < 0)
        {
            co_return std::unexpected("拉取配置响应的 version 为负数");
        }

        const auto content = data->find("content");
        if (content == data->end() || !content->is_object())
        {
            co_return std::unexpected("拉取配置响应的 content 不是 JSON 对象");
        }
        snapshot.content = *content;

        if (const auto hash = data->find("hash"); hash != data->end())
        {
            if (!hash->is_string())
            {
                co_return std::unexpected("拉取配置响应的 hash 不是字符串");
            }
            snapshot.hash = hash->get<std::string>();
        }
        co_return snapshot;
    }

    AsynGyanis::Core::Task<std::expected<void, std::string>> AgentClient::reportConfig(const std::int64_t nodeId, const std::int64_t version,
                                                                                       const AsynGyanis::Base::ConfigValue &content, const std::string_view status,
                                                                                       const std::string_view message)
    {
        if (!m_initializationError.empty() || m_client == nullptr)
        {
            co_return std::unexpected(m_initializationError);
        }
        if (nodeId <= 0)
        {
            co_return std::unexpected("节点标识无效，无法上报配置结果");
        }
        if (version < 0)
        {
            co_return std::unexpected("配置版本不能为负数");
        }
        if (status != "applied" && status != "failed")
        {
            co_return std::unexpected("配置上报状态只允许 applied 或 failed");
        }
        if (!content.is_object())
        {
            co_return std::unexpected("配置上报正文必须是 JSON 对象");
        }

        const AsynGyanis::Base::ConfigValue requestBody =
                AsynGyanis::Base::ConfigValue::object({{"version", version}, {"content", content}, {"status", std::string{status}}, {"message", std::string{message}}});
        const auto serialized = AsynGyanis::Base::serializeConfigValue(requestBody);
        if (!serialized.has_value())
        {
            co_return std::unexpected("配置上报正文无法序列化为 JSON");
        }

        AsynGyanis::Net::HttpClientRequest request;
        request.method      = "POST";
        request.body        = *serialized;
        request.contentType = "application/json";

        std::unique_ptr<AsynGyanis::Net::HttpClientResponse> response;
        try
        {
            response = co_await m_client->send(m_agentNodesUrl + "/" + std::to_string(nodeId) + "/config/report", request, m_requestTimeout);
        } catch (const std::exception &failure)
        {
            co_return std::unexpected("上报配置结果请求异常：" + std::string{failure.what()});
        }
        if (response == nullptr)
        {
            co_return std::unexpected("上报配置结果请求发送失败：连接、TLS 握手或超时，详细原因见引擎日志");
        }
        if (response->statusCode < 200 || response->statusCode >= 300)
        {
            co_return std::unexpected(httpFailure("上报配置结果", response->statusCode, response->body));
        }
        const auto checked = checkSuccessEnvelope(response->body);
        if (!checked)
        {
            co_return std::unexpected("上报配置结果" + checked.error());
        }
        co_return std::expected<void, std::string>{};
    }

    AsynGyanis::Core::Task<std::expected<DownloadResult, std::string>> AgentClient::downloadFirmware(std::string downloadPath,
                                                                                                    AsynGyanis::Net::HttpResponseBodyReceiver receiver)
    {
        if (!m_initializationError.empty() || m_client == nullptr)
        {
            co_return std::unexpected(m_initializationError);
        }
        if (!receiver)
        {
            co_return std::unexpected("固件下载缺少正文接收口");
        }

        auto resolved = resolveDownloadUrl(downloadPath, m_baseUrl, m_configuration.allowPlaintextForTests);
        if (!resolved.has_value())
        {
            co_return std::unexpected(resolved.error());
        }

        AsynGyanis::Net::HttpClientRequest request;
        request.method               = "GET";
        request.responseBodyReceiver = std::move(receiver);

        std::unique_ptr<AsynGyanis::Net::HttpClientResponse> response;
        try
        {
            response = co_await m_client->send(*resolved, request, kFirmwareDownloadTimeout);
        } catch (const std::exception &failure)
        {
            co_return std::unexpected("固件下载请求异常：" + std::string{failure.what()});
        }
        if (response == nullptr)
        {
            co_return std::unexpected("固件下载请求发送失败：连接、TLS 握手或超时，详细原因见引擎日志");
        }

        DownloadResult result;
        result.httpStatus = response->statusCode;
        if (result.httpStatus < 200 || result.httpStatus >= 300)
        {
            // 正文已交给接收口，这里的 body 为空；失败原因取统一包络或固定文案
            result.message = envelopeMessage(response->body);
        }
        co_return result;
    }

    AsynGyanis::Core::Task<std::expected<void, std::string>> AgentClient::reportFirmwareStatus(const std::int64_t firmwareId, const std::int64_t nodeId,
                                                                                               const std::string_view status, const int progress, const std::string_view message)
    {
        if (!m_initializationError.empty() || m_client == nullptr)
        {
            co_return std::unexpected(m_initializationError);
        }
        if (firmwareId <= 0)
        {
            co_return std::unexpected("固件标识无效，无法上报状态");
        }
        if (nodeId <= 0)
        {
            co_return std::unexpected("节点标识无效，无法上报固件状态");
        }
        if (status != "pending" && status != "downloading" && status != "installing" && status != "installed" && status != "failed")
        {
            co_return std::unexpected("固件状态只允许 pending、downloading、installing、installed 或 failed");
        }
        if (progress < 0 || progress > 100)
        {
            co_return std::unexpected("固件进度必须在 0 到 100 之间");
        }

        const AsynGyanis::Base::ConfigValue requestBody =
                AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"status", std::string{status}}, {"progress", progress}, {"message", std::string{message}}});
        // HttpClientRequest::body 是 string_view：序列化结果必须落在协程帧里的具名局部量
        const std::string bodyText = requestBody.dump();

        AsynGyanis::Net::HttpClientRequest request;
        request.method      = "POST";
        request.body        = bodyText;
        request.contentType = "application/json";

        std::unique_ptr<AsynGyanis::Net::HttpClientResponse> response;
        try
        {
            response = co_await m_client->send(m_firmwareUrl + "/" + std::to_string(firmwareId) + "/status", request, m_requestTimeout);
        } catch (const std::exception &failure)
        {
            co_return std::unexpected("上报固件状态请求异常：" + std::string{failure.what()});
        }
        if (response == nullptr)
        {
            co_return std::unexpected("上报固件状态请求发送失败：连接、TLS 握手或超时，详细原因见引擎日志");
        }
        if (response->statusCode < 200 || response->statusCode >= 300)
        {
            co_return std::unexpected(httpFailure("上报固件状态", response->statusCode, response->body));
        }
        const auto checked = checkSuccessEnvelope(response->body);
        if (!checked)
        {
            co_return std::unexpected("上报固件状态" + checked.error());
        }
        co_return std::expected<void, std::string>{};
    }
} // namespace EdgePulse::Agent
