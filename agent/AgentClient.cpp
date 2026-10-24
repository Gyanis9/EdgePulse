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
    } // namespace

    AgentClient::AgentClient(AsynGyanis::Core::EventLoop &loop, const AgentConfiguration &configuration, AgentIdentity identity) :
        m_loop(loop), m_configuration(configuration), m_identity(std::move(identity)),
        m_requestTimeout(std::chrono::seconds{configuration.requestTimeoutSeconds > 0 ? configuration.requestTimeoutSeconds : 30})
    {
        std::string baseUrl = configuration.baseUrl;
        while (baseUrl.size() > 1 && baseUrl.back() == '/')
        {
            baseUrl.pop_back();
        }
        m_registerUrl = baseUrl + "/api/agent/register";

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
} // namespace EdgePulse::Agent
