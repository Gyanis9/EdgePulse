/**
 * @file IdempotencyRepository.h
 * @brief 写请求幂等记录的 SQL 访问
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Database/Database.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <memory>
#include <string>

namespace EdgePulse::Web
{
    /**
     * @brief 一次已完成响应，用于同键同指纹的重放
     */
    struct IdempotencyReplay
    {
        int         status{0};   ///< 首次响应的 HTTP 状态码
        std::string traceId;     ///< 首次响应的 x-request-id
        std::string contentType; ///< 首次响应的 content-type
        std::string body;        ///< 首次响应正文
    };

    /**
     * @brief begin() 的四种结局
     */
    enum class IdempotencyState
    {
        Started,   ///< 已插入占位，调用方应执行业务
        Replay,    ///< 同键同指纹已完成，应当重放
        Conflict,  ///< 同键但请求指纹不同
        InProgress ///< 同键同指纹仍在处理中
    };

    /**
     * @brief begin() 的结果；只有 Replay 会填充 replay 字段
     */
    struct IdempotencyBeginResult
    {
        IdempotencyState  state{IdempotencyState::Started}; ///< 本次占位的结局
        IdempotencyReplay replay;                           ///< Replay 时的首次响应
    };

    /**
     * @brief sys_idempotency 表的读写入口
     * @note 唯一约束负责并发去重，SQL 只用 SQLite 与 MySQL 都接受的语法
     */
    class IdempotencyRepository
    {
    public:
        /**
         * @brief 构造
         * @param database 进程级数据库
         */
        explicit IdempotencyRepository(std::shared_ptr<Common::Database> database);

        /**
         * @brief 尝试为本次写请求占位
         * @param loop 恢复协程用的事件循环
         * @param tokenHash Bearer 令牌明文的 SHA-256
         * @param keyHash Idempotency-Key 的 SHA-256
         * @param requestHash 方法、路径与正文的联合指纹
         * @param method 请求方法，落库用于诊断
         * @param path 请求路径，落库用于诊断
         * @param expiresAt 占位到期时间，SQL DATETIME UTC 文本
         * @return IdempotencyBeginResult 占位成功、可重放、冲突或处理中
         * @throws Common::DatabaseException 删除过期行、插入或回读失败
         */
        [[nodiscard]] AsynGyanis::Core::Task<IdempotencyBeginResult> begin(AsynGyanis::Core::EventLoop &loop, std::string tokenHash, std::string keyHash, std::string requestHash,
                                                                           std::string method, std::string path, std::string expiresAt);

        /**
         * @brief 把占位改成已完成并保存响应
         * @param loop 恢复协程用的事件循环
         * @param tokenHash Bearer 令牌明文的 SHA-256
         * @param keyHash Idempotency-Key 的 SHA-256
         * @param status 首次响应的 HTTP 状态码
         * @param traceId 首次响应的 x-request-id
         * @param contentType 首次响应的 content-type
         * @param body 首次响应正文
         * @note 占位已被清掉时不做任何事，避免覆盖别的请求刚写入的记录
         * @throws Common::DatabaseException 更新失败
         */
        AsynGyanis::Core::Task<> complete(AsynGyanis::Core::EventLoop &loop, std::string tokenHash, std::string keyHash, int status, std::string traceId, std::string contentType,
                                          std::string body);

        /**
         * @brief 撤销尚未完成的占位，让客户端可以重试
         * @param loop 恢复协程用的事件循环
         * @param tokenHash Bearer 令牌明文的 SHA-256
         * @param keyHash Idempotency-Key 的 SHA-256
         * @note 只删除 in_progress 行，已完成的记录仍可供重放
         * @throws Common::DatabaseException 删除失败
         */
        AsynGyanis::Core::Task<> abort(AsynGyanis::Core::EventLoop &loop, std::string tokenHash, std::string keyHash);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
    };
} // namespace EdgePulse::Web
