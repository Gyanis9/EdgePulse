/**
 * @file SqlExecution.h
 * @brief 把驱动返回的空结果统一翻成带原因的异常
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/EdgePulseException.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace EdgePulse::Sql
{
    /**
     * @brief 执行查询；驱动报错时抛出带原文的 DatabaseException
     * @param connection 连接
     * @param sql SQL 文本
     * @param parameters 参数
     * @return std::unique_ptr<AsynGyanis::Database::DatabaseResult> 结果集
     */
    [[nodiscard]] inline std::unique_ptr<AsynGyanis::Database::DatabaseResult> executeOrThrow(AsynGyanis::Database::DatabaseConnection &connection, const std::string_view sql,
                                                                                              const std::span<const AsynGyanis::Database::DatabaseValue> parameters = {})
    {
        std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = parameters.empty() ? connection.execute(std::string{sql}) : connection.execute(std::string{sql}, parameters);
        if (!result)
        {
            throw Common::DatabaseException("执行 SQL 失败：" + connection.lastError());
        }
        return result;
    }

    /**
     * @brief 执行写语句并丢弃结果集
     * @param connection 连接
     * @param sql SQL 文本
     * @param parameters 参数
     */
    inline void executeOrThrowVoid(AsynGyanis::Database::DatabaseConnection &connection, const std::string_view sql,
                                   const std::span<const AsynGyanis::Database::DatabaseValue> parameters = {})
    {
        static_cast<void>(executeOrThrow(connection, sql, parameters));
    }
} // namespace EdgePulse::Sql
