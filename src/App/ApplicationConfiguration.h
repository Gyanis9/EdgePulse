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
        std::filesystem::path    configurationFile;                                   ///< 主配置路径
        std::string              listenAddress{"127.0.0.1"};                          ///< 控制面与开发口的监听地址
        std::uint16_t            httpPort{8080};                                      ///< 开发档明文 HTTP
        std::uint16_t            httpsPort{8443};                                     ///< 控制台 HTTPS/h2 与 h3
        std::uint16_t            agentPort{9443};                                     ///< Agent mTLS
        std::uint16_t            opsPort{9100};                                       ///< 运维口
        std::string              opsAddress{"127.0.0.1"};                             ///< 运维口监听地址
        std::string              databaseDriver{"sqlite"};                            ///< sqlite 或 mysql
        std::string              databasePath{"var/edgepulse.db"};                    ///< SQLite 文件路径
        std::string              databaseHost{"127.0.0.1"};                           ///< MySQL 主机
        std::uint16_t            databasePort{3306};                                  ///< MySQL 端口
        std::string              databaseName{"edgepulse"};                           ///< MySQL 库名
        std::string              databaseUser{"edgepulse"};                           ///< MySQL 用户
        std::string              databasePassword;                                    ///< MySQL 口令，只从环境变量读取
        std::string              instanceId;                                          ///< 实例标识；空值时装载阶段生成 hostname-pid-random
        bool                     haEnabled{false};                                    ///< 是否按多实例 HA 部署；为 false 时单实例仍运行租约
        bool                     redisEnabled{false};                                 ///< 是否使用 Redis 会话
        std::string              redisHost{"127.0.0.1"};                              ///< Redis 主机
        std::uint16_t            redisPort{6379};                                     ///< Redis 端口
        int                      redisDatabase{0};                                    ///< Redis 键空间
        std::string              redisPassword;                                       ///< Redis 口令，只从环境变量读取
        int                      threads{1};                                          ///< 事件循环线程数
        std::string              migrationsDirectory{"migrations"};                   ///< SQL 迁移目录
        std::string              webRoot{"web"};                                      ///< 静态站点根目录
        std::string              firmwareDirectory{"var/firmware"};                   ///< 固件落盘目录
        std::int64_t             maximumFirmwareBytes{256LL * 1024 * 1024};           ///< 单份固件上限
        bool                     firmwareRequireSignature{false};                     ///< 是否拒绝未签名固件的上传与下发
        std::string              firmwareTrustedKeysDirectory{"config/firmware-keys"}; ///< 固件公钥目录，文件名即 key id
        int                      telemetryRetentionDays{30};                          ///< 遥测保留天数
        int                      logRetentionDays{90};                                ///< 操作与登录日志保留天数
        int                      deliveryRetentionDays{30};                           ///< 通知投递记录保留天数
        std::string              certificateDirectory{"var/certs"};                   ///< 开发证书目录
        std::string              opsTokenFile{"var/ops-token"};                       ///< 运维口令牌文件
        int                      captchaTtlSeconds{120};                              ///< 验证码有效期，单位秒
        int                      captchaRequestsPerMinute{30};                        ///< 单 IP 每分钟签发上限
        int                      sessionTtlSeconds{7200};                             ///< 会话有效期，单位秒
        bool                     exposeCaptchaCode{false};                            ///< 开发档在响应里带验证码答案
        std::vector<std::string> allowedOrigins;                                      ///< 浏览器 WebSocket 的 Origin 白名单
        bool                     allowPrivateWebhooks{false};                         ///< 是否放行普通私网、回环、CGNAT、组播、保留与文档示例段；云元数据及链路本地地址始终拒绝
        std::string              agentFingerprint;                                    ///< 允许的 Agent 证书 SHA-256 指纹，空表示只校验 CA
        std::string              enrollmentMode{"off"};                               ///< 注册模式：off 或 required
        bool                     agentCaEnabled{false};                               ///< 是否启用控制面内部 CA 自动签发 Agent 证书
        std::string              agentCaCertificateFile{"var/certs/ca.pem"};          ///< Agent CA 证书链 PEM
        std::string              agentCaKeyFile{"var/certs/ca-key.pem"};              ///< Agent CA 私钥 PEM，POSIX 上要求 0600 或更严
        int                      agentCaValidityDays{30};                             ///< 签发证书有效期，1 到 3650 天
        int                      agentCaOverlapHours{24};                             ///< 旧证书重叠窗口，1 到 168 小时
        int                      heartbeatIntervalSeconds{30};                        ///< Agent 期望心跳间隔，随心跳响应下发
        int                      heartbeatTimeoutSeconds{90};                         ///< 超过该秒数未心跳即判离线
        int                      offlineSweepSeconds{30};                             ///< 离线扫描周期
        bool                     udpTelemetryEnabled{false};                          ///< 是否启用 UDP 遥测监听
        std::string              udpTelemetryAddress{"0.0.0.0"};                      ///< UDP 遥测监听地址
        std::uint16_t            udpTelemetryPort{9555};                              ///< UDP 遥测端口
        std::string              udpTelemetrySecret;                                  ///< HMAC 共享密钥，只从环境变量读取
        bool                     reverseProxyEnabled{false};                          ///< 是否启用边缘反向代理
        std::string              reverseProxyPrefix{"/edge"};                         ///< 对外路径前缀
        std::string              reverseProxyUpstream;                                ///< 上游基地址
        bool                     reverseProxyStripPrefix{true};                       ///< 转发时去掉前缀
        bool                     reverseProxyRequireSession{true};                    ///< 是否要求登录
        std::string              reverseProxyPermission;                              ///< 需要的权限码
        std::int64_t             reverseProxyTimeoutMs{15000};                        ///< 单次转发时限
        std::int64_t             reverseProxyMaximumRequestBytes{8LL * 1024 * 1024};  ///< 请求正文上限
        std::int64_t             reverseProxyMaximumResponseBytes{8LL * 1024 * 1024}; ///< 响应正文上限
        bool                     oidcEnabled{false};                                  ///< 是否启用 OIDC 登录
        std::string              oidcIssuer;                                          ///< 签发者基地址
        std::string              oidcClientId;                                        ///< 客户端标识
        std::string              oidcClientSecret;                                    ///< 客户端密钥，只从环境变量读取
        std::string              oidcRedirectUri;                                     ///< 回调地址
        std::string              oidcFrontendRedirect{"/login"};                      ///< 回调后回到的前端路径
        std::string              oidcScopes{"openid profile email"};                  ///< 申请的 scope
        std::string              oidcAllowedEmailDomains;                             ///< 允许的邮箱域，逗号分隔
        bool                     oidcAutoProvision{false};                            ///< 首次登录自动建号
        std::int64_t             oidcDefaultRoleId{0};                                ///< 自动建号角色
        std::int64_t             oidcDefaultDepartmentId{101};                        ///< 自动建号部门
        std::string              oidcTenantCode{"default"};                           ///< 外部身份绑定租户
        std::string              mfaEncryptionKey;                                    ///< 两步验证主密钥的 base64 文本，只从环境变量读取

        double                   requestsPerSecond{0.0};                              ///< 每个实例的全局请求速率上限，可为小数，0 表示关闭
        double                   rateLimitBurstCapacity{1.0};                         ///< 令牌桶容量，即允许的瞬时突发量
        std::int64_t             idleTimeoutMs{75000};                                ///< HTTP 连接空闲超时，0 表示关闭
        std::int64_t             h3IdleTimeoutSeconds{30};                            ///< h3 传输层空闲超时，单位秒，必须为 1 到 3600；0 在 QUIC 里表示不设超时，故拒绝
        std::int64_t             readTimeoutMs{60000};                                ///< HTTP 连接相邻两次读取的最长间隔，0 表示关闭
        std::int64_t             writeTimeoutMs{60000};                               ///< HTTP 响应产出时限，0 表示关闭
        std::int64_t             maximumRequestBodyBytes{16LL * 1024 * 1024};         ///< HTTP 解析器正文上限
        int                      passwordMinimumLength{10};                           ///< 新建、重置与修改口令的最短长度
        bool                     passwordRequireComplexity{true};                     ///< 是否要求口令同时包含四类字符
        int                      loginFailureWindowSeconds{900};                      ///< 账号失败计数的滑动窗口，单位秒
        int                      loginMaximumFailures{5};                             ///< 触发账号锁定的连续失败次数
        int                      loginLockSeconds{900};                               ///< 账号锁定时长，单位秒

        /**
         * @brief 读取 YAML 并组装配置；同目录存在 logging.yaml 时一并载入
         * @param configurationPath 主配置路径
         * @return 成功返回完整配置；失败返回中文原因，调用方可直接打印
         * @note 只读配置键与做范围校验，不在这里启动任何资源
         */
        [[nodiscard]] static std::expected<ApplicationConfiguration, ConfigurationError> load(const std::filesystem::path &configurationPath);
    };
} // namespace EdgePulse::App
