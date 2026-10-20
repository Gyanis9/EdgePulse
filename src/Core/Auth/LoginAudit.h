/**
 * @file LoginAudit.h
 * @brief 登录审计落库
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Database/Database.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <memory>
#include <string>

namespace EdgePulse::Auth
{
    /**
     * @brief 一条登录审计记录
     */
    struct LoginAuditEntry
    {
        std::string  username;        ///< 登录名，失败时也记录
        std::string  ip;              ///< 来源 IP
        bool         success{false};  ///< 是否成功
        std::string  message;         ///< 结果说明，不含口令
        std::string  traceId;         ///< 请求标识
        std::int64_t userId{0};       ///< 账号标识；账号不存在时为 0
        std::int64_t departmentId{0}; ///< 归属部门；未知时为 0
    };

    /**
     * @brief 把登录结果写入 sys_login_log
     */
    class LoginAudit
    {
    public:
        /**
         * @brief 用数据库封装构造
         * @param database 进程级数据库
         */
        explicit LoginAudit(std::shared_ptr<Common::Database> database);

        /**
         * @brief 写一条审计
         * @param loop 恢复协程用的事件循环
         * @param entry 审计内容
         * @return 是否写入成功
         * @throws Common::DatabaseException 数据库写入失败
         */
        AsynGyanis::Core::Task<bool> record(AsynGyanis::Core::EventLoop &loop, LoginAuditEntry entry);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
    };
} // namespace EdgePulse::Auth
