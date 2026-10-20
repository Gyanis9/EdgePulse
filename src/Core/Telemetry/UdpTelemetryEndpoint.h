/**
 * @file UdpTelemetryEndpoint.h
 * @brief UDP 遥测监听：认证数据报、写库并评估告警
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Telemetry/UdpTelemetryProtocol.h"

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}
namespace AsynGyanis::Net
{
    class UdpServer;
}
namespace EdgePulse::Alarm
{
    class AlarmService;
}
namespace EdgePulse::Device
{
    class NodeRepository;
}
namespace EdgePulse::Telemetry
{
    class TelemetryRepository;

    /**
     * @brief UDP 遥测端点
     * @note 只在所属事件循环上构造、启动与停止
     */
    class UdpTelemetryEndpoint
    {
    public:
        /**
         * @brief 端点配置
         */
        struct Configuration
        {
            std::string   address{"0.0.0.0"}; ///< 监听地址
            std::uint16_t port{9555};         ///< 监听端口；0 表示由内核分配
            std::string   secret;             ///< HMAC 共享密钥
        };

        /**
         * @brief 构造并登记报文处理器（尚未监听）
         * @param loop 所属事件循环
         * @param configuration 配置
         * @param nodes 节点仓储
         * @param telemetry 遥测仓储
         * @param alarms 告警服务
         * @throws Base::InvalidArgumentException 密钥为空、报文上限非法
         * @throws Base::SystemException 监听地址无法解析
         */
        UdpTelemetryEndpoint(AsynGyanis::Core::EventLoop &loop, Configuration configuration, Device::NodeRepository &nodes, TelemetryRepository &telemetry,
                             Alarm::AlarmService &alarms);

        ~UdpTelemetryEndpoint();

        UdpTelemetryEndpoint(const UdpTelemetryEndpoint &)            = delete;
        UdpTelemetryEndpoint &operator=(const UdpTelemetryEndpoint &) = delete;

        /**
         * @brief 绑定端口并开始收包
         * @return AsynGyanis::Core::Task<> 协程帧必须活到 stop() 之后
         */
        [[nodiscard]] AsynGyanis::Core::Task<> listen();

        /**
         * @brief 停止收包
         */
        void stop() noexcept;

        /// 实际监听端口；尚未绑定为 0
        [[nodiscard]] std::uint16_t port() const noexcept;

    private:
        /**
         * @brief 处理一条数据报
         * @param payload 报文
         * @return std::vector<std::uint8_t> 应答
         */
        [[nodiscard]] AsynGyanis::Core::Task<std::vector<std::uint8_t>> handleDatagram(std::vector<std::uint8_t> payload);

        AsynGyanis::Core::EventLoop                &m_loop;          ///< 所属事件循环
        Configuration                               m_configuration; ///< 配置
        Device::NodeRepository                     &m_nodes;         ///< 节点仓储
        TelemetryRepository                        &m_telemetry;     ///< 遥测仓储
        Alarm::AlarmService                        &m_alarms;        ///< 告警服务
        std::unique_ptr<AsynGyanis::Net::UdpServer> m_server;        ///< UDP 服务端
        UdpTelemetryReplayGuard                     m_replayGuard;   ///< 时间窗内的重放防护
    };
} // namespace EdgePulse::Telemetry
