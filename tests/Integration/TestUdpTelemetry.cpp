/**
 * @file TestUdpTelemetry.cpp
 * @brief 真实 UDP 套接字到遥测落库的端到端用例
 * @author Gyanis
 * @date 2026-10-21
 */

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "Base/Config/ConfigValue.h"
#include "Base/Security/Hmac.h"
#include "Core/Alarm/AlarmRepository.h"
#include "Core/Alarm/AlarmService.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Device/NodeService.h"
#include "Core/Telemetry/TelemetryRepository.h"
#include "Core/Telemetry/UdpTelemetryEndpoint.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace
{
    using AsynGyanis::Base::ConfigValue;

    class DatagramClient
    {
    public:
        DatagramClient()
        {
#ifdef _WIN32
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            {
                return;
            }
#endif
            m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        }

        ~DatagramClient()
        {
#ifdef _WIN32
            if (m_socket != INVALID_SOCKET)
            {
                closesocket(m_socket);
            }
            WSACleanup();
#else
            if (m_socket >= 0)
            {
                ::close(m_socket);
            }
#endif
        }

        DatagramClient(const DatagramClient &)            = delete;
        DatagramClient &operator=(const DatagramClient &) = delete;

        [[nodiscard]] bool usable() const noexcept
        {
#ifdef _WIN32
            return m_socket != INVALID_SOCKET;
#else
            return m_socket >= 0;
#endif
        }

        [[nodiscard]] bool send(const std::uint16_t port, const std::string &text) const
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port   = htons(port);
            inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
            const auto length = static_cast<int>(text.size());
            return ::sendto(m_socket, text.data(), length, 0, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == length;
        }

        [[nodiscard]] std::string receive(const std::chrono::milliseconds timeout) const
        {
#ifdef _WIN32
            const DWORD milliseconds = static_cast<DWORD>(timeout.count());
            setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&milliseconds), sizeof(milliseconds));
#else
            const timeval value{static_cast<long>(timeout.count() / 1000), static_cast<long>((timeout.count() % 1000) * 1000)};
            setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
#endif
            char        buffer[4096];
            sockaddr_in source{};
            socklen_t   sourceLength = sizeof(source);
            const int   received     = static_cast<int>(::recvfrom(m_socket, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr *>(&source), &sourceLength));
            return received > 0 ? std::string(buffer, static_cast<std::size_t>(received)) : std::string{};
        }

    private:
#ifdef _WIN32
        SOCKET m_socket{INVALID_SOCKET};
#else
        int m_socket{-1};
#endif
    };

    [[nodiscard]] std::string signedDatagram(const std::string &secret, const std::int64_t nodeId, const std::int64_t timestamp, const std::string &nonce, const ConfigValue &body)
    {
        const std::string canonical = "v1\n" + std::to_string(nodeId) + "\n" + std::to_string(timestamp) + "\n" + nonce + "\n" + body.dump();
        const std::string signature = EdgePulse::Common::hmacSha256Hex(secret, canonical);
        return ConfigValue::object({{"nodeId", nodeId}, {"timestamp", timestamp}, {"nonce", nonce}, {"body", body}, {"signature", signature}}).dump();
    }

    [[nodiscard]] ConfigValue telemetryBody()
    {
        return ConfigValue::object({{"samples", ConfigValue::array({ConfigValue::object({{"metric", "udp.cpu"}, {"value", 42.5}, {"unit", "%"}})})}});
    }
} // namespace

// 钉住：签名数据报经真实 UDP 端口落库，重放与坏签名被拒
TEST(UdpTelemetry, IngestOverRealSocketRejectsReplayAndBadSignature)
{
    EdgePulse::TestSupport::AuthTestContext context;
    auto                                    runner     = std::make_unique<EdgePulse::TestSupport::EventLoopRunner>();
    auto                                    nodes      = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto                                    service    = std::make_shared<EdgePulse::Device::NodeService>(nodes);
    auto                                    telemetry  = std::make_shared<EdgePulse::Telemetry::TelemetryRepository>(context.database);
    auto                                    alarmsRepo = std::make_shared<EdgePulse::Alarm::AlarmRepository>(context.database);
    auto                                    alarms     = std::make_shared<EdgePulse::Alarm::AlarmService>(alarmsRepo);

    const auto node = runner->run(service->create(runner->loop(), "udp-node", "{}", 200, 0, 0));
    ASSERT_TRUE(node.has_value());

    const std::string secret = "0123456789abcdef0123456789abcdef";
    auto endpoint = std::make_unique<EdgePulse::Telemetry::UdpTelemetryEndpoint>(runner->loop(), EdgePulse::Telemetry::UdpTelemetryEndpoint::Configuration{"127.0.0.1", 0, secret},
                                                                                 *nodes, *telemetry, *alarms);

    auto                     listenCompletion = std::make_shared<std::promise<void>>();
    auto                     listenDone       = listenCompletion->get_future();
    AsynGyanis::Core::Task<> listenTask       = [](EdgePulse::Telemetry::UdpTelemetryEndpoint &target, std::shared_ptr<std::promise<void>> completion) -> AsynGyanis::Core::Task<>
    {
        co_await target.listen();
        completion->set_value();
        co_return;
    }(*endpoint, listenCompletion);
    runner->loop().scheduler().scheduleRemote(listenTask.handle());

    for (int attempt = 0; attempt < 500 && endpoint->port() == 0; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_NE(0, endpoint->port());

    DatagramClient client;
    ASSERT_TRUE(client.usable());

    const auto now      = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto body     = telemetryBody();
    const auto datagram = signedDatagram(secret, node->id, now, "nonce-integration-1", body);
    ASSERT_TRUE(client.send(endpoint->port(), datagram));
    const auto acknowledgement = client.receive(std::chrono::milliseconds{3000});
    EXPECT_NE(std::string::npos, acknowledgement.find("\"accepted\":1")) << acknowledgement;

    bool stored = false;
    for (int attempt = 0; attempt < 100 && !stored; ++attempt)
    {
        for (const auto &point: runner->run(telemetry->latest(runner->loop(), node->id)))
        {
            if (point.metric == "udp.cpu" && point.value == 42.5)
            {
                stored = true;
                break;
            }
        }
        if (!stored)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }
    EXPECT_TRUE(stored);

    ASSERT_TRUE(client.send(endpoint->port(), datagram));
    const auto replay = client.receive(std::chrono::milliseconds{3000});
    EXPECT_NE(std::string::npos, replay.find("\"error\":\"replay\"")) << replay;

    std::string tampered = signedDatagram(secret, node->id, now, "nonce-integration-2", body);
    const auto  position = tampered.find("\"signature\":\"");
    ASSERT_NE(std::string::npos, position);
    tampered[position + 13] = tampered[position + 13] == 'a' ? 'b' : 'a';
    ASSERT_TRUE(client.send(endpoint->port(), tampered));
    const auto unauthorized = client.receive(std::chrono::milliseconds{3000});
    EXPECT_NE(std::string::npos, unauthorized.find("\"error\":\"unauthorized\"")) << unauthorized;

    runner->loop().scheduler().postRemote([rawEndpoint = endpoint.get()] { rawEndpoint->stop(); });
    ASSERT_EQ(std::future_status::ready, listenDone.wait_for(std::chrono::seconds{5}));
    runner.reset();
    endpoint.reset();
}
