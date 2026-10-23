/**
 * @file EnrollmentService.h
 * @brief 一次性注册/改绑令牌的签发、查询与吊销
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Device/EnrollmentRepository.h"
#include "Core/System/TenantScope.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace EdgePulse::Device
{
    /**
     * @brief 签发结果：明文只在这里出现一次
     */
    struct IssuedEnrollmentToken
    {
        EnrollmentTokenRecord record;    ///< 已落库的记录
        std::string           plaintext; ///< 只返回一次
    };

    /**
     * @brief 令牌业务服务
     */
    class EnrollmentService
    {
    public:
        /**
         * @brief 用仓储构造
         * @param repository 令牌仓储
         */
        explicit EnrollmentService(std::shared_ptr<EnrollmentRepository> repository);

        /// 按租户边界派生服务视图
        [[nodiscard]] std::shared_ptr<EnrollmentService> scoped(System::TenantScope tenant) const;

        /**
         * @brief 签发一枚一次性令牌
         * @param loop 恢复协程用的事件循环
         * @param tenantId 归属租户
         * @param nodeName 限定节点名；空表示任意节点
         * @param action enroll 或 rebind
         * @param expiresInSeconds 有效期，300 到 2592000 秒
         * @param operatorId 签发人
         * @return 成功返回带明文的结果；参数不合法时返回中文原因
         */
        AsynGyanis::Core::Task<std::expected<IssuedEnrollmentToken, std::string>> create(AsynGyanis::Core::EventLoop &loop, std::int64_t tenantId, std::string nodeName,
                                                                                         std::string action, int expiresInSeconds, std::int64_t operatorId);

        /**
         * @brief 按派生状态列出令牌
         * @param loop 恢复协程用的事件循环
         * @param status empty、active、used、expired 或 revoked
         * @return 按签发时间倒序的记录；不含明文与哈希
         */
        AsynGyanis::Core::Task<std::vector<EnrollmentTokenRecord>> list(AsynGyanis::Core::EventLoop &loop, std::string status);

        /**
         * @brief 吊销一枚尚未使用的令牌
         * @return 是否命中并更新一行
         */
        AsynGyanis::Core::Task<bool> revoke(AsynGyanis::Core::EventLoop &loop, std::int64_t tokenId);

        /**
         * @brief 计算令牌明文的存储摘要
         * @param token 明文
         * @return std::string SHA-256 十六进制
         */
        [[nodiscard]] static std::string hashToken(std::string_view token);

    private:
        std::shared_ptr<EnrollmentRepository> m_repository; ///< 令牌仓储
    };
} // namespace EdgePulse::Device
