/**
 * @file LogRepository.h
 * @brief 操作日志、登录日志、统计与工作台数据
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Api/Pagination.h"
#include "Base/Database/Database.h"
#include "Core/System/SystemModels.h"

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace EdgePulse::System
{
    /**
     * @brief 登录统计
     */
    struct LoginStats
    {
        std::int64_t            total{0};  ///< 登录次数
        std::int64_t            failed{0}; ///< 失败次数
        std::int64_t            users{0};  ///< 登录账号数
        std::int64_t            ips{0};    ///< 来源 IP 数
        std::vector<TimeBucket> series;    ///< 趋势桶
    };

    /**
     * @brief 操作统计
     */
    struct OperStats
    {
        std::int64_t            total{0};       ///< 操作数
        std::int64_t            failed{0};      ///< 失败数
        std::int64_t            activeUsers{0}; ///< 活跃账号数
        double                  avgCost{0.0};   ///< 平均耗时毫秒
        double                  p95Cost{0.0};   ///< 95 分位耗时毫秒
        std::vector<TimeBucket> series;         ///< 趋势桶
    };

    /**
     * @brief 泳道时间块
     */
    struct TimelineBlock
    {
        std::int64_t              startSec{0}; ///< 当日起始秒
        std::int64_t              endSec{0};   ///< 当日结束秒
        std::int64_t              count{0};    ///< 次数
        std::int64_t              failed{0};   ///< 失败数
        std::int64_t              maxCost{0};  ///< 最大耗时
        std::vector<std::int64_t> ids;         ///< 日志标识，最多五十个
    };

    /**
     * @brief 泳道
     */
    struct TimelineLane
    {
        std::string                module;   ///< 模块
        std::int64_t               count{0}; ///< 总次数
        std::vector<TimelineBlock> blocks;   ///< 时间块
    };

    /**
     * @brief 当日操作泳道
     */
    struct OperTimeline
    {
        std::string                 date;   ///< 日期
        std::optional<std::int64_t> nowSec; ///< 当前秒数；非当天为空
        std::vector<TimelineLane>   lanes;  ///< 泳道
    };

    /**
     * @brief 登录热力单元
     */
    struct HeatCell
    {
        int          dayOfWeek{0}; ///< 0 周一
        int          hour{0};      ///< 小时
        std::int64_t total{0};     ///< 次数
        std::int64_t failed{0};    ///< 失败数
    };

    /**
     * @brief 登录异常来源
     */
    struct HeatAnomaly
    {
        std::string  kind;      ///< ip 或 user
        std::string  key;       ///< 来源 IP 或账号
        std::int64_t failed{0}; ///< 失败数
        std::int64_t total{0};  ///< 总数
        std::int64_t spread{0}; ///< 关联账号数或来源数
        std::string  firstTime; ///< 首次出现
        std::string  lastTime;  ///< 最后出现
    };

    /**
     * @brief 登录热力与异常
     */
    struct LoginHeatmap
    {
        int                      days{7};      ///< 天数
        std::string              since;        ///< 起始时间
        std::int64_t             threshold{5}; ///< 异常阈值
        std::vector<HeatCell>    cells;        ///< 星期×小时单元
        std::vector<HeatAnomaly> anomalies;    ///< 异常来源
    };

    /**
     * @brief 日志查询的数据范围
     */
    struct LogScope
    {
        bool                      all{true};        ///< 可见全部
        bool                      selfOnly{false};  ///< 仅本人
        std::int64_t              userId{0};        ///< 当前账号
        std::vector<std::int64_t> deptIds;          ///< 可见部门
        std::int64_t              tenantId{1};      ///< 归属租户
        bool                      allTenants{true}; ///< 是否跨租户（仅平台管理员）
    };

    /**
     * @brief 日志仓储
     */
    class LogRepository
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         */
        explicit LogRepository(std::shared_ptr<Common::Database> database);

        /**
         * @brief 写一条操作日志
         * @param loop 恢复协程用的事件循环
         * @param record 日志行
         * @return bool 是否写入成功
         */
        AsynGyanis::Core::Task<bool> insertOperLog(AsynGyanis::Core::EventLoop &loop, OperLogRecord record);

        /**
         * @brief 分页查询操作日志
         * @param loop 恢复协程用的事件循环
         * @param keyword 关键字
         * @param result 空、0、1
         * @param page 分页
         * @return Common::PageRows<OperLogRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<OperLogRecord>> listOperLogs(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string result,
                                                                             const Common::PageParameters &page, LogScope scope = {});

        /**
         * @brief 导出操作日志
         * @param loop 恢复协程用的事件循环
         * @param keyword 关键字
         * @param result 结果
         * @return std::vector<OperLogRecord> 行
         */
        AsynGyanis::Core::Task<std::vector<OperLogRecord>> exportOperLogs(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string result, LogScope scope = {});

        /**
         * @brief 删除一条操作日志
         * @param loop 恢复协程用的事件循环
         * @param id 日志标识
         * @param scope 租户与数据范围
         * @return bool 是否命中
         */
        AsynGyanis::Core::Task<bool> removeOperLog(AsynGyanis::Core::EventLoop &loop, std::int64_t id, const LogScope &scope);

        /**
         * @brief 清空可见范围内的操作日志
         * @param loop 恢复协程用的事件循环
         * @param scope 租户与数据范围
         * @return bool 是否成功
         */
        AsynGyanis::Core::Task<bool> clearOperLogs(AsynGyanis::Core::EventLoop &loop, const LogScope &scope);

        /**
         * @brief 分页查询登录日志
         * @param loop 恢复协程用的事件循环
         * @param keyword 关键字
         * @param status 空、0、1
         * @param page 分页
         * @return Common::PageRows<LoginLogRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<LoginLogRecord>> listLoginLogs(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status,
                                                                               const Common::PageParameters &page, LogScope scope = {});

        /**
         * @brief 导出登录日志
         * @param loop 恢复协程用的事件循环
         * @param keyword 关键字
         * @param status 状态
         * @return std::vector<LoginLogRecord> 行
         */
        AsynGyanis::Core::Task<std::vector<LoginLogRecord>> exportLoginLogs(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status, LogScope scope = {});

        /**
         * @brief 删除一条登录日志
         * @param loop 恢复协程用的事件循环
         * @param id 日志标识
         * @param scope 租户与数据范围
         * @return bool 是否命中
         */
        AsynGyanis::Core::Task<bool> removeLoginLog(AsynGyanis::Core::EventLoop &loop, std::int64_t id, const LogScope &scope);

        /**
         * @brief 清空可见范围内的登录日志
         * @param loop 恢复协程用的事件循环
         * @param scope 租户与数据范围
         * @return bool 是否成功
         */
        AsynGyanis::Core::Task<bool> clearLoginLogs(AsynGyanis::Core::EventLoop &loop, const LogScope &scope);

        /**
         * @brief 按保留天数清理操作日志与登录日志
         * @param loop 恢复协程用的事件循环
         * @param retentionDays 保留天数，非正数不清理
         * @return bool 是否执行了清理
         */
        AsynGyanis::Core::Task<bool> prune(AsynGyanis::Core::EventLoop &loop, int retentionDays);

        /**
         * @brief 登录统计
         * @param loop 恢复协程用的事件循环
         * @param range 24h、7d、30d
         * @return LoginStats 统计
         */
        AsynGyanis::Core::Task<LoginStats> loginStats(AsynGyanis::Core::EventLoop &loop, std::string range, LogScope scope = {});

        /**
         * @brief 操作统计
         * @param loop 恢复协程用的事件循环
         * @param range 24h、7d、30d
         * @return OperStats 统计
         */
        AsynGyanis::Core::Task<OperStats> operStats(AsynGyanis::Core::EventLoop &loop, std::string range, LogScope scope = {});

        /**
         * @brief 当日操作泳道
         * @param loop 恢复协程用的事件循环
         * @param date 日期 YYYY-MM-DD；空表示今天
         * @return OperTimeline 泳道
         */
        AsynGyanis::Core::Task<OperTimeline> operTimeline(AsynGyanis::Core::EventLoop &loop, std::string date, LogScope scope = {});

        /**
         * @brief 登录热力与异常
         * @param loop 恢复协程用的事件循环
         * @param days 7 或 30
         * @return LoginHeatmap 热力
         */
        AsynGyanis::Core::Task<LoginHeatmap> loginHeatmap(AsynGyanis::Core::EventLoop &loop, int days, LogScope scope = {});

        /**
         * @brief 近 N 天每日操作量
         * @param loop 恢复协程用的事件循环
         * @param days 天数
         * @return std::vector<TimeBucket> 每日桶
         */
        AsynGyanis::Core::Task<std::vector<TimeBucket>> dailyOper(AsynGyanis::Core::EventLoop &loop, int days, LogScope scope = {});

        /**
         * @brief 最近登录记录，供工作台活动流
         * @param loop 恢复协程用的事件循环
         * @param limit 条数
         * @return std::vector<LoginLogRecord> 记录
         */
        AsynGyanis::Core::Task<std::vector<LoginLogRecord>> recentLogins(AsynGyanis::Core::EventLoop &loop, int limit, LogScope scope = {});

        /**
         * @brief 指定时间窗内的登录次数
         * @param loop 恢复协程用的事件循环
         * @param from SQL DATETIME 起点
         * @param to SQL DATETIME 终点
         * @return std::int64_t 次数
         */
        AsynGyanis::Core::Task<std::int64_t> loginCountBetween(AsynGyanis::Core::EventLoop &loop, std::string from, std::string to, LogScope scope = {});

        /**
         * @brief 指定时间窗内的操作次数
         * @param loop 恢复协程用的事件循环
         * @param from SQL DATETIME 起点
         * @param to SQL DATETIME 终点
         * @return std::int64_t 次数
         */
        AsynGyanis::Core::Task<std::int64_t> operCountBetween(AsynGyanis::Core::EventLoop &loop, std::string from, std::string to, LogScope scope = {});

        /**
         * @brief 指定时刻之后出现过的登录账号数
         * @param loop 恢复协程用的事件循环
         * @param since SQL DATETIME 起点
         * @return std::int64_t 账号数
         */
        AsynGyanis::Core::Task<std::int64_t> distinctLoginUsersSince(AsynGyanis::Core::EventLoop &loop, std::string since, LogScope scope = {});

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
    };
} // namespace EdgePulse::System
