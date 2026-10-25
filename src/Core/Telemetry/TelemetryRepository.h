/**
 * @file TelemetryRepository.h
 * @brief 遥测读写仓储
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Database/Database.h"
#include "Core/Telemetry/TelemetryModels.h"
#include "Core/System/TenantScope.h"

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Telemetry
{
    /**
     * @brief 节点遥测的最新时间戳
     */
    struct TelemetryFreshness
    {
        std::string createdAt;   ///< 最新一条的服务端接收时间
        std::string collectedAt; ///< 最新一条的采集时间
    };

    /**
     * @brief 遥测仓储
     */
    class TelemetryRepository
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         */
        explicit TelemetryRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant = {});

        /// 按租户边界派生仓储视图
        [[nodiscard]] std::shared_ptr<TelemetryRepository> scoped(System::TenantScope tenant) const;

        /**
         * @brief 节点遥测最新一条的服务端接收时间与采集时间
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点
         * @return std::optional<TelemetryFreshness> 没有任何遥测时为空
         * @note 两个时间各自取最大值，不要求来自同一行；门控只关心节点最近是否仍有数据上报
         */
        AsynGyanis::Core::Task<std::optional<TelemetryFreshness>> newestTimestamps(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId);

        /**
         * @brief 节点最新一条遥测的接收时间与采集时间
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点
         * @return std::optional<TelemetryFreshness> 没有任何遥测时为空
         * @note 两个时间来自同一行；门控必须用一条样本同时证明接收与采集都发生在窗口之后
         */
        AsynGyanis::Core::Task<std::optional<TelemetryFreshness>> newestSample(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId);

        /**
         * @brief 批量写入遥测
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点
         * @param samples 样本
         * @return bool 是否成功
         */
        AsynGyanis::Core::Task<bool> insertBatch(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId, std::vector<TelemetrySample> samples);

        /**
         * @brief 每个指标的最新读数
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点
         * @return std::vector<TelemetryPoint> 读数
         */
        AsynGyanis::Core::Task<std::vector<TelemetryPoint>> latest(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId);

        /**
         * @brief 时间序列
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点
         * @param metric 指标
         * @param from SQL DATETIME 起点
         * @param to SQL DATETIME 终点
         * @param limit 最多返回点数
         * @return std::vector<TelemetryPoint> 读数，按时间升序
         */
        AsynGyanis::Core::Task<std::vector<TelemetryPoint>> series(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId, std::string metric, std::string from, std::string to,
                                                                   int limit);

        /**
         * @brief 按保留天数清理
         * @param loop 恢复协程用的事件循环
         * @param retentionDays 保留天数
         * @return bool 是否成功
         */
        AsynGyanis::Core::Task<bool> prune(AsynGyanis::Core::EventLoop &loop, int retentionDays);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
        System::TenantScope               m_tenant;   ///< 租户边界
    };
} // namespace EdgePulse::Telemetry
