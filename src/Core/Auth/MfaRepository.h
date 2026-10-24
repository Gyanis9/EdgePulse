/**
 * @file MfaRepository.h
 * @brief 用户两步验证行的 SQL 访问
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
     * @brief 一条用户两步验证记录；秘密只以密文形式存在
     */
    struct MfaRecord
    {
        std::int64_t             userId{0};        ///< 用户标识，主键
        std::int64_t             tenantId{1};      ///< 归属租户
        std::string              secretCiphertext; ///< 秘密密文与认证标签的 base64
        std::string              secretNonce;      ///< 12 字节 nonce 的 base64
        int                      keyVersion{1};    ///< 主密钥版本，供后续轮换
        std::int64_t             lastUsedStep{0};  ///< 上次成功校验的 TOTP 步号
        std::vector<std::string> recoveryHashes;   ///< 恢复码的 SHA-256 十六进制
        bool                     enabled{false};   ///< 是否已启用
        std::string              createdAt;        ///< 创建时间，UTC
        std::string              updatedAt;        ///< 更新时间，UTC
    };

    /**
     * @brief 两步验证仓储
     * @note 按用户主键的读取与管理员重置使用全租户视图，其余读写都带租户过滤；
     *       调用方必须保证传入的用户标识已经过身份或租户校验
     */
    class MfaRepository
    {
    public:
        /**
         * @brief 用数据库封装构造
         * @param database 进程级数据库
         * @param tenant 租户边界
         */
        explicit MfaRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant = {});

        /**
         * @brief 按租户边界派生仓储视图
         * @param tenant 租户边界
         * @return std::shared_ptr<MfaRepository> 共享同一数据库的视图
         */
        [[nodiscard]] std::shared_ptr<MfaRepository> scoped(System::TenantScope tenant) const;

        /**
         * @brief 按用户标识读取当前租户内的记录
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return std::optional<MfaRecord> 命中时返回记录
         */
        AsynGyanis::Core::Task<std::optional<MfaRecord>> findByUser(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 写入待启用的秘密；已启用行不会被覆盖
         * @param loop 恢复协程用的事件循环
         * @param record 除恢复码与启用状态外的字段
         * @throws Common::DatabaseException 归属租户越界、写入失败或并发启用导致主键冲突
         * @note 同一用户重复开始设置时覆盖上一条未启用记录；并发启用后的覆盖会以主键冲突失败，
         *       而不是把已启用账号静默降级
         */
        AsynGyanis::Core::Task<> upsertSecret(AsynGyanis::Core::EventLoop &loop, MfaRecord record);

        /**
         * @brief 把未启用行标记为启用并写入恢复码哈希
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @param recoveryCodesJson 恢复码哈希数组的 JSON
         * @param now 更新时间，UTC
         * @return bool 是否命中一条尚未启用的记录
         */
        AsynGyanis::Core::Task<bool> enable(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, std::string recoveryCodesJson, std::string now);

        /**
         * @brief 删除用户的记录，供关闭与管理员重置使用
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return bool 是否删掉一行
         */
        AsynGyanis::Core::Task<bool> remove(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 条件写入已消费的步号
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @param step 本次命中的步号
         * @param now 更新时间，UTC
         * @return bool 是否写入；并发请求已推进步号时返回 false，即重放拒绝
         */
        AsynGyanis::Core::Task<bool> consumeStep(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, std::int64_t step, std::string now);

        /**
         * @brief 以 JSON 文本为乐观锁删除一枚恢复码哈希
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @param hash 待删除的 SHA-256 十六进制
         * @param now 更新时间，UTC
         * @return bool 是否删除成功；哈希不存在或并发修改导致 CAS 失败时返回 false
         */
        AsynGyanis::Core::Task<bool> consumeRecoveryCode(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, std::string hash, std::string now);

        /**
         * @brief 整组替换恢复码哈希
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @param recoveryCodesJson 新的 JSON 数组
         * @param now 更新时间，UTC
         * @return bool 是否命中记录
         */
        AsynGyanis::Core::Task<bool> updateRecoveryCodes(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, std::string recoveryCodesJson, std::string now);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
        System::TenantScope               m_tenant;   ///< 租户边界
    };
} // namespace EdgePulse::Auth
