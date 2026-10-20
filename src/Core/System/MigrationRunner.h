/**
 * @file MigrationRunner.h
 * @brief 版本化 SQL 迁移的装载与执行
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    class DatabaseConnection;
}

namespace EdgePulse::System
{
    /**
     * @brief 把一份 SQL 脚本按行尾分号拆成可逐条执行的语句
     * @param script SQL 文本
     * @return 去掉注释与空语句后的语句列表
     * @throws Common::MigrationException 引号或块注释没有闭合
     * @note 只处理本仓库使用的形状：单引号、双引号、-- 行注释与块注释
     */
    [[nodiscard]] std::vector<std::string> splitSqlStatements(std::string_view script);

    /**
     * @brief 按 V<版本>__<说明>.sql 的文件名顺序执行迁移
     */
    class MigrationRunner
    {
    public:
        /**
         * @brief 应用目录内尚未执行的迁移
         * @param connection 已连接的数据库
         * @param directory 迁移目录
         * @throws Common::MigrationException 文件解析、校验和不一致或语句执行失败
         * @note 每条语句各自提交；MySQL 的 DDL 隐式提交无法包成原子事务，这里不假装原子
         */
        static void apply(AsynGyanis::Database::DatabaseConnection &connection, const std::filesystem::path &directory);
    };
} // namespace EdgePulse::System
