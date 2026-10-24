#include "AgentClient.h"

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
        m_commandsUrl  = m_baseUrl + "/api/agent/commands";

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
        AsynGyanis::Net::HttpClientRequest  request;
        request.method      = "POST";
        request.body        = requestBody.dump();
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

    AsynGyanis::Core::Task<std::expected<void, std::string>> AgentClient::reportCommandResult(const std::int64_t commandId, const std::int64_t nodeId,
                                                                                              const std::string_view status, const std::string_view result,
                                                                                              const std::string_view message)
    {
        if (!m_initializationError.empty() || m_client == nullptr)
        {
            co_return std::unexpected(m_initializationError);
        }

        const AsynGyanis::Base::ConfigValue requestBody =
                AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"status", std::string{status}}, {"result", std::string{result}}, {"message", std::string{message}}});
        const auto serialized = AsynGyanis::Base::serializeConfigValue(requestBody);
        if (!serialized.has_value())
        {
            co_return std::unexpected("命令结果正文无法序列化为 JSON");
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
            co_return std::unexpected("上报命令结果请求异常：" + std::string{failure.what()});
        }
        if (response == nullptr)
        {
            co_return std::unexpected("上报命令结果请求发送失败：连接、TLS 握手或超时，详细原因见引擎日志");
        }
        // 只有 200 表示结果已被服务端接受：其他 2xx 不保证命令已结束，结果继续留在队列里
        if (response->statusCode != 200)
        {
            co_return std::unexpected(httpFailure("上报命令结果", response->statusCode, response->body));
        }
        co_return std::expected<void, std::string>{};
    }
} // namespace EdgePulse::Agent
