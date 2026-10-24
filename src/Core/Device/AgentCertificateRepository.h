/**
 * @file AgentCertificateRepository.h
 * @brief Agent 证书签发历史仓储
 * @author Gyanis
 * @date 2026-10-25
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
     * @brief 一条 Agent 证书签发历史
     */
    struct AgentCertificateRecord
    {
        std::int64_t id{0};       ///< 主键
        std::int64_t tenantId{1}; ///< 归属租户
        std::int64_t nodeId{0};   ///< 节点标识
        std::string  serial;      ///< 证书序列号
        std::string  fingerprint; ///< 证书 SHA-256，小写
        std::string  subject;     ///< 证书主题
        std::string  notBefore;   ///< 生效时间，UTC
        std::string  notAfter;    ///< 到期时间，UTC
        std::string  status;      ///< active / previous / revoked
        std::string  issuedAt;    ///< 签发时间，UTC
        std::string  revokedAt;   ///< 吊销时间，可空
    };

    /**
     * @brief Agent 证书历史的 SQL 访问
     */
    class AgentCertificateRepository
    {
    public:
        /**
         * @brief 用数据库封装构造
         * @param database 进程级数据库
         * @param tenant 租户边界
         */
        explicit AgentCertificateRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant = {});

        /// 按租户边界派生仓储视图
        [[nodiscard]] std::shared_ptr<AgentCertificateRepository> scoped(System::TenantScope tenant) const;

        /**
         * @brief 按节点列出证书签发历史
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点标识
         * @return 按签发时间倒序的历史，最多 200 条；租户边界外的节点返回空
         */
        AsynGyanis::Core::Task<std::vector<AgentCertificateRecord>> listByNode(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
        System::TenantScope               m_tenant;   ///< 租户边界
    };
} // namespace EdgePulse::Device
