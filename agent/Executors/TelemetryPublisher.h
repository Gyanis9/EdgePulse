/**
 * @file TelemetryPublisher.h
 * @brief 遥测上报与离线补报
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "TelemetryCollector.h"

#include "Core/Coroutine/Task.h"

#include <cstdint>
#include <vector>

namespace EdgePulse::Agent
{
    class AgentClient;
    class AgentState;

    /**
     * @brief 把采集到的样本直接上报，失败时落入本地队列
     * @details 直报遇可重试失败时逐条入队，遇终态 4xx 时丢弃本批并记 ERROR；
     *          补报按入队顺序每批最多 500 条，只有 HTTP 200 才确认并删除对应行。
     *          损坏的队列正文会被删除并记 ERROR，避免永久堵塞队列。
     *          同一时刻只由遥测循环的一条协程调用，不额外加锁。
     */
    class TelemetryPublisher
    {
    public:
        /**
         * @brief 装配上报器
         * @param client 服务端客户端
         * @param state 本地状态库，提供遥测离线队列
         */
        TelemetryPublisher(AgentClient &client, AgentState &state);

        /**
         * @brief 上报一批新采集的样本
         * @param nodeId 节点标识
         * @param samples 本轮样本，最多 500 条
         * @note 可重试失败按样本逐条入队并记 WARN；终态 4xx 丢弃本批并记 ERROR
         */
        AsynGyanis::Core::Task<> publish(std::int64_t nodeId, const std::vector<TelemetrySample> &samples);

        /**
         * @brief 按 FIFO 补报离线队列，直到队列为空或遇到可重试失败
         * @param nodeId 节点标识
         * @note 每批最多 500 条；损坏行先删除再发送其余行；终态 4xx 删除该批后继续下一批
         */
        AsynGyanis::Core::Task<> flush(std::int64_t nodeId);

    private:
        AgentClient &m_client; ///< 服务端客户端
        AgentState  &m_state;  ///< 本地状态库
    };
} // namespace EdgePulse::Agent
