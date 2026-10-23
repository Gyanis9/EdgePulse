#include "Core/Command/AgentChannelRegistry.h"

#include "Net/WebSocket/WebSocketPeer.h"

#include <string>
#include <utility>

namespace EdgePulse::Command
{
    void AgentChannelRegistry::registerNode(const std::int64_t nodeId, AsynGyanis::Net::WebSocketPeer *peer)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_peers[nodeId] = peer;
    }

    void AgentChannelRegistry::unregisterNode(const std::int64_t nodeId, AsynGyanis::Net::WebSocketPeer *peer)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (const auto iterator = m_peers.find(nodeId); iterator != m_peers.end() && iterator->second == peer)
        {
            m_peers.erase(iterator);
        }
    }

    bool AgentChannelRegistry::isConnected(const std::int64_t nodeId) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_peers.contains(nodeId);
    }

    AsynGyanis::Core::Task<> AgentChannelRegistry::closeNode(const std::int64_t nodeId, const std::uint16_t code, const std::string_view reason)
    {
        AsynGyanis::Net::WebSocketPeer *peer = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (const auto iterator = m_peers.find(nodeId); iterator != m_peers.end())
            {
                peer = iterator->second;
                // 先摘表再关闭：命令路由在协程挂起期间也不能再拿到这条通道
                m_peers.erase(iterator);
            }
        }
        if (peer != nullptr && peer->isOpen())
        {
            static_cast<void>(co_await peer->close(code, reason));
        }
        co_return;
    }

    AsynGyanis::Core::Task<bool> AgentChannelRegistry::send(const std::int64_t nodeId, std::string_view text)
    {
        AsynGyanis::Net::WebSocketPeer *peer = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (const auto iterator = m_peers.find(nodeId); iterator != m_peers.end())
            {
                peer = iterator->second;
            }
        }
        if (peer == nullptr || !peer->isOpen())
        {
            co_return false;
        }
        co_return co_await peer->sendText(text);
    }
} // namespace EdgePulse::Command
