/**
 * @file OpsRepository.h
 * @brief 运维总览只读查询
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Database/Database.h"

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <memory>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Ops
{
    /**
     * @brief 运维读数
     */
    struct OpsSummary
    {
        std::int64_t nodeTotal{0};      ///< 节点总数
        std::int64_t nodeOnline{0};     ///< 在线节点
        std::int64_t alarmOpen{0};      ///< 未恢复告警
        std::int64_t telemetry24h{0};   ///< 近 24 小时遥测条数
        std::int64_t commandTotal{0};   ///< 命令总数
        std::int64_t commandPending{0}; ///< 待执行/执行中命令
    };

    /**
     * @brief 运维只读仓储
     */
    class OpsRepository
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         */
        explicit OpsRepository(std::shared_ptr<Common::Database> database);

        /**
         * @brief 读取总览
         * @param loop 恢复协程用的事件循环
         * @return OpsSummary 读数
         */
        AsynGyanis::Core::Task<OpsSummary> summary(AsynGyanis::Core::EventLoop &loop);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
    };
} // namespace EdgePulse::Ops
