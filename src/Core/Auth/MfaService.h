/**
 * @file MfaService.h
 * @brief 两步验证的设置、启用、校验、恢复码与关闭
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Database/Database.h"
#include "Core/Auth/MfaRepository.h"
#include "Core/Auth/SessionStore.h"
#include "Core/Cache/KeyValueStore.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace EdgePulse::Auth
{
    /**
     * @brief 两步验证接口的可恢复错误
     */
    struct MfaError
    {
        int         httpStatus{400}; ///< HTTP 状态码
        std::string message;         ///< 中文可操作说明
    };

    /**
     * @brief 一次登录挑战的缓存内容
     * @note 挑战只放在键值存储里，落库的只有用户的两步验证记录
     */
    struct MfaChallenge
    {
        std::string  token;        ///< 一次性挑战令牌
        std::int64_t userId{0};    ///< 待验证的用户
        std::int64_t tenantId{0};  ///< 用户归属租户
        std::string  clientIp;     ///< 发起口令登录的来源 IP
        int          attempts{0};  ///< 已失败的验证次数
        std::int64_t expiresAt{0}; ///< 绝对过期时刻，Unix 秒
    };

    /**
     * @brief 登录挑战失败后的去向
     */
    enum class ChallengeFailureOutcome
    {
        Retained,     ///< 挑战按剩余时间与次数写回
        LimitReached, ///< 达到失败上限，挑战不再保留
        Expired,      ///< 挑战已过绝对有效期
        WriteFailed   ///< 挑战续期写入缓存失败
    };

    /**
     * @brief 开始设置时返回给用户的一次性材料
     */
    struct MfaSetup
    {
        std::string secret;     ///< base32 秘密，仅本次响应展示
        std::string otpauthUri; ///< 身份验证器可扫描的 otpauth URI
    };

    /**
     * @brief 当前用户的两步验证状态
     */
    struct MfaStatus
    {
        bool        enabled{false};            ///< 是否已启用
        std::size_t remainingRecoveryCodes{0}; ///< 剩余可用恢复码数量
    };

    /**
     * @brief 两步验证服务
     * @details 待启用的秘密加密后同时写入停用行与缓存，启用时用一次条件更新把密文、恢复码、步号与启用位一起落库；
     *          TOTP 校验用条件更新推进步号，恢复码以 JSON 文本为乐观锁逐枚删除。未配置主密钥时，
     *          开始设置、启用、关闭与换发恢复码返回 503，状态、校验与管理员重置仍可安全执行。
     * @note 登录挑战最多接受 5 次失败，随后连同缓存一起销毁；每次挑战失败同时记入按用户计数，签发新挑战前先检查该计数。
     * @note 持有会话的设置口令复核、启用、关闭与换发恢复码共用一份按用户的失败计数，键为
     *       edgepulse:mfa:attempt: 加用户标识，窗口 300 秒，最多 5 次，第 6 次起直接返回 429。
     *       登录挑战的验证码失败也计入同一份计数；只有完成第二因素校验或成功的管理操作才清零。
     * @note 恢复码以主密钥为键做 HMAC-SHA256 后落库，只有数据库泄露不足以离线还原 40 位随机码。
     */
    class MfaService
    {
    public:
        /// 登录挑战有效期，单位秒
        static constexpr int kChallengeTtlSeconds = 300;

        /// 登录挑战允许的最大失败次数
        static constexpr int kChallengeMaxAttempts = 5;

        /// 会话态入口（启用、关闭、换发恢复码）允许的最大失败次数
        static constexpr int kAttemptMaxFailures = 5;

        /// 会话态失败计数窗口，单位秒
        static constexpr int kAttemptTtlSeconds = 300;

        /**
         * @brief 构造
         * @param database 数据库封装
         * @param cache 待启用秘密的键值存储
         * @param encryptionKey base64 编码的 32 字节主密钥；空串或非法值按未配置处理
         */
        MfaService(std::shared_ptr<Common::Database> database, std::shared_ptr<Cache::KeyValueStore> cache, std::string encryptionKey);

        /**
         * @brief 创建登录挑战并写入缓存
         * @param loop 恢复协程用的事件循环
         * @param userId 已通过口令校验的用户
         * @param tenantId 用户归属租户
         * @param clientIp 来源 IP，校验时必须与登录请求一致
         * @return std::optional<std::string> 成功返回一次性令牌；随机源或缓存不可用、或按用户失败计数已达上限时为空
         * @note 挑战的有效期是签发时刻起算的 300 秒绝对时长，失败重写不会延长
         * @note 调用方在为空时应先复查 attemptLimitReached，命中计数上限要回 429 而不是服务不可用
         */
        AsynGyanis::Core::Task<std::optional<std::string>> createChallenge(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, std::int64_t tenantId, std::string clientIp);

        /**
         * @brief 原子取出并删除挑战
         * @param loop 恢复协程用的事件循环
         * @param token 一次性令牌
         * @return std::optional<MfaChallenge> 命中时返回挑战内容；不存在、过期或格式不符时为空
         * @note 取值与删除原子完成；调用方在每个令牌上只会有一次校验机会
         */
        AsynGyanis::Core::Task<std::optional<MfaChallenge>> takeChallenge(AsynGyanis::Core::EventLoop &loop, std::string_view token);

        /**
         * @brief 记录一次失败并把挑战按剩余时间与次数重写回缓存
         * @param loop 恢复协程用的事件循环
         * @param challenge 本次取出的挑战
         * @return ChallengeFailureOutcome 写回、达到上限、已过期或写入失败的区分
         * @note 有效期按挑战里的绝对过期时刻计算，失败不会把 300 秒重新往后推
         */
        AsynGyanis::Core::Task<ChallengeFailureOutcome> recordChallengeFailure(AsynGyanis::Core::EventLoop &loop, MfaChallenge challenge);

        /**
         * @brief 生成新秘密，加密后写入待启用缓存与一条停用记录
         * @param loop 恢复协程用的事件循环
         * @param session 当前身份；秘密按该用户与其租户记录
         * @return 成功返回 base32 秘密与 otpauth URI；未配置主密钥返回 503，已启用或并发状态变化返回 409
         * @throws Base::Exception 随机源不可用
         * @note 启用时需要一条已存在的停用记录，因此本方法在返回秘密前先把密文写进 sys_user_mfa
         */
        AsynGyanis::Core::Task<std::expected<MfaSetup, MfaError>> beginSetup(AsynGyanis::Core::EventLoop &loop, SessionRecord session);

        /**
         * @brief 校验待启用秘密的验证码，加密落库并签发恢复码
         * @param loop 恢复协程用的事件循环
         * @param session 当前身份
         * @param code 身份验证器当前的 6 位验证码
         * @return 成功返回 10 枚明文恢复码，只出现这一次；设置过期返回 400，验证码错误返回 401，已启用或并发状态变化返回 409，
         *         失败次数用尽返回 429
         * @note 密文、恢复码、步号与启用位由一次条件更新写入，任一步失败都不会留下半启用的行
         * @note 命中步号会写成 last_used_step，setup 口令不能在启用后继续用于登录校验
         * @note 调用方必须先复核当前口令；本方法只负责验证码与失败计数
         */
        AsynGyanis::Core::Task<std::expected<std::vector<std::string>, MfaError>> enable(AsynGyanis::Core::EventLoop &loop, SessionRecord session, std::string code);

        /**
         * @brief 校验 TOTP 后消费步号并删除记录
         * @param loop 恢复协程用的事件循环
         * @param session 当前身份
         * @param code 身份验证器当前的 6 位验证码
         * @return 成功为空；未启用返回 404，验证码错误或已消费返回 401，未配置主密钥返回 503，
         *         失败次数用尽返回 429
         * @note 与 verify 共用步号水位：登录时用过的口令不能再关闭两步验证
         * @note 调用方必须先复核当前口令；本方法只负责验证码与失败计数
         */
        AsynGyanis::Core::Task<std::expected<void, MfaError>> disable(AsynGyanis::Core::EventLoop &loop, SessionRecord session, std::string code);

        /**
         * @brief 校验 TOTP 后整组替换恢复码
         * @param loop 恢复协程用的事件循环
         * @param session 当前身份
         * @param code 身份验证器当前的 6 位验证码
         * @return 成功返回 10 枚新恢复码，只出现这一次；未启用返回 404，验证码错误或已消费返回 401，
         *         失败次数用尽返回 429
         * @note 命中步号会推进 last_used_step，生成恢复码的验证码不能继续用于登录
         */
        AsynGyanis::Core::Task<std::expected<std::vector<std::string>, MfaError>> regenerateRecoveryCodes(AsynGyanis::Core::EventLoop &loop, SessionRecord session,
                                                                                                          std::string code);

        /**
         * @brief 校验 TOTP 并推进已消费步号
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识，来自已确认的登录身份
         * @param code 6 位验证码
         * @return bool 验证码命中窗口内尚未消费的步号时为 true；未启用、密钥缺失、解密失败或重放均为 false
         * @note 按主键读记录，调用方必须保证该用户已经过登录或会话认证
         */
        AsynGyanis::Core::Task<bool> verify(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, std::string code);

        /**
         * @brief 消费一枚恢复码
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @param code 明文恢复码，大小写不敏感
         * @return bool 命中并成功删除一枚哈希时为 true；第二次提交同一枚返回 false
         * @note 按主键读记录，调用方必须保证该用户已经过登录或会话认证
         */
        AsynGyanis::Core::Task<bool> verifyRecovery(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, std::string code);

        /**
         * @brief 查询是否已启用
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return bool 记录存在且 enabled 为真
         * @note 按主键读记录，调用方必须保证该用户已经过登录或会话认证
         */
        AsynGyanis::Core::Task<bool> enabled(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 查询启用状态与剩余恢复码数量
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return MfaStatus 未启用时两项为 false 与 0
         * @note 按主键读记录，调用方必须保证该用户已经过登录或会话认证
         */
        AsynGyanis::Core::Task<MfaStatus> status(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 管理员重置：删除记录、待启用缓存与该用户的失败计数
         * @param loop 恢复协程用的事件循环
         * @param userId 目标用户标识；调用方负责确认目标在当前租户内并持有 system:user:edit
         * @return bool 是否删掉一条记录
         * @note 按主键全租户删除，管理端控制器必须完成 system:user:edit 权限与租户校验
         */
        AsynGyanis::Core::Task<bool> reset(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 查询按用户的失败计数是否已到上限
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return bool 计数不小于 kAttemptMaxFailures 时为 true
         * @note 登录挑战、设置口令复核、启用、关闭与换发恢复码共用这一份计数
         */
        AsynGyanis::Core::Task<bool> attemptLimitReached(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 记录一次第二因素或口令复核失败，计数窗口首次创建时开始计时
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @note 调用方拿到失败结果后立即记录；成功完成的操作由服务内部清零
         */
        AsynGyanis::Core::Task<> recordAttemptFailure(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 清空按用户的失败计数
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         */
        AsynGyanis::Core::Task<> clearAttemptFailures(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

    private:
        /**
         * @brief 判断主密钥是否可用
         * @return bool 构造时成功解出 32 字节时为 true
         */
        [[nodiscard]] bool keyConfigured() const noexcept;

        /**
         * @brief 解密记录里的秘密
         * @param record 两步验证记录
         * @return std::optional<std::string> 成功返回 base32 秘密；密钥缺失或认证失败为空
         */
        [[nodiscard]] std::optional<std::string> decryptSecret(const MfaRecord &record) const;

        /**
         * @brief 在 ±1 步窗口内找出验证码对应的步号
         * @param secret base32 秘密
         * @param code 用户输入的验证码
         * @param nowStep 当前步号
         * @param lastUsedStep 上次消费的步号，候选必须严格更大
         * @return std::optional<std::int64_t> 命中时返回步号
         */
        [[nodiscard]] std::optional<std::int64_t> matchingStepForSecret(std::string_view secret, std::string_view code, std::int64_t nowStep, std::int64_t lastUsedStep) const;

        /**
         * @brief 解密记录后找出验证码对应的步号
         * @param record 两步验证记录
         * @param code 用户输入的验证码
         * @param nowStep 当前步号
         * @param lastUsedStep 上次消费的步号，候选必须严格更大
         * @return std::optional<std::int64_t> 命中时返回步号
         */
        [[nodiscard]] std::optional<std::int64_t> matchingStep(const MfaRecord &record, std::string_view code, std::int64_t nowStep, std::int64_t lastUsedStep) const;

        std::shared_ptr<Cache::KeyValueStore> m_cache;      ///< 待启用秘密缓存
        MfaRepository                         m_repository; ///< 两步验证仓储
        std::string                           m_key;        ///< 解码后的 32 字节主密钥；空表示未配置
    };
} // namespace EdgePulse::Auth
