/**
 * @file CaptchaService.h
 * @brief 一次性验证码的签发与消费
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Cache/KeyValueStore.h"

#include <Core/Coroutine/Task.h>

#include <atomic>
#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Auth
{
    /**
     * @brief 验证码配置
     */
    struct CaptchaOptions
    {
        int ttlSeconds{120};       ///< 有效期
        int requestsPerMinute{30}; ///< 单 IP 每分钟签发上限
    };

    /**
     * @brief 一次签发的结果
     */
    struct CaptchaChallenge
    {
        std::string uuid; ///< 一次性标识
        std::string code; ///< 答案，仅开发档可回给调用方
        std::string svg;  ///< 直接可渲染的 SVG
    };

    /**
     * @brief 签发被拒的原因
     */
    enum class CaptchaErrorReason
    {
        RateLimited, ///< 该 IP 在窗口内签发次数超限
    };

    /**
     * @brief 签发失败
     */
    struct CaptchaError
    {
        CaptchaErrorReason reason;  ///< 机器可判的原因
        std::string        message; ///< 中文可操作说明
    };

    /**
     * @brief 验证码服务
     * @details 验证码与限流计数都放键值存储，内存与 Redis 后端行为一致；
     *          本对象不持有时钟，过期由存储后端的 TTL 负责。
     */
    class CaptchaService
    {
    public:
        /**
         * @brief 构造
         * @param store 键值存储
         * @param options 有效期与限流参数
         */
        CaptchaService(std::shared_ptr<Cache::KeyValueStore> store, CaptchaOptions options);

        /**
         * @brief 签发一条验证码
         * @param loop 恢复协程用的事件循环
         * @param clientIp 来源 IP，用于限流
         * @return std::expected<CaptchaChallenge, CaptchaError> 挑战或限流原因
         */
        [[nodiscard]] AsynGyanis::Core::Task<std::expected<CaptchaChallenge, CaptchaError>> issue(AsynGyanis::Core::EventLoop &loop, std::string clientIp);

        /**
         * @brief 消费一条验证码
         * @param loop 恢复协程用的事件循环
         * @param uuid 签发时给出的标识
         * @param code 调用方提交的答案
         * @return bool 答案正确且未过期；无论对错该 uuid 都会被删除
         */
        [[nodiscard]] AsynGyanis::Core::Task<bool> consume(AsynGyanis::Core::EventLoop &loop, std::string_view uuid, std::string_view code);

        /**
         * @brief 运行期更新验证码配置
         * @param options 新配置；非正数字段被忽略
         */
        void setOptions(CaptchaOptions options) noexcept;

    private:
        std::shared_ptr<Cache::KeyValueStore> m_store;             ///< 键值存储
        std::atomic<int>                      m_ttlSeconds;        ///< 有效期
        std::atomic<int>                      m_requestsPerMinute; ///< 单 IP 每分钟上限
    };
} // namespace EdgePulse::Auth
