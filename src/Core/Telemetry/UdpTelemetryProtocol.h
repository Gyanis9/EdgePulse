/**
 * @file UdpTelemetryProtocol.h
 * @brief UDP 遥测数据报的认证、解析与重放防护
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Telemetry/TelemetryModels.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace EdgePulse::Telemetry
{
    /**
     * @brief 一条通过认证的 UDP 遥测数据报
     */
    struct UdpTelemetryDatagram
    {
        std::int64_t                 nodeId{0};    ///< 节点
        std::int64_t                 timestamp{0}; ///< 发送时刻，Unix 秒
        std::string                  nonce;        ///< 一次性随机串
        std::vector<TelemetrySample> samples;      ///< 样本
    };

    /**
     * @brief 解析结果
     */
    struct UdpTelemetryParseResult
    {
        bool                 ok{false};           ///< 认证、时间窗与样本都合法
        bool                 unauthorized{false}; ///< 签名或时间窗不合法，调用方只回笼统原因
        std::string          error;               ///< 中文原因；认证失败时为空
        UdpTelemetryDatagram datagram;            ///< ok 为真时有效
    };

    /**
     * @brief 校验并解析一条数据报
     * @param payload 数据报原文
     * @param secret HMAC 共享密钥
     * @param nowSeconds 当前 Unix 秒
     * @return UdpTelemetryParseResult 结果
     * @note 签名覆盖版本、节点、时间戳、随机串与 payload 的规范化 JSON
     */
    [[nodiscard]] UdpTelemetryParseResult parseUdpTelemetryDatagram(std::string_view payload, std::string_view secret, std::int64_t nowSeconds);

    /**
     * @brief 时间窗内的重放防护
     * @note 归 UDP 收包循环独占，不加锁
     */
    class UdpTelemetryReplayGuard
    {
    public:
        /**
         * @brief 构造
         * @param windowSeconds 时间窗；只保留两倍窗口内的随机串
         */
        explicit UdpTelemetryReplayGuard(std::int64_t windowSeconds = 120);

        /**
         * @brief 登记一个随机串
         * @return bool 首次出现为 true；重复或表已满为 false
         */
        bool accept(std::int64_t nodeId, std::string_view nonce, std::int64_t timestamp, std::int64_t nowSeconds);

        /// 当前保留的随机串数量
        [[nodiscard]] std::size_t size() const noexcept
        {
            return m_seen.size();
        }

    private:
        static constexpr std::size_t kMaximumEntries = 65536;

        void prune(std::int64_t nowSeconds);

        std::int64_t                                  m_windowSeconds{120};
        std::unordered_map<std::string, std::int64_t> m_seen; ///< 节点:随机串 -> 时间戳
    };
} // namespace EdgePulse::Telemetry
