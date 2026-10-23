/**
 * @file SqlRetry.h
 * @brief SQL 并发失败与唯一键冲突的分类
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <string_view>

namespace EdgePulse::Sql
{
    /**
     * @brief 判断数据库失败是否属于可在新事务里重试的一类
     * @param message 驱动或仓储给出的错误文本
     * @return true 表示锁冲突、死锁或唯一键冲突；其余错误应原样抛出
     * @note 匹配按 ASCII 大小写不敏感，覆盖 SQLite 的 database is locked/busy 与 MySQL 的 deadlock/duplicate entry
     */
    [[nodiscard]] bool isRetryableSqlFailure(std::string_view message);

    /**
     * @brief 判断数据库失败是否来自唯一键冲突
     * @param message 驱动或仓储给出的错误文本
     * @return true 表示 SQLite 的 UNIQUE 或 MySQL 的 duplicate entry
     */
    [[nodiscard]] bool isUniqueViolation(std::string_view message);
} // namespace EdgePulse::Sql
