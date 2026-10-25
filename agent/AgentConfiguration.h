/**
 * @file AgentConfiguration.h
 * @brief Agent 配置的加载、路径归一与形态校验入口
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace EdgePulse::Agent
{
    /**
     * @brief Agent 运行配置
     * @details 非敏感项来自 YAML；注册与换绑令牌只按环境变量名读取，不落在本结构里。
     *          相对路径在加载时已按配置文件所在目录解析为绝对路径。
     */
    struct AgentConfiguration
    {
        std::filesystem::path configurationFile;                                      ///< 本次加载的配置文件绝对路径
        std::string           baseUrl;                                                ///< 服务端基地址，必须为 https://
        std::filesystem::path caFile;                                                 ///< 校验服务端证书的 CA 文件
        std::filesystem::path certificateFile;                                        ///< 客户端证书
        std::filesystem::path privateKeyFile;                                         ///< 客户端私钥
        std::string           tenantCode{"default"};                                  ///< 租户编码
        std::string           name;                                                   ///< 节点名，1 到 128 个字符
        std::string           enrollmentTokenEnv{"EDGEPULSE_AGENT_ENROLLMENT_TOKEN"}; ///< 注册令牌所在环境变量名
        std::string           rebindTokenEnv{"EDGEPULSE_AGENT_REBIND_TOKEN"};         ///< 换绑令牌所在环境变量名
        int                   heartbeatIntervalSeconds{30};                           ///< 心跳间隔，5 到 600 秒
        int                   commandPollIntervalSeconds{15};                         ///< 命令轮询间隔，5 到 600 秒
        int                   telemetryIntervalSeconds{30};                           ///< 遥测采集间隔，5 到 3600 秒
        bool                  websocketEnabled{true};                                 ///< 是否建立命令长连接
        int                   requestTimeoutSeconds{30};                              ///< 单次请求超时，单位秒
        bool                  certificateRenewalEnabled{true};                        ///< 是否启动证书自动续期循环；使用外部 CA 时可关闭
        int                   certificateRenewBeforeDays{0};                          ///< 续期提前量覆盖值，0 表示按规范 max(25% 生命周期, 24h)，允许 0 到 90 天
        int                   certificateRenewalCheckSeconds{3600};                   ///< 续期检查间隔，60 到 86400 秒
        std::filesystem::path stateDirectory{"var/agent"};                            ///< 状态库与运行期文件的目录
        std::filesystem::path logFile{"var/agent/agent.log"};                         ///< 日志文件路径
        int                   logMaxSizeMb{32};                                       ///< 单个日志文件的上限，单位 MB
        int                   logMaxBackup{5};                                        ///< 日志备份保留份数
        std::string           logLevel{"info"};                                       ///< 日志等级
        std::filesystem::path telemetryDiskPath{"/"};                                 ///< 磁盘遥测的挂载点或卷路径
        std::size_t           diagnosticsLogTailBytes{65536};                         ///< 诊断上报附带的日志尾长度，单位字节
        std::filesystem::path configPath;                                             ///< 由服务端下发的配置文件路径
        std::filesystem::path configApplyHook;                                        ///< 配置应用钩子，空表示不做额外动作
        std::int64_t          maximumConfigBytes{65536};                              ///< 可接受的配置正文上限，单位字节
        std::filesystem::path firmwareInstallHook;                                    ///< 固件安装钩子
        std::filesystem::path firmwareTrustedKeysDirectory;                           ///< 固件签名公钥目录
        std::filesystem::path firmwareStagingDirectory;                               ///< 固件下载暂存目录
        std::filesystem::path packageTrustedKeysDirectory;                            ///< 升级包签名公钥目录
        std::filesystem::path updateInstallerScript;                                  ///< 升级安装脚本
        int                   updateHealthTimeoutSeconds{60};                         ///< 升级后健康检查时限，单位秒
        std::filesystem::path updateStagingDirectory;                                 ///< 升级包暂存目录
        bool                  allowPlaintextForTests{false};                          ///< 是否允许 http://，仅测试环境使用

        /**
         * @brief 读取 YAML 并校验配置形态
         * @param path 配置文件路径，相对路径按当前工作目录解析
         * @return 成功返回解析后的配置；失败返回中文原因，可直接打印给运维
         * @note 只校验形态与取值范围，不检查证书、私钥与 CA 是否存在；身份加载阶段负责这些检查
         */
        [[nodiscard]] static std::expected<AgentConfiguration, std::string> load(const std::filesystem::path &path);
    };
} // namespace EdgePulse::Agent
