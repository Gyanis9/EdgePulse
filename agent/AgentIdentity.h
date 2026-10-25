/**
 * @file AgentIdentity.h
 * @brief Agent 证书身份的加载与指纹、到期时间提取
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <expected>
#include <string>

namespace EdgePulse::Agent
{
    struct AgentConfiguration;

    /**
     * @brief 一次加载得到的证书身份
     */
    struct AgentIdentity
    {
        std::string fingerprint; ///< 叶证书 SHA-256 指纹，64 位小写十六进制
        std::string expiresAt;   ///< 到期时间，形如 2026-10-24T12:00:00Z
        std::string notBefore;   ///< 生效时间，形如 2026-10-24T12:00:00Z；与 expiresAt 一起决定生命周期

        /**
         * @brief 读取配置里的 CA、证书与私钥并提取身份信息
         * @param configuration 已通过形态校验的配置
         * @return 成功返回指纹与到期时间；失败返回中文原因
         * @note 只解析证书的到期时间，不检查是否已过期；私钥在 POSIX 上要求不向同组与其他用户开放读权限，
         *       Windows 上只记录警告。证书即将到期或已经过期时记录 WARN，但仍然加载成功。
         *       私钥内容不在这里解析，证书与私钥是否配对由注册时装载 TLS 客户端身份来判定。
         */
        [[nodiscard]] static std::expected<AgentIdentity, std::string> load(const AgentConfiguration &configuration);

        /**
         * @brief 重新读取磁盘上的证书与私钥并提取身份
         * @param configuration 已通过形态校验的配置
         * @return 成功返回新指纹与到期时间；失败返回中文原因
         * @note 证书与私钥文件被原地替换后用它刷新内存身份；口径与 load() 完全一致，
         *       包括 POSIX 上的私钥权限检查与即将到期的 WARN
         */
        [[nodiscard]] static std::expected<AgentIdentity, std::string> reload(const AgentConfiguration &configuration);
    };
} // namespace EdgePulse::Agent
