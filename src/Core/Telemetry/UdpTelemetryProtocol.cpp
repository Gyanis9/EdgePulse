#include "Core/Telemetry/UdpTelemetryProtocol.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Json/JsonFields.h"
#include "Base/Security/Hmac.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace EdgePulse::Telemetry
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        constexpr std::int64_t kMaximumClockSkewSeconds = 120;
        constexpr std::size_t  kMaximumSamples          = 500;
        constexpr std::size_t  kMaximumMetricLength     = 64;

        [[nodiscard]] UdpTelemetryParseResult rejected()
        {
            UdpTelemetryParseResult result;
            result.unauthorized = true;
            return result;
        }

        [[nodiscard]] UdpTelemetryParseResult invalid(std::string message)
        {
            UdpTelemetryParseResult result;
            result.error = std::move(message);
            return result;
        }

        [[nodiscard]] std::string canonicalText(const std::int64_t nodeId, const std::int64_t timestamp, const std::string_view nonce, const ConfigValue &body)
        {
            std::string text{"v1\n"};
            text += std::to_string(nodeId);
            text.push_back('\n');
            text += std::to_string(timestamp);
            text.push_back('\n');
            text.append(nonce);
            text.push_back('\n');
            text += body.dump();
            return text;
        }
    } // namespace

    UdpTelemetryParseResult parseUdpTelemetryDatagram(const std::string_view payload, const std::string_view secret, const std::int64_t nowSeconds)
    {
        const auto value = AsynGyanis::Base::parseConfigValue(payload);
        if (!value.has_value() || !value->is_object())
        {
            return rejected();
        }
        const auto nodeId    = Json::optionalInteger(*value, "nodeId");
        const auto timestamp = Json::optionalInteger(*value, "timestamp");
        const auto nonce     = Json::optionalString(*value, "nonce");
        const auto signature = Json::optionalString(*value, "signature");
        const auto body      = value->find("body");
        if (!nodeId.has_value() || *nodeId <= 0 || !timestamp.has_value() || !nonce.has_value() || nonce->size() < 8 || nonce->size() > 64 || !signature.has_value() ||
            body == value->end() || !body->is_object())
        {
            return rejected();
        }
        if (nowSeconds > *timestamp + kMaximumClockSkewSeconds || *timestamp > nowSeconds + kMaximumClockSkewSeconds)
        {
            return rejected();
        }

        const std::string expected = Common::hmacSha256Hex(secret, canonicalText(*nodeId, *timestamp, *nonce, *body));
        if (expected.empty() || !Common::constantTimeHexEquals(expected, *signature))
        {
            return rejected();
        }

        const auto samples = body->find("samples");
        if (samples == body->end() || !samples->is_array() || samples->empty() || samples->size() > kMaximumSamples)
        {
            return invalid("samples 必须是 1 到 500 条的数组");
        }

        UdpTelemetryParseResult result;
        result.datagram.nodeId    = *nodeId;
        result.datagram.timestamp = *timestamp;
        result.datagram.nonce     = *nonce;
        result.datagram.samples.reserve(samples->size());
        for (const ConfigValue &sample: *samples)
        {
            if (!sample.is_object())
            {
                return invalid("samples 的每一项必须是对象");
            }
            TelemetrySample parsed;
            parsed.metric      = Json::stringField(sample, "metric");
            parsed.unit        = Json::stringField(sample, "unit");
            parsed.labelsJson  = Json::stringField(sample, "labels");
            parsed.collectedAt = Json::stringField(sample, "collectedAt");
            const auto number  = sample.find("value");
            if (parsed.metric.empty() || parsed.metric.size() > kMaximumMetricLength || number == sample.end() || !number->is_number())
            {
                return invalid("样本缺少 metric 或数值 value");
            }
            parsed.value = number->get<double>();
            if (!std::isfinite(parsed.value))
            {
                return invalid("value 必须是有限数值");
            }
            result.datagram.samples.push_back(std::move(parsed));
        }
        result.ok = true;
        return result;
    }

    UdpTelemetryReplayGuard::UdpTelemetryReplayGuard(const std::int64_t windowSeconds) : m_windowSeconds(windowSeconds > 0 ? windowSeconds : kMaximumClockSkewSeconds)
    {
    }

    bool UdpTelemetryReplayGuard::accept(const std::int64_t nodeId, const std::string_view nonce, const std::int64_t timestamp, const std::int64_t nowSeconds)
    {
        prune(nowSeconds);
        if (m_seen.size() >= kMaximumEntries)
        {
            return false;
        }
        std::string key = std::to_string(nodeId);
        key.push_back(':');
        key.append(nonce);
        if (m_seen.contains(key))
        {
            return false;
        }
        m_seen.emplace(std::move(key), timestamp);
        return true;
    }

    void UdpTelemetryReplayGuard::prune(const std::int64_t nowSeconds)
    {
        const std::int64_t oldest = nowSeconds - m_windowSeconds * 2;
        for (auto iterator = m_seen.begin(); iterator != m_seen.end();)
        {
            if (iterator->second < oldest)
            {
                iterator = m_seen.erase(iterator);
            } else
            {
                ++iterator;
            }
        }
    }
} // namespace EdgePulse::Telemetry
