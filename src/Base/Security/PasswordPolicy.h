/**
 * @file PasswordPolicy.h
 * @brief 新建、重置与修改口令共用的强度策略
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace EdgePulse::Common
{
    /**
     * @brief 口令长度与字符类别约束
     * @note 只做策略判定，不接触哈希与存储；调用方负责把错误文案原样交给用户
     */
    class PasswordPolicy
    {
    public:
        /**
         * @brief 构造策略
         * @param minimumLength 最短长度，按 UTF-8 码点计；下限由调用方决定，生产配置范围为 8 到 64
         * @param requireComplexity 是否要求同时包含大写、小写、数字与符号
         * @note 上限固定为 64 个码点；服务端配置校验把下限限制在 8 到 64 之间
         */
        explicit PasswordPolicy(int minimumLength = 10, bool requireComplexity = true);

        /**
         * @brief 校验一条明文口令
         * @param password 待校验的明文
         * @return 成功为空；失败为中文可操作说明
         */
        [[nodiscard]] std::expected<void, std::string> validate(std::string_view password) const;

    private:
        int  m_minimumLength{10};       ///< 最短长度
        bool m_requireComplexity{true}; ///< 是否要求四类字符
    };
} // namespace EdgePulse::Common
