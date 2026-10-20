#include "Core/Telemetry/UdpTelemetryEndpoint.h"

#include "Base/Config/ConfigValue.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Telemetry/TelemetryIngest.h"
#include "Net/Udp/UdpServer.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Telemetry
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        [[nodiscard]] std::vector<std::uint8_t> textReply(const ConfigValue &value)
        {
            const std::string text = value.dump();
            return std::vector<std::uint8_t>(text.begin(), text.end());
        }
    } // namespace

    UdpTelemetryEndpoint::UdpTelemetryEndpoint(AsynGyanis::Core::EventLoop &loop, Configuration configuration, Device::NodeRepository &nodes, TelemetryRepository &telemetry,
                                               Alarm::AlarmService &alarms) :
        m_loop(loop), m_configuration(std::move(configuration)), m_nodes(nodes), m_telemetry(telemetry), m_alarms(alarms)
    {
        if (m_configuration.secret.empty())
        {
            throw std::invalid_argument("UDP 遥测已启用但没有配置共享密钥");
        }
        AsynGyanis::Net::UdpServer::Configuration serverConfiguration;
        serverConfiguration.onMessage = [this](AsynGyanis::Core::InetAddress, const std::span<const std::uint8_t> payload)
        { return handleDatagram(std::vector<std::uint8_t>(payload.begin(), payload.end())); };
        m_server = std::make_unique<AsynGyanis::Net::UdpServer>(loop, std::move(serverConfiguration));
        if (!AsynGyanis::Core::InetAddress::resolve(m_configuration.address, m_configuration.port).has_value())
        {
            throw std::invalid_argument("UDP 遥测监听地址无法解析：" + m_configuration.address);
        }
    }

    UdpTelemetryEndpoint::~UdpTelemetryEndpoint() = default;

    AsynGyanis::Core::Task<> UdpTelemetryEndpoint::listen()
    {
        const auto address = AsynGyanis::Core::InetAddress::resolve(m_configuration.address, m_configuration.port);
        if (!address.has_value())
        {
            throw std::invalid_argument("UDP 遥测监听地址无法解析：" + m_configuration.address);
        }
        co_await m_server->listen(*address);
    }

    void UdpTelemetryEndpoint::stop() noexcept
    {
        m_server->stop();
    }

    std::uint16_t UdpTelemetryEndpoint::port() const noexcept
    {
        return m_server->listeningPort();
    }

    AsynGyanis::Core::Task<std::vector<std::uint8_t>> UdpTelemetryEndpoint::handleDatagram(std::vector<std::uint8_t> payload)
    {
        const auto nowSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto parsed     = parseUdpTelemetryDatagram(std::string_view{reinterpret_cast<const char *>(payload.data()), payload.size()}, m_configuration.secret,
                                                          static_cast<std::int64_t>(nowSeconds));
        if (!parsed.ok)
        {
            if (parsed.unauthorized)
            {
                co_return textReply(ConfigValue::object({{"error", "unauthorized"}}));
            }
            co_return textReply(ConfigValue::object({{"error", parsed.error}}));
        }
        if (!m_replayGuard.accept(parsed.datagram.nodeId, parsed.datagram.nonce, parsed.datagram.timestamp, nowSeconds))
        {
            co_return textReply(ConfigValue::object({{"error", "replay"}}));
        }

        const TelemetryIngestOutcome outcome = co_await ingestTelemetry(m_loop, m_nodes, m_telemetry, m_alarms, parsed.datagram.nodeId, parsed.datagram.samples);
        if (!outcome.nodeExists)
        {
            co_return textReply(ConfigValue::object({{"error", "unknown node"}}));
        }
        if (!outcome.stored)
        {
            co_return textReply(ConfigValue::object({{"error", "storage failed"}}));
        }
        co_return textReply(
                ConfigValue::object({{"accepted", static_cast<std::int64_t>(parsed.datagram.samples.size())}, {"alarms", static_cast<std::int64_t>(outcome.alarms.size())}}));
    }
} // namespace EdgePulse::Telemetry
