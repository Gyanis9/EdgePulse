/**
 * @file AgentChannelRegistry.h
 * @brief 在线 Agent 的 WebSocket 通道表
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace AsynGyanis::Net
{
    class WebSocketPeer;
}

namespace EdgePulse::Command
{
    /**
     * @brief 节点到 Agent 连接的映射
     * @note 默认单事件循环；多循环部署下通道必须只由所属循环触碰，
     *       命令路由与 Agent WebSocket 因此都要落在同一条循环上。
     */
    class AgentChannelRegistry
    {
    public:
        /**
         * @brief 登记或替换节点连接
         * @param nodeId 节点
         * @param peer 连接
         */
        void registerNode(std::int64_t nodeId, AsynGyanis::Net::WebSocketPeer *peer);

        /**
         * @brief 注销连接；只在当前登记的仍是该连接时移除
         * @param nodeId 节点
         * @param peer 连接
         */
        void unregisterNode(std::int64_t nodeId, AsynGyanis::Net::WebSocketPeer *peer);

        /**
         * @brief 节点是否在线
         * @param nodeId 节点
         * @return bool 是否在线
         */
        [[nodiscard]] bool isConnected(std::int64_t nodeId) const;

        /**
         * @brief 从通道表摘除节点并以指定状态码关闭连接
         * @param nodeId 节点
         * @param code 关闭状态码，证书吊销按策略违规使用 1008
         * @param reason 关闭原因，按「指针 + 长度」取，必须活到本次协程结束
         * @return AsynGyanis::Core::Task<> 完成时通道已摘除；连接不存在或已关闭时只摘表
         */
        AsynGyanis::Core::Task<> closeNode(std::int64_t nodeId, std::uint16_t code, std::string_view reason);

        /**
         * @brief 向节点推送一条文本
         * @param nodeId 节点
         * @param text 文本
         * @return AsynGyanis::Core::Task<bool> 是否写出成功；节点不在线时为 false
         */
        AsynGyanis::Core::Task<bool> send(std::int64_t nodeId, std::string_view text);

    private:
        mutable std::mutex                                                 m_mutex; ///< 保护通道表
        std::unordered_map<std::int64_t, AsynGyanis::Net::WebSocketPeer *> m_peers; ///< 节点到连接
    };
} // namespace EdgePulse::Command
