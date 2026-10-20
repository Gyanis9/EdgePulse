/**
 * @file EdgePulseException.h
 * @brief 应用层运行期异常的统一基类
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <stdexcept>
#include <string>

namespace EdgePulse::Common
{
    /**
     * @brief EdgePulse 运行期故障基类
     * @details 携带干净的中文消息；what() 只含这条消息，不含引擎的抛出点坐标。
     */
    class EdgePulseException : public std::runtime_error
    {
    public:
        /**
         * @brief 用中文消息构造
         * @param message 面向日志的原因，不含凭据
         */
        explicit EdgePulseException(const std::string &message) : std::runtime_error(message)
        {
        }
    };

    /**
     * @brief 数据库打开、查询或连接池故障
     */
    class DatabaseException : public EdgePulseException
    {
    public:
        using EdgePulseException::EdgePulseException;
    };

    /**
     * @brief 迁移文件读取、校验或执行失败
     */
    class MigrationException : public EdgePulseException
    {
    public:
        using EdgePulseException::EdgePulseException;
    };
} // namespace EdgePulse::Common
