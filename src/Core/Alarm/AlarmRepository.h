/**
 * @file AlarmRepository.h
 * @brief 告警规则与告警生命周期仓储
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Api/Pagination.h"
#include "Base/Database/Database.h"
#include "Core/Telemetry/TelemetryModels.h"

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

namespace EdgePulse::Alarm
{
    /**
     * @brief 告警仓储
     */
    class AlarmRepository
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         */
        explicit AlarmRepository(std::shared_ptr<Common::Database> database);

        /**
         * @brief 分页查询规则
         * @param loop 恢复协程用的事件循环
         * @param keyword 关键字
         * @param page 分页
         * @return Common::PageRows<AlarmRuleRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<AlarmRuleRecord>> listRules(AsynGyanis::Core::EventLoop &loop, std::string keyword, const Common::PageParameters &page);

        /**
         * @brief 取全部启用规则
         * @param loop 恢复协程用的事件循环
         * @return std::vector<AlarmRuleRecord> 规则
         */
        AsynGyanis::Core::Task<std::vector<AlarmRuleRecord>> enabledRules(AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 创建规则
         * @param loop 恢复协程用的事件循环
         * @param rule 规则字段
         * @param operatorId 操作人
         * @return std::int64_t 新规则标识
         */
        AsynGyanis::Core::Task<std::int64_t> createRule(AsynGyanis::Core::EventLoop &loop, AlarmRuleRecord rule, std::int64_t operatorId);

        /**
         * @brief 更新规则
         * @param loop 恢复协程用的事件循环
         * @param ruleId 规则标识
         * @param rule 新字段
         * @param operatorId 操作人
         * @return bool 是否命中
         */
        AsynGyanis::Core::Task<bool> updateRule(AsynGyanis::Core::EventLoop &loop, std::int64_t ruleId, AlarmRuleRecord rule, std::int64_t operatorId);

        /**
         * @brief 删除规则
         * @param loop 恢复协程用的事件循环
         * @param ruleId 规则标识
         * @return bool 是否命中
         */
        AsynGyanis::Core::Task<bool> removeRule(AsynGyanis::Core::EventLoop &loop, std::int64_t ruleId);

        /**
         * @brief 查一条规则
         * @param loop 恢复协程用的事件循环
         * @param ruleId 规则标识
         * @return std::optional<AlarmRuleRecord> 规则
         */
        AsynGyanis::Core::Task<std::optional<AlarmRuleRecord>> findRule(AsynGyanis::Core::EventLoop &loop, std::int64_t ruleId);

        /**
         * @brief 查找某规则在某节点上的未恢复告警
         * @param loop 恢复协程用的事件循环
         * @param ruleId 规则
         * @param nodeId 节点
         * @return std::optional<AlarmRecord> 告警
         */
        AsynGyanis::Core::Task<std::optional<AlarmRecord>> findOpen(AsynGyanis::Core::EventLoop &loop, std::int64_t ruleId, std::int64_t nodeId);

        /**
         * @brief 插入一条未恢复告警
         * @param loop 恢复协程用的事件循环
         * @param rule 规则
         * @param nodeId 节点
         * @param value 触发值
         * @param message 说明
         * @return std::optional<AlarmRecord> 新告警
         */
        AsynGyanis::Core::Task<std::optional<AlarmRecord>> insertAlarm(AsynGyanis::Core::EventLoop &loop, AlarmRuleRecord rule, std::int64_t nodeId, double value,
                                                                       std::string message);

        /**
         * @brief 规则在冷却窗口内是否刚恢复过
         * @param loop 恢复协程用的事件循环
         * @param ruleId 规则
         * @param nodeId 节点
         * @param cooldownSeconds 冷却秒数
         * @return bool 是否在冷却中
         */
        AsynGyanis::Core::Task<bool> recentlyResolved(AsynGyanis::Core::EventLoop &loop, std::int64_t ruleId, std::int64_t nodeId, int cooldownSeconds);

        /**
         * @brief 分页查询告警
         * @param loop 恢复协程用的事件循环
         * @param status 状态过滤
         * @param level 级别过滤
         * @param nodeId 节点过滤，0 表示全部
         * @param page 分页
         * @return Common::PageRows<AlarmRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<AlarmRecord>> listAlarms(AsynGyanis::Core::EventLoop &loop, std::string status, std::string level, std::int64_t nodeId,
                                                                         const Common::PageParameters &page);

        /**
         * @brief 按标识取告警
         * @param loop 恢复协程用的事件循环
         * @param alarmId 告警标识
         * @return std::optional<AlarmRecord> 告警
         */
        AsynGyanis::Core::Task<std::optional<AlarmRecord>> findAlarm(AsynGyanis::Core::EventLoop &loop, std::int64_t alarmId);

        /**
         * @brief 确认告警
         * @param loop 恢复协程用的事件循环
         * @param alarmId 告警标识
         * @param operatorId 操作人
         * @return std::optional<AlarmRecord> 更新后的告警
         */
        AsynGyanis::Core::Task<std::optional<AlarmRecord>> acknowledge(AsynGyanis::Core::EventLoop &loop, std::int64_t alarmId, std::int64_t operatorId);

        /**
         * @brief 恢复告警
         * @param loop 恢复协程用的事件循环
         * @param alarmId 告警标识
         * @param operatorId 操作人
         * @return std::optional<AlarmRecord> 更新后的告警
         */
        AsynGyanis::Core::Task<std::optional<AlarmRecord>> resolve(AsynGyanis::Core::EventLoop &loop, std::int64_t alarmId, std::int64_t operatorId);

        /**
         * @brief 未恢复告警数
         * @param loop 恢复协程用的事件循环
         * @return std::int64_t 数量
         */
        AsynGyanis::Core::Task<std::int64_t> openCount(AsynGyanis::Core::EventLoop &loop);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
    };
} // namespace EdgePulse::Alarm
