/**
 * @file AgentState.h
 * @brief Agent 本地状态库：键值状态、遥测与命令结果离线队列
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    class DatabaseConnection;
} // namespace AsynGyanis::Database

namespace EdgePulse::Agent
{
    /**
     * @brief 一条待上报的遥测记录
     */
    struct QueuedItem
    {
        std::int64_t id{0};     ///< 队列内自增标识，确认投递时按它删除
        std::string  payload;   ///< 遥测正文
        std::string  createdAt; ///< 入队时刻，UTC
    };

    /**
     * @brief 一条待上报的命令结果
     */
    struct QueuedResult
    {
        std::int64_t id{0};        ///< 队列内自增标识，确认投递时按它删除
        std::int64_t commandId{0}; ///< 服务端命令标识，结果按它去重
        std::string  payload;      ///< 结果正文
        std::string  createdAt;    ///< 入队时刻，UTC
    };

    /**
     * @brief 单行升级状态
     */
    struct UpdateStateRecord
    {
        std::string version;        ///< 目标版本，空表示没有进行中的升级
        std::string previousBinary; ///< 升级前二进制路径
        std::string healthyAt;      ///< 新版本写出健康标记的时刻，空表示尚未确认
        std::string pendingResult;  ///< 升级待上报结果，空表示没有
    };

    /**
     * @brief 进程内的 SQLite 状态库
     * @details 单条连接由内部互斥量串行化；写操作包在立即取写锁的事务里，
     *          遇到锁竞争按有界次数重试，普通 SQL 与约束错误只记录日志，不向调用方抛出。
     *          队列读取只取指定条数，确认投递成功后才删除。
     */
    class AgentState
    {
    public:
        /**
         * @brief 打开或创建状态库、建表并清理过期遥测
         * @param databasePath 状态库文件路径，父目录不存在时自动创建
         * @return 成功返回已就绪的状态库；失败返回中文原因
         */
        [[nodiscard]] static std::expected<AgentState, std::string> open(const std::filesystem::path &databasePath);

        AgentState(AgentState &&other) noexcept;

        AgentState &operator=(AgentState &&other) noexcept;

        AgentState(const AgentState &) = delete;

        AgentState &operator=(const AgentState &) = delete;

        ~AgentState();

        /**
         * @brief 读取一个状态键
         * @param key 状态键名
         * @return 键存在时返回文本；键缺失或读取失败时为空
         */
        std::optional<std::string> get(std::string_view key);

        /**
         * @brief 写入一个状态键，已存在时覆盖
         * @param key 状态键名
         * @param value 状态文本
         * @note 写失败只记录 ERROR 日志，不改变调用方流程
         */
        void set(std::string_view key, std::string_view value);

        /**
         * @brief 追加一条遥测到离线队列
         * @param payload 遥测正文
         * @note 队列超过上限时丢弃最旧的一条并记录 WARN
         */
        void enqueueTelemetry(std::string payload);

        /**
         * @brief 读取最多 limit 条遥测，不删除
         * @param limit 本次最多读取的条数，超过队列上限时按上限截断
         * @return 按入队顺序排列的遥测记录；读取失败时为空
         */
        std::vector<QueuedItem> takeTelemetry(std::size_t limit);

        /**
         * @brief 删除一条已确认投递的遥测
         * @param id 队列内自增标识
         */
        void ackTelemetry(std::int64_t id);

        /**
         * @brief 在一个事务里删除多条已确认投递的遥测
         * @param ids 队列内自增标识列表
         * @return 成功为空；列表超过单批上限或删除失败时返回中文原因
         * @note 空列表直接成功，不打开写事务；单批上限 500 条，低于 SQLite 单语句绑定参数上限
         */
        [[nodiscard]] std::expected<void, std::string> ackTelemetryBatch(const std::vector<std::int64_t> &ids);

        /**
         * @brief 追加一条命令结果
         * @param commandId 服务端命令标识
         * @param payload 结果正文
         * @note 同一命令只保留第一条结果；队列超过上限时丢弃最旧的一条并记录 WARN
         */
        void enqueueResult(std::int64_t commandId, std::string payload);

        /**
         * @brief 判断某个命令是否已有待上报的结果
         * @param commandId 服务端命令标识
         * @return true 表示结果队列里已有该命令的结果
         * @note 查询失败时按已有结果处理并记录 ERROR：状态不可知时重复执行比漏执行更危险
         */
        bool hasResult(std::int64_t commandId);

        /**
         * @brief 读取最多 limit 条命令结果，不删除
         * @param limit 本次最多读取的条数，超过队列上限时按上限截断
         * @return 按入队顺序排列的结果记录；读取失败时为空
         */
        std::vector<QueuedResult> takeResults(std::size_t limit);

        /**
         * @brief 删除一条已确认投递的命令结果
         * @param id 队列内自增标识
         */
        void ackResult(std::int64_t id);

        /**
         * @brief 读取整行升级状态
         * @return 成功返回四个字段；读取失败时返回中文原因
         * @note 表里始终有一行，空串表示该字段没有内容
         */
        [[nodiscard]] std::expected<UpdateStateRecord, std::string> updateState();

        /**
         * @brief 覆盖整行升级状态
         * @param record 目标版本、上一版二进制、健康时刻与待上报结果
         * @return 成功为空；写入失败时返回中文原因
         * @note 与逐字段写入不同，本方法把失败交回调用方：升级前写不进状态就不能启动安装器
         */
        [[nodiscard]] std::expected<void, std::string> setUpdateState(const UpdateStateRecord &record);

        /**
         * @brief 读取上次升级遗留的待上报结果
         * @return 有内容时返回文本；没有待上报内容时为空
         */
        std::optional<std::string> pendingUpdateResult();

        /**
         * @brief 记录升级待上报结果，已有内容时覆盖
         * @param text 待上报文本
         */
        void setPendingUpdateResult(std::string text);

        /**
         * @brief 读取遥测离线队列当前条数
         * @return 条数；查询失败时记录 ERROR 并返回 0
         */
        std::size_t telemetryQueueDepth();

        /**
         * @brief 读取命令结果队列当前条数
         * @return 条数；查询失败时记录 ERROR 并返回 0
         */
        std::size_t resultQueueDepth();

    private:
        /**
         * @brief 接管一条已连接的 SQLite 连接
         * @param connection 已连接的状态库连接
         */
        explicit AgentState(std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection);

        /**
         * @brief 设置连接级 PRAGMA、创建表结构并清理过期遥测
         * @return 成功为空；失败返回中文原因
         */
        std::expected<void, std::string> initialize();

        /**
         * @brief 删除超过保留期的遥测队列项
         * @return 成功为空；失败返回中文原因
         */
        std::expected<void, std::string> cleanupTelemetry();

        std::unique_ptr<AsynGyanis::Database::DatabaseConnection> m_connection; ///< 状态库的唯一连接
        std::unique_ptr<std::mutex>                               m_mutex;      ///< 串行化所有连接访问
    };
} // namespace EdgePulse::Agent
