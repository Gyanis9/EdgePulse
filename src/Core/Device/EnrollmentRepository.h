/**
 * @file EnrollmentRepository.h
 * @brief 一次性注册/改绑令牌的仓储
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Database/Database.h"
#include "Core/System/TenantScope.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace EdgePulse::Device
{
    /**
     * @brief 一条注册令牌记录；明文与哈希都不出现在这里
     */
    struct EnrollmentTokenRecord
    {
        std::int64_t id{0};        ///< 主键
        std::int64_t tenantId{1};  ///< 归属租户
        std::string  nodeName;     ///< 限定节点名；空表示任意节点
        std::string  action;       ///< enroll / rebind
        std::string  expiresAt;    ///< 到期时间，UTC
        std::string  usedAt;       ///< 消费时间，可空
        std::string  revokedAt;    ///< 吊销时间，可空
        std::int64_t createdBy{0}; ///< 签发人
        std::string  createdAt;    ///< 签发时间，UTC
    };

    /**
     * @brief 令牌的 SQL 访问
     */
    class EnrollmentRepository
    {
    public:
        /**
         * @brief 用数据库封装构造
         * @param database 进程级数据库
         * @param tenant 租户边界
         */
        explicit EnrollmentRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant = {});

        /// 按租户边界派生仓储视图
        [[nodiscard]] std::shared_ptr<EnrollmentRepository> scoped(System::TenantScope tenant) const;

        /**
         * @brief 写入一条令牌；明文哈希由调用方算好传入
         * @param loop 恢复协程用的事件循环
         * @param record 除主键外的记录字段
         * @param tokenHash 明文的 SHA-256 十六进制
         * @return 带主键的已落库记录
         */
        AsynGyanis::Core::Task<EnrollmentTokenRecord> create(AsynGyanis::Core::EventLoop &loop, EnrollmentTokenRecord record, std::string tokenHash);

        /**
         * @brief 按派生状态列出令牌
         * @param loop 恢复协程用的事件循环
         * @param status empty、active、used、expired 或 revoked
         * @return 按签发时间倒序的记录
         */
        AsynGyanis::Core::Task<std::vector<EnrollmentTokenRecord>> list(AsynGyanis::Core::EventLoop &loop, std::string status);

        /**
         * @brief 吊销一条尚未使用且未吊销的令牌
         * @param loop 恢复协程用的事件循环
         * @param tokenId 令牌标识
         * @return 是否命中并更新一行
         */
        AsynGyanis::Core::Task<bool> revoke(AsynGyanis::Core::EventLoop &loop, std::int64_t tokenId);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
        System::TenantScope               m_tenant;   ///< 租户边界
    };
} // namespace EdgePulse::Device
