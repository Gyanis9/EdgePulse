/**
 * @file CaptchaService.h
 * @brief 进程内一次性验证码的签发与消费
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

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
     * @brief 进程内验证码存储
     * @note P0 单进程有效；P1 迁移到 Redis 后本类由同一接口替换
     */
    class CaptchaService
    {
    public:
        using Clock = std::function<std::chrono::steady_clock::time_point()>;

        /**
         * @brief 构造
         * @param options 有效期与限流参数
         * @param clock 时钟注入；留空使用 steady_clock
         */
        explicit CaptchaService(CaptchaOptions options, Clock clock = {});

        /**
         * @brief 签发一条验证码
         * @param clientIp 来源 IP，用于限流
         * @return 成功返回 uuid、答案与 SVG；超限返回 CaptchaError
         */
        [[nodiscard]] std::expected<CaptchaChallenge, CaptchaError> issue(const std::string &clientIp);

        /**
         * @brief 消费一条验证码
         * @param uuid 签发时给出的标识
         * @param code 调用方提交的答案
         * @return true 答案正确且未过期
         * @note 无论对错都会删除该 uuid，防止同一验证码被反复试
         */
        [[nodiscard]] bool consume(std::string_view uuid, std::string_view code);

        /**
         * @brief 当前缓存条数，主要用于测试与诊断
         */
        [[nodiscard]] std::size_t size() const;

    private:
        struct Entry
        {
            std::string                           code;      ///< 答案
            std::chrono::steady_clock::time_point expiresAt; ///< 过期时刻
        };

        struct RateWindow
        {
            std::chrono::steady_clock::time_point windowStart; ///< 当前窗口起点
            int                                   count{0};    ///< 窗口内已签发数
        };

        [[nodiscard]] std::chrono::steady_clock::time_point now() const;

        /**
         * @brief 清掉过期验证码与旧限流窗口
         * @param moment 当前时刻
         * @note 只在缓存变大时调用，避免每次签发都扫全表
         */
        void pruneExpired(const std::chrono::steady_clock::time_point moment);

        CaptchaOptions                              m_options;     ///< 配置
        Clock                                       m_clock;       ///< 时钟
        mutable std::mutex                          m_mutex;       ///< 保护两张表
        std::unordered_map<std::string, Entry>      m_challenges;  ///< uuid -> 验证码
        std::unordered_map<std::string, RateWindow> m_rateWindows; ///< IP -> 限流窗口
    };
} // namespace EdgePulse::Auth
