/**
 * @file ApiTokenRepository.h
 * @brief API 令牌行的 SQL 访问
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
#include <optional>
#include <string>
#include <vector>

namespace EdgePulse::Auth
{
    /**
     * @brief 一条 API 令牌记录；明文只存在于创建或轮换的返回值里
     */
    struct ApiTokenRecord
    {
        std::int64_t             id{0};              ///< 主键
        std::int64_t             tenantId{1};        ///< 归属租户
        std::int64_t             ownerUserId{0};     ///< 所属用户，权限按该用户实时计算
        std::string              name;               ///< 管理名
        std::string              tokenPrefix;        ///< 明文前 12 个字符，供列表辨识
        std::string              tokenHash;          ///< 明文的 SHA-256 十六进制
        std::string              previousHash;       ///< 轮换前的哈希，空表示没有重叠窗口
        std::string              previousValidUntil; ///< 旧哈希可用截止时间，UTC
        std::vector<std::string> scopes;             ///< 权限范围，必须是 owner 权限的子集
        std::string              expiresAt;          ///< 到期时间，UTC
        std::string              lastUsedAt;         ///< 最近一次成功鉴权时间，UTC
        std::string              revokedAt;          ///< 吊销时间，UTC
        std::int64_t             createdBy{0};       ///< 创建人
        std::string              createdAt;          ///< 创建时间，UTC
    };

    /**
     * @brief API 令牌仓储
     * @note 租户边界作用于创建、按主键查询、列表与轮换；按哈希反查供鉴权使用全租户视图
     */
    class ApiTokenRepository
    {
    public:
        /**
         * @brief 用数据库封装构造
         * @param database 进程级数据库
         * @param tenant 租户边界
         */
        explicit ApiTokenRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant = {});

        /**
         * @brief 按租户边界派生仓储视图
         * @param tenant 租户边界
         * @return std::shared_ptr<ApiTokenRepository> 共享同一数据库的视图
         */
        [[nodiscard]] std::shared_ptr<ApiTokenRepository> scoped(System::TenantScope tenant) const;

        /**
         * @brief 写入一条令牌；哈希与范围由调用方算好传入
         * @param loop 恢复协程用的事件循环
         * @param record 除主键与生命周期列外的字段
         * @return ApiTokenRecord 带主键的已落库记录
         * @throws Common::DatabaseException 归属租户越界或写入失败
         */
        AsynGyanis::Core::Task<ApiTokenRecord> create(AsynGyanis::Core::EventLoop &loop, ApiTokenRecord record);

        /**
         * @brief 按主键查一条当前租户内的令牌
         * @param loop 恢复协程用的事件循环
         * @param id 令牌标识
         * @return std::optional<ApiTokenRecord> 命中时返回记录
         */
        AsynGyanis::Core::Task<std::optional<ApiTokenRecord>> findById(AsynGyanis::Core::EventLoop &loop, std::int64_t id);

        /**
         * @brief 按哈希查一条仍可用的令牌
         * @param loop 恢复协程用的事件循环
         * @param hash 明文的 SHA-256 十六进制
         * @param now 当前 UTC 时间，用于判断到期与重叠窗口
         * @return std::optional<ApiTokenRecord> 先查当前哈希，再查重叠哈希；未命中返回空
         */
        AsynGyanis::Core::Task<std::optional<ApiTokenRecord>> findByHash(AsynGyanis::Core::EventLoop &loop, std::string hash, std::string now);

        /**
         * @brief 列出当前租户内的全部令牌，包含已吊销与已过期行
         * @param loop 恢复协程用的事件循环
         * @return std::vector<ApiTokenRecord> 按创建时间倒序的记录
         */
        AsynGyanis::Core::Task<std::vector<ApiTokenRecord>> list(AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 标记吊销，不物理删除
         * @param loop 恢复协程用的事件循环
         * @param id 令牌标识
         * @param revokedAt 吊销时间，UTC
         * @return bool 是否命中一条尚未吊销的记录
         */
        AsynGyanis::Core::Task<bool> revoke(AsynGyanis::Core::EventLoop &loop, std::int64_t id, std::string revokedAt);

        /**
         * @brief 轮换当前哈希，并把旧哈希写入重叠窗口
         * @param loop 恢复协程用的事件循环
         * @param id 令牌标识
         * @param expectedHash 调用方读到的当前哈希，用于拒绝并发轮换
         * @param newHash 新明文的 SHA-256 十六进制
         * @param newPrefix 新明文的前 12 个字符
         * @param previousValidUntil 旧哈希可用截止时间，UTC
         * @param now 当前 UTC 时间；到期时间不晚于它的行不再更新
         * @return bool 是否命中且当前哈希仍等于期望值、令牌仍未过期
         */
        AsynGyanis::Core::Task<bool> replaceHash(AsynGyanis::Core::EventLoop &loop, std::int64_t id, std::string expectedHash, std::string newHash, std::string newPrefix,
                                                 std::string previousValidUntil, std::string now);

        /**
         * @brief 条件写入最近使用时间
         * @param loop 恢复协程用的事件循环
         * @param tenantId 归属租户，写路径也显式带边界
         * @param id 令牌标识
         * @param usedAt 本次鉴权时间，UTC
         * @param cutoff 节流下限；最近使用时间晚于它时不写库
         * @return bool 是否发生一次写入
         */
        AsynGyanis::Core::Task<bool> touchLastUsed(AsynGyanis::Core::EventLoop &loop, std::int64_t tenantId, std::int64_t id, std::string usedAt, std::string cutoff);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
        System::TenantScope               m_tenant;   ///< 租户边界
    };
} // namespace EdgePulse::Auth
