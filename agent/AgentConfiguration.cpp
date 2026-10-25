#include "AgentConfiguration.h"

#include "Platform/Platform.h"

#include "Base/Config/ConfigLoadResult.h"
#include "Base/Config/ConfigManager.h"
#include "Base/Config/ConfigValue.h"
#include "Platform/FileSystem/FileSystem.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace EdgePulse::Agent
{
    namespace
    {
        constexpr int kMinimumHeartbeatSeconds    = 5;     ///< 心跳间隔下限
        constexpr int kMaximumHeartbeatSeconds    = 600;   ///< 心跳间隔上限
        constexpr int kMinimumPollSeconds         = 5;     ///< 命令轮询间隔下限
        constexpr int kMaximumPollSeconds         = 600;   ///< 命令轮询间隔上限
        constexpr int kMinimumTelemetrySeconds    = 5;     ///< 遥测间隔下限
        constexpr int kMaximumTelemetrySeconds    = 3600;  ///< 遥测间隔上限
        constexpr int kMinimumRenewBeforeDays     = 0;     ///< 续期提前天数下限，0 表示按规范窗口自动计算
        constexpr int kMaximumRenewBeforeDays     = 90;    ///< 续期提前天数上限
        constexpr int kMinimumRenewalCheckSeconds = 60;    ///< 续期检查间隔下限
        constexpr int kMaximumRenewalCheckSeconds = 86400; ///< 续期检查间隔上限
        constexpr int kMaximumBackupCount         = 4096;  ///< 日志备份份数上限，与滚动 Sink 的钳制口径一致

        /**
         * @brief 去掉首尾空白
         * @param text 待裁剪文本
         * @return std::string 裁剪结果；全空白时为空串
         */
        [[nodiscard]] std::string trimCopy(const std::string_view text)
        {
            const auto begin = std::find_if_not(text.begin(), text.end(), [](const unsigned char character) { return std::isspace(character) != 0; });
            const auto end   = std::find_if_not(text.rbegin(), text.rend(), [](const unsigned char character) { return std::isspace(character) != 0; }).base();
            if (begin >= end)
            {
                return {};
            }
            return std::string(begin, end);
        }

        /**
         * @brief 转成小写，供日志等级等词表字段做大小写不敏感比较
         * @param text 原文本
         * @return std::string 全小写副本
         */
        [[nodiscard]] std::string lowercaseCopy(std::string text)
        {
            std::ranges::transform(text, text.begin(), [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
            return text;
        }

        /**
         * @brief 读取只接受十进制整数的配置
         * @param manager 已装载配置的读取入口
         * @param key 配置键
         * @param fallback 键缺失时的默认值
         * @return 成功返回解析后的整数；失败返回中文原因，含键名与原始文本
         * @note 引号标量在引擎里是字符串，单靠文本解析会把 "30" 当成 30；这里先要求底层值本身是整数类型
         */
        [[nodiscard]] std::expected<std::int64_t, std::string> readInteger(AsynGyanis::Base::ConfigManager &manager, const std::string_view key, const std::int64_t fallback)
        {
            if (!manager.has(key))
            {
                return fallback;
            }
            const std::optional<AsynGyanis::Base::ConfigValue> stored = manager.getOptional(key);
            const std::string                                  text   = trimCopy(manager.getText(key));
            const auto                                         reject = [&]()
            {
                const std::string display = text.empty() ? "空值" : "“" + text + "”";
                return std::unexpected("配置项 " + std::string{key} + " 必须是十进制整数，不能加引号、写成小数或带单位，当前值不是整数：" + display);
            };
            if (!stored.has_value() || !stored->is_number_integer() || text.empty())
            {
                return reject();
            }
            std::int64_t value                 = 0;
            const auto [parsedEnd, parseError] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parseError != std::errc{} || parsedEnd != text.data() + text.size())
            {
                return reject();
            }
            return value;
        }

        /**
         * @brief 把配置里的路径文本解成路径对象，相对路径落在基准目录下
         * @param baseDirectory 配置文件所在目录
         * @param text 配置里的路径文本，可带首尾空白
         * @return std::filesystem::path 归一后的绝对路径；空文本返回空路径
         * @note 文本按 UTF-8 解释，避免 Windows 本地代码页把非 ASCII 路径写成问号
         */
        [[nodiscard]] std::filesystem::path resolvePath(const std::filesystem::path &baseDirectory, const std::string &text)
        {
            const std::string trimmed = trimCopy(text);
            if (trimmed.empty())
            {
                return {};
            }
            std::filesystem::path resolved = AsynGyanis::Platform::FileSystem::pathFromUtf8(trimmed);
            if (resolved.is_relative())
            {
                resolved = baseDirectory / resolved;
            }
            return resolved.lexically_normal();
        }

        /**
         * @brief 判断文本是否落在闭区间内，并生成统一的中文越界说明
         * @param value 当前取值
         * @param minimum 下限
         * @param maximum 上限
         * @param key 配置键，写进错误文本
         * @return std::optional<std::string> 越界时返回错误文本，合法时为空
         */
        [[nodiscard]] std::optional<std::string> checkRange(const std::int64_t value, const std::int64_t minimum, const std::int64_t maximum, const std::string_view key)
        {
            if (value < minimum || value > maximum)
            {
                return "配置项 " + std::string{key} + " 必须在 " + std::to_string(minimum) + " 到 " + std::to_string(maximum) + " 之间，当前值：" + std::to_string(value);
            }
            return std::nullopt;
        }
    } // namespace

    std::expected<AgentConfiguration, std::string> AgentConfiguration::load(const std::filesystem::path &path)
    {
        std::error_code             filesystemError;
        const std::filesystem::path absoluteConfigurationPath = std::filesystem::absolute(path, filesystemError);
        if (filesystemError || !std::filesystem::exists(absoluteConfigurationPath, filesystemError))
        {
            return std::unexpected("配置文件不存在：" + path.string());
        }

        AsynGyanis::Base::ConfigManager         &manager    = AsynGyanis::Base::ConfigManager::instance();
        const AsynGyanis::Base::ConfigLoadResult loadResult = manager.loadFiles({absoluteConfigurationPath});
        if (!loadResult)
        {
            std::string message = "配置加载失败：";
            for (const std::string &detail: loadResult.errors)
            {
                message += "\n  " + detail;
            }
            return std::unexpected(std::move(message));
        }

        const std::filesystem::path baseDirectory = absoluteConfigurationPath.parent_path();
        AgentConfiguration          configuration;
        configuration.configurationFile      = absoluteConfigurationPath;
        configuration.baseUrl                = trimCopy(manager.getString("agent.server.base_url", std::string{}));
        configuration.allowPlaintextForTests = manager.getBool("agent.server.allow_plaintext_for_tests", false);

        // 明文地址必须先于其余校验被拦下：后面缺证书、缺名字的报错会把真正的原因盖掉
        const bool schemeAllowed = configuration.baseUrl.starts_with("https://") || (configuration.allowPlaintextForTests && configuration.baseUrl.starts_with("http://"));
        if (!schemeAllowed)
        {
            return std::unexpected("agent.server.base_url 必须以 https:// 开头；测试环境确需明文时请显式设置 agent.server.allow_plaintext_for_tests: true，当前值：“" +
                                   configuration.baseUrl + "”");
        }

        configuration.caFile             = resolvePath(baseDirectory, manager.getString("agent.server.ca_file", std::string{}));
        configuration.certificateFile    = resolvePath(baseDirectory, manager.getString("agent.server.certificate_file", std::string{}));
        configuration.privateKeyFile     = resolvePath(baseDirectory, manager.getString("agent.server.private_key_file", std::string{}));
        configuration.tenantCode         = trimCopy(manager.getString("agent.server.tenant_code", configuration.tenantCode));
        configuration.name               = trimCopy(manager.getString("agent.server.name", std::string{}));
        configuration.enrollmentTokenEnv = trimCopy(manager.getString("agent.server.enrollment_token_env", configuration.enrollmentTokenEnv));
        configuration.rebindTokenEnv     = trimCopy(manager.getString("agent.server.rebind_token_env", configuration.rebindTokenEnv));

        const auto heartbeatInterval = readInteger(manager, "agent.server.heartbeat_interval_seconds", configuration.heartbeatIntervalSeconds);
        if (!heartbeatInterval)
        {
            return std::unexpected(heartbeatInterval.error());
        }
        if (auto rangeError = checkRange(*heartbeatInterval, kMinimumHeartbeatSeconds, kMaximumHeartbeatSeconds, "agent.server.heartbeat_interval_seconds"))
        {
            return std::unexpected(std::move(*rangeError));
        }
        configuration.heartbeatIntervalSeconds = static_cast<int>(*heartbeatInterval);

        const auto commandPollInterval = readInteger(manager, "agent.server.command_poll_interval_seconds", configuration.commandPollIntervalSeconds);
        if (!commandPollInterval)
        {
            return std::unexpected(commandPollInterval.error());
        }
        if (auto rangeError = checkRange(*commandPollInterval, kMinimumPollSeconds, kMaximumPollSeconds, "agent.server.command_poll_interval_seconds"))
        {
            return std::unexpected(std::move(*rangeError));
        }
        configuration.commandPollIntervalSeconds = static_cast<int>(*commandPollInterval);

        const auto requestTimeout = readInteger(manager, "agent.server.request_timeout_seconds", configuration.requestTimeoutSeconds);
        if (!requestTimeout)
        {
            return std::unexpected(requestTimeout.error());
        }
        if (auto rangeError = checkRange(*requestTimeout, 1, std::numeric_limits<int>::max(), "agent.server.request_timeout_seconds"))
        {
            return std::unexpected(std::move(*rangeError));
        }
        configuration.requestTimeoutSeconds = static_cast<int>(*requestTimeout);

        configuration.websocketEnabled = manager.getBool("agent.server.websocket_enabled", configuration.websocketEnabled);

        // 外部 CA 部署关掉续期循环，避免每轮都向未启用内部 CA 的控制面换取 503
        configuration.certificateRenewalEnabled = manager.getBool("agent.certificate.renewal_enabled", configuration.certificateRenewalEnabled);

        const auto renewBeforeDays = readInteger(manager, "agent.certificate.renew_before_days", configuration.certificateRenewBeforeDays);
        if (!renewBeforeDays)
        {
            return std::unexpected(renewBeforeDays.error());
        }
        if (auto rangeError = checkRange(*renewBeforeDays, kMinimumRenewBeforeDays, kMaximumRenewBeforeDays, "agent.certificate.renew_before_days"))
        {
            return std::unexpected(std::move(*rangeError));
        }
        configuration.certificateRenewBeforeDays = static_cast<int>(*renewBeforeDays);

        const auto renewalCheckSeconds = readInteger(manager, "agent.certificate.renewal_check_seconds", configuration.certificateRenewalCheckSeconds);
        if (!renewalCheckSeconds)
        {
            return std::unexpected(renewalCheckSeconds.error());
        }
        if (auto rangeError = checkRange(*renewalCheckSeconds, kMinimumRenewalCheckSeconds, kMaximumRenewalCheckSeconds, "agent.certificate.renewal_check_seconds"))
        {
            return std::unexpected(std::move(*rangeError));
        }
        configuration.certificateRenewalCheckSeconds = static_cast<int>(*renewalCheckSeconds);

        const auto telemetryInterval = readInteger(manager, "agent.telemetry.interval_seconds", configuration.telemetryIntervalSeconds);
        if (!telemetryInterval)
        {
            return std::unexpected(telemetryInterval.error());
        }
        if (auto rangeError = checkRange(*telemetryInterval, kMinimumTelemetrySeconds, kMaximumTelemetrySeconds, "agent.telemetry.interval_seconds"))
        {
            return std::unexpected(std::move(*rangeError));
        }
        configuration.telemetryIntervalSeconds = static_cast<int>(*telemetryInterval);
        configuration.telemetryDiskPath        = resolvePath(baseDirectory, manager.getString("agent.telemetry.disk_path", "/"));

        configuration.stateDirectory = resolvePath(baseDirectory, manager.getString("agent.state.directory", "var/agent"));
        configuration.logFile        = resolvePath(baseDirectory, manager.getString("agent.logging.file", "var/agent/agent.log"));
        configuration.logLevel       = lowercaseCopy(trimCopy(manager.getString("agent.logging.level", configuration.logLevel)));

        const auto logMaxSizeMb = readInteger(manager, "agent.logging.max_size_mb", configuration.logMaxSizeMb);
        if (!logMaxSizeMb)
        {
            return std::unexpected(logMaxSizeMb.error());
        }
        if (auto rangeError = checkRange(*logMaxSizeMb, 1, 1024 * 1024, "agent.logging.max_size_mb"))
        {
            return std::unexpected(std::move(*rangeError));
        }
        configuration.logMaxSizeMb = static_cast<int>(*logMaxSizeMb);

        const auto logMaxBackup = readInteger(manager, "agent.logging.max_backup", configuration.logMaxBackup);
        if (!logMaxBackup)
        {
            return std::unexpected(logMaxBackup.error());
        }
        if (auto rangeError = checkRange(*logMaxBackup, 0, kMaximumBackupCount, "agent.logging.max_backup"))
        {
            return std::unexpected(std::move(*rangeError));
        }
        configuration.logMaxBackup = static_cast<int>(*logMaxBackup);

        configuration.configPath                   = resolvePath(baseDirectory, manager.getString("agent.executors.config.path", std::string{}));
        configuration.configApplyHook              = resolvePath(baseDirectory, manager.getString("agent.executors.config.apply_hook", std::string{}));
        configuration.firmwareInstallHook          = resolvePath(baseDirectory, manager.getString("agent.executors.firmware.install_hook", std::string{}));
        configuration.firmwareTrustedKeysDirectory = resolvePath(baseDirectory, manager.getString("agent.executors.firmware.trusted_keys_dir", std::string{}));
        configuration.firmwareStagingDirectory     = resolvePath(baseDirectory, manager.getString("agent.executors.firmware.staging_directory", std::string{}));

        const auto maximumConfigBytes = readInteger(manager, "agent.executors.config.maximum_bytes", configuration.maximumConfigBytes);
        if (!maximumConfigBytes)
        {
            return std::unexpected(maximumConfigBytes.error());
        }
        if (*maximumConfigBytes < 1)
        {
            return std::unexpected("agent.executors.config.maximum_bytes 必须是正数");
        }
        configuration.maximumConfigBytes = *maximumConfigBytes;

        const auto diagnosticsLogTailBytes = readInteger(manager, "agent.executors.diagnostics.log_tail_bytes", static_cast<std::int64_t>(configuration.diagnosticsLogTailBytes));
        if (!diagnosticsLogTailBytes)
        {
            return std::unexpected(diagnosticsLogTailBytes.error());
        }
        if (*diagnosticsLogTailBytes < 1)
        {
            return std::unexpected("agent.executors.diagnostics.log_tail_bytes 必须是正数");
        }
        configuration.diagnosticsLogTailBytes = static_cast<std::size_t>(*diagnosticsLogTailBytes);

        configuration.packageTrustedKeysDirectory = resolvePath(baseDirectory, manager.getString("agent.update.package_trusted_keys_dir", std::string{}));
        configuration.updateInstallerScript       = resolvePath(baseDirectory, manager.getString("agent.update.installer_script", std::string{}));
        configuration.updateStagingDirectory      = resolvePath(baseDirectory, manager.getString("agent.update.staging_directory", std::string{}));

        const auto updateHealthTimeout = readInteger(manager, "agent.update.health_timeout_seconds", configuration.updateHealthTimeoutSeconds);
        if (!updateHealthTimeout)
        {
            return std::unexpected(updateHealthTimeout.error());
        }
        if (auto rangeError = checkRange(*updateHealthTimeout, 1, std::numeric_limits<int>::max(), "agent.update.health_timeout_seconds"))
        {
            return std::unexpected(std::move(*rangeError));
        }
        configuration.updateHealthTimeoutSeconds = static_cast<int>(*updateHealthTimeout);

        if (configuration.caFile.empty() || configuration.certificateFile.empty() || configuration.privateKeyFile.empty())
        {
            return std::unexpected("agent.server.ca_file、certificate_file 与 private_key_file 必须配置为非空路径");
        }
        if (configuration.name.empty() || configuration.name.size() > 128)
        {
            return std::unexpected("agent.server.name 长度必须在 1 到 128 个字符之间");
        }
        if (configuration.tenantCode.empty())
        {
            return std::unexpected("agent.server.tenant_code 不能为空");
        }
        if (configuration.stateDirectory.empty() || configuration.logFile.empty())
        {
            return std::unexpected("agent.state.directory 与 agent.logging.file 必须配置为非空路径");
        }
        if (configuration.logLevel != "trace" && configuration.logLevel != "debug" && configuration.logLevel != "info" && configuration.logLevel != "warn" &&
            configuration.logLevel != "error" && configuration.logLevel != "fatal" && configuration.logLevel != "off")
        {
            return std::unexpected("agent.logging.level 只能是 trace、debug、info、warn、error、fatal 或 off，当前值：“" + configuration.logLevel + "”");
        }

        return configuration;
    }
} // namespace EdgePulse::Agent
