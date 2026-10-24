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
     * @details 待启用的秘密加密后放在缓存里，启用时直接落库；TOTP 校验用条件更新推进步号，
     *          恢复码以 JSON 文本为乐观锁逐枚删除。未配置主密钥时只关闭设置与启用入口，
     *          状态、校验与管理员重置仍可安全执行。
     * @note 本服务不承担尝试次数限制。登录挑战的 5 次失败门禁与挑战绑定，由登录流程统一计数；
     *       这里每次校验都直接返回结果，另起一套计数器会与挑战口径冲突。
     * @note 恢复码以主密钥为键做 HMAC-SHA256 后落库，只有数据库泄露不足以离线还原 40 位随机码。
     */
    class MfaService
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         * @param cache 待启用秘密的键值存储
         * @param encryptionKey base64 编码的 32 字节主密钥；空串或非法值按未配置处理
         */
        MfaService(std::shared_ptr<Common::Database> database, std::shared_ptr<Cache::KeyValueStore> cache, std::string encryptionKey);

        /**
         * @brief 生成新秘密，加密后写入待启用缓存
         * @param loop 恢复协程用的事件循环
         * @param session 当前身份；秘密按该用户与其租户记录
         * @return 成功返回 base32 秘密与 otpauth URI；未配置主密钥返回 503，已启用返回 409
         * @throws Base::Exception 随机源不可用
         */
        AsynGyanis::Core::Task<std::expected<MfaSetup, MfaError>> beginSetup(AsynGyanis::Core::EventLoop &loop, SessionRecord session);

        /**
         * @brief 校验待启用秘密的验证码，加密落库并签发恢复码
         * @param loop 恢复协程用的事件循环
         * @param session 当前身份
         * @param code 身份验证器当前的 6 位验证码
         * @return 成功返回 10 枚明文恢复码，只出现这一次；设置过期返回 400，验证码错误返回 401，已启用返回 409
         * @note 命中步号会写成 last_used_step，setup 口令不能在启用后继续用于登录校验
         */
        AsynGyanis::Core::Task<std::expected<std::vector<std::string>, MfaError>> enable(AsynGyanis::Core::EventLoop &loop, SessionRecord session, std::string code);

        /**
         * @brief 校验 TOTP 后消费步号并删除记录
         * @param loop 恢复协程用的事件循环
         * @param session 当前身份
         * @param code 身份验证器当前的 6 位验证码
         * @return 成功为空；未启用返回 404，验证码错误或已消费返回 401，未配置主密钥返回 503
         * @note 与 verify 共用步号水位：登录时用过的口令不能再关闭两步验证
         */
        AsynGyanis::Core::Task<std::expected<void, MfaError>> disable(AsynGyanis::Core::EventLoop &loop, SessionRecord session, std::string code);

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
         * @brief 管理员重置：删除记录与待启用缓存
         * @param loop 恢复协程用的事件循环
         * @param userId 目标用户标识；调用方负责确认目标在当前租户内并持有 system:user:edit
         * @return bool 是否删掉一条记录
         * @note 按主键全租户删除，管理端控制器必须完成 system:user:edit 权限与租户校验
         */
        AsynGyanis::Core::Task<bool> reset(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

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
