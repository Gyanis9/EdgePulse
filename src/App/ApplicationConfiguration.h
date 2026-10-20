/**
 * @file ApplicationConfiguration.h
 * @brief 应用配置的加载与校验入口
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace EdgePulse::App
{
    /**
     * @brief 配置加载失败的原因
     */
    struct ConfigurationError
    {
        std::string message; ///< 中文可操作说明，含出错键名或路径
    };

    /**
     * @brief 应用配置：非敏感项来自 YAML，秘密项只从环境变量读取
     */
    struct ApplicationConfiguration
    {
        std::filesystem::path    configurationFile;                         ///< 主配置路径
        std::string              listenAddress{"127.0.0.1"};                ///< 控制面与开发口的监听地址
        std::uint16_t            httpPort{8080};                            ///< 开发档明文 HTTP
        std::uint16_t            httpsPort{8443};                           ///< 控制台 HTTPS/h2 与 h3
        std::uint16_t            agentPort{9443};                           ///< Agent mTLS
        std::uint16_t            opsPort{9100};                             ///< 运维口
        std::string              opsAddress{"127.0.0.1"};                   ///< 运维口监听地址
        std::string              databaseDriver{"sqlite"};                  ///< sqlite 或 mysql
        std::string              databasePath{"var/edgepulse.db"};          ///< SQLite 文件路径
        std::string              databaseHost{"127.0.0.1"};                 ///< MySQL 主机
        std::uint16_t            databasePort{3306};                        ///< MySQL 端口
        std::string              databaseName{"edgepulse"};                 ///< MySQL 库名
        std::string              databaseUser{"edgepulse"};                 ///< MySQL 用户
        std::string              databasePassword;                          ///< MySQL 口令，只从环境变量读取
        bool                     redisEnabled{false};                       ///< 是否使用 Redis 会话
        std::string              redisHost{"127.0.0.1"};                    ///< Redis 主机
        std::uint16_t            redisPort{6379};                           ///< Redis 端口
        int                      redisDatabase{0};                          ///< Redis 键空间
        std::string              redisPassword;                             ///< Redis 口令，只从环境变量读取
        int                      threads{1};                                ///< 事件循环线程数
        std::string              migrationsDirectory{"migrations"};         ///< SQL 迁移目录
        std::string              webRoot{"web"};                            ///< 静态站点根目录
        std::string              firmwareDirectory{"var/firmware"};         ///< 固件落盘目录
        std::int64_t             maximumFirmwareBytes{256LL * 1024 * 1024}; ///< 单份固件上限
        int                      telemetryRetentionDays{30};                ///< 遥测保留天数
        std::string              certificateDirectory{"var/certs"};         ///< 开发证书目录
        std::string              opsTokenFile{"var/ops-token"};             ///< 运维口令牌文件
        int                      captchaTtlSeconds{120};                    ///< 验证码有效期，单位秒
        int                      captchaRequestsPerMinute{30};              ///< 单 IP 每分钟签发上限
        int                      sessionTtlSeconds{7200};                   ///< 会话有效期，单位秒
        bool                     exposeCaptchaCode{false};                  ///< 开发档在响应里带验证码答案
        std::vector<std::string> allowedOrigins;                            ///< 浏览器 WebSocket 的 Origin 白名单
        std::string              agentFingerprint;                          ///< 允许的 Agent 证书 SHA-256 指纹，空表示只校验 CA
        bool                     udpTelemetryEnabled{false};                ///< 是否启用 UDP 遥测监听
        std::string              udpTelemetryAddress{"0.0.0.0"};            ///< UDP 遥测监听地址
        std::uint16_t            udpTelemetryPort{9555};                    ///< UDP 遥测端口
        std::string              udpTelemetrySecret;                        ///< HMAC 共享密钥，只从环境变量读取

        /**
         * @brief 读取 YAML 并组装配置；同目录存在 logging.yaml 时一并载入
         * @param configurationPath 主配置路径
         * @return 成功返回完整配置；失败返回中文原因，调用方可直接打印
         * @note 只读配置键与做范围校验，不在这里启动任何资源
         */
        [[nodiscard]] static std::expected<ApplicationConfiguration, ConfigurationError> load(const std::filesystem::path &configurationPath);
    };
} // namespace EdgePulse::App
