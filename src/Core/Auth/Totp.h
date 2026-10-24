/**
 * @file Totp.h
 * @brief RFC 6238 TOTP、RFC 4648 base32 秘密编码与 otpauth URI 生成
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace EdgePulse::Auth
{
    /**
     * @brief 两步验证的时间型一次性口令
     * @details 只实现身份验证器通用的那一档：SHA-1、30 秒步长、6 位输出、校验窗口 ±1 步。
     *          SHA-1 是 RFC 6238 与各家验证器的互操作基线，不是这里可替换的强度选项。
     */
    namespace Totp
    {
        /// 秘密字节数；20 字节对应 base32 编码后的 32 个字符
        inline constexpr std::size_t kSecretByteCount = 20;

        /// 默认输出位数
        inline constexpr int kDefaultDigits = 6;

        /// 默认步长，单位秒
        inline constexpr int kDefaultPeriodSeconds = 30;

        /// 默认校验窗口，前后各允许一步
        inline constexpr int kDefaultWindow = 1;

        /**
         * @brief 按 RFC 4648 编码 base32，不带填充
         * @param bytes 待编码字节
         * @return std::string 大写字母与 2 到 7 组成的文本
         */
        [[nodiscard]] std::string base32Encode(std::span<const std::uint8_t> bytes);

        /**
         * @brief 解码 RFC 4648 base32
         * @param text 待解码文本；大小写均可，忽略空白与末尾填充符
         * @return std::optional<std::vector<std::uint8_t>> 合法时返回字节；非法字符、残缺组或非零补位返回空
         */
        [[nodiscard]] std::optional<std::vector<std::uint8_t>> base32Decode(std::string_view text);

        /**
         * @brief 生成 20 字节随机秘密并以 base32 展示
         * @return std::string 32 个字符的秘密
         * @throws Base::Exception 随机源不可用
         */
        [[nodiscard]] std::string generateSecret();

        /**
         * @brief 当前时间所在的步号
         * @return std::int64_t Unix 秒数除以 30 的整数商
         */
        [[nodiscard]] std::int64_t currentStep();

        /**
         * @brief 计算某一步的 TOTP
         * @param secret base32 秘密
         * @param step 步号
         * @param digits 输出位数，1 到 9；默认 6
         * @return std::string 左侧补零的 digits 位数字；秘密非法或位数越界时为空串
         * @throws Base::Exception OpenSSL HMAC 失败
         */
        [[nodiscard]] std::string code(std::string_view secret, std::int64_t step, int digits = kDefaultDigits);

        /**
         * @brief 校验 TOTP 并拒绝已消费的步号
         * @param secret base32 秘密
         * @param code 用户输入的 6 位数字
         * @param nowStep 当前步号
         * @param lastUsedStep 上次成功校验的步号；候选步号必须严格大于它
         * @param window 前后允许的步数，默认 1
         * @return bool 命中窗口内尚未使用的步号时为 true
         */
        [[nodiscard]] bool verify(std::string_view secret, std::string_view code, std::int64_t nowStep, std::int64_t lastUsedStep, int window = kDefaultWindow);

        /**
         * @brief 生成身份验证器可扫描的 otpauth URI
         * @param issuer 签发者，例如 EdgePulse
         * @param account 账号，例如登录名
         * @param secret base32 秘密
         * @param digits 输出位数
         * @param periodSeconds 步长，单位秒
         * @return std::string otpauth://totp/... 文本；账号与签发者按 RFC 3986 百分号编码
         */
        [[nodiscard]] std::string otpauthUri(std::string_view issuer, std::string_view account, std::string_view secret, int digits = kDefaultDigits,
                                             int periodSeconds = kDefaultPeriodSeconds);
    } // namespace Totp
} // namespace EdgePulse::Auth
