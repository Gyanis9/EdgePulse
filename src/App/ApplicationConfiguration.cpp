#include "App/ApplicationConfiguration.h"

#include "Base/Config/ConfigManager.h"
#include "Base/Runtime/Environment.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::App
{
    namespace
    {
        constexpr std::int64_t kMinimumPort = 0;     ///< 0 表示由内核分配端口
        constexpr std::int64_t kMaximumPort = 65535; ///< TCP/UDP 端口上限
        constexpr int          kMinimumTtl  = 1;     ///< TTL 必须为正

        /**
         * @brief 去掉首尾空白
         * @param text 待裁剪文本
         * @return std::string 裁剪结果；全空白时为空串
         */
        [[nodiscard]] std::string trimCopy(const std::string_view text)
        {
            const auto begin = std::find_if_not(text.begin(), text.end(), [](const unsigned char character) { return std::isspace(character) != 0; });
            const auto end   = std::find_if_not(text.rbegin(), text.rend(), [](const unsigned char character) { return std::isspace(character) != 0; }).base();
            // begin 越过 end 说明整段都是空白，直接交回空串而不是构造非法区间
            if (begin >= end)
            {
                return {};
            }
            return std::string(begin, end);
        }

        /**
         * @brief 按逗号拆分 Origin 白名单并丢掉空项
         * @param text YAML 里的逗号分隔文本
         * @return std::vector<std::string> 去空白后的非空项
         */
        [[nodiscard]] std::vector<std::string> splitByComma(const std::string &text)
        {
            std::vector<std::string> values;
            std::string              current;
            for (const char character: text)
            {
                if (character != ',')
                {
                    current.push_back(character);
                    continue;
                }
                // 每遇到逗号就落一个非空项，空项（连续逗号、尾逗号）不进入白名单
                if (std::string trimmed = trimCopy(current); !trimmed.empty())
                {
                    values.push_back(std::move(trimmed));
                }
                current.clear();
            }
            if (std::string trimmed = trimCopy(current); !trimmed.empty())
            {
                values.push_back(std::move(trimmed));
            }
            return values;
        }

        /**
         * @brief 读一个端口并校验范围
         * @param configurationManager 已装载配置的读取入口
         * @param key 配置键
         * @param fallback 键缺失时的默认值
         * @return 成功返回 0..65535 的端口；失败返回中文越界说明
         */
        [[nodiscard]] std::expected<std::uint16_t, ConfigurationError> readPort(AsynGyanis::Base::ConfigManager &configurationManager, const std::string_view key,
                                                                                const std::uint16_t fallback)
        {
            const std::int64_t value = configurationManager.getInt(key, fallback);
            // 端口越界属于配置错误，必须在绑定套接字之前拒绝，而不是截断成别的端口
            if (value < kMinimumPort || value > kMaximumPort)
            {
                return std::unexpected(ConfigurationError{"端口配置越界（要求 0..65535）：" + std::string{key}});
            }
            return static_cast<std::uint16_t>(value);
        }
    } // namespace

    std::expected<ApplicationConfiguration, ConfigurationError> ApplicationConfiguration::load(const std::filesystem::path &configurationPath)
    {
        std::error_code             filesystemError;
        const std::filesystem::path absoluteConfigurationPath = std::filesystem::absolute(configurationPath, filesystemError);
        if (filesystemError || !std::filesystem::exists(absoluteConfigurationPath, filesystemError))
        {
            return std::unexpected(ConfigurationError{"配置文件不存在：" + configurationPath.string()});
        }

        std::vector<std::filesystem::path> configurationFiles{absoluteConfigurationPath};
        const std::filesystem::path        loggingPath = absoluteConfigurationPath.parent_path() / "logging.yaml";
        if (std::filesystem::exists(loggingPath, filesystemError))
        {
            configurationFiles.push_back(loggingPath);
        }

        AsynGyanis::Base::ConfigManager         &configurationManager = AsynGyanis::Base::ConfigManager::instance();
        const AsynGyanis::Base::ConfigLoadResult loadResult           = configurationManager.loadFiles(configurationFiles);
        if (!loadResult)
        {
            // 把每个失败文件的诊断原样拼起来，调用方据此定位是哪一个键/文件
            std::string message = "配置加载失败：";
            for (const std::string &detail: loadResult.errors)
            {
                message += "\n  " + detail;
            }
            return std::unexpected(ConfigurationError{std::move(message)});
        }

        ApplicationConfiguration configuration;
        configuration.configurationFile        = absoluteConfigurationPath;
        configuration.listenAddress            = configurationManager.getString("edgepulse.listen_address", configuration.listenAddress);
        configuration.opsAddress               = configurationManager.getString("edgepulse.ops_address", configuration.opsAddress);
        configuration.databaseDriver           = configurationManager.getString("edgepulse.database.driver", configuration.databaseDriver);
        configuration.databasePath             = configurationManager.getString("edgepulse.database.path", configuration.databasePath);
        configuration.databaseHost             = configurationManager.getString("edgepulse.database.host", configuration.databaseHost);
        configuration.databaseName             = configurationManager.getString("edgepulse.database.name", configuration.databaseName);
        configuration.databaseUser             = configurationManager.getString("edgepulse.database.user", configuration.databaseUser);
        configuration.redisEnabled             = configurationManager.getBool("edgepulse.redis.enabled", configuration.redisEnabled);
        configuration.redisHost                = configurationManager.getString("edgepulse.redis.host", configuration.redisHost);
        configuration.redisDatabase            = static_cast<int>(configurationManager.getInt("edgepulse.redis.database", configuration.redisDatabase));
        configuration.threads                  = static_cast<int>(configurationManager.getInt("edgepulse.threads", configuration.threads));
        configuration.migrationsDirectory      = configurationManager.getString("edgepulse.migrations_dir", configuration.migrationsDirectory);
        configuration.webRoot                  = configurationManager.getString("edgepulse.web_root", configuration.webRoot);
        configuration.firmwareDirectory        = configurationManager.getString("edgepulse.firmware_dir", configuration.firmwareDirectory);
        configuration.maximumFirmwareBytes     = configurationManager.getInt("edgepulse.maximum_firmware_bytes", configuration.maximumFirmwareBytes);
        configuration.telemetryRetentionDays   = static_cast<int>(configurationManager.getInt("edgepulse.telemetry_retention_days", configuration.telemetryRetentionDays));
        configuration.certificateDirectory     = configurationManager.getString("edgepulse.cert_dir", configuration.certificateDirectory);
        configuration.agentFingerprint         = configurationManager.getString("edgepulse.agent.fingerprint", configuration.agentFingerprint);
        configuration.captchaTtlSeconds        = static_cast<int>(configurationManager.getInt("edgepulse.captcha.ttl_seconds", configuration.captchaTtlSeconds));
        configuration.captchaRequestsPerMinute = static_cast<int>(configurationManager.getInt("edgepulse.captcha.requests_per_minute", configuration.captchaRequestsPerMinute));
        configuration.sessionTtlSeconds        = static_cast<int>(configurationManager.getInt("edgepulse.session.ttl_seconds", configuration.sessionTtlSeconds));
        configuration.exposeCaptchaCode        = configurationManager.getBool("edgepulse.captcha.expose_code", configuration.exposeCaptchaCode);
        configuration.allowedOrigins           = splitByComma(configurationManager.getString("edgepulse.security.allowed_origins", ""));

        if (const auto password = Common::environmentValue("EDGEPULSE_DB_PASSWORD"); password.has_value())
        {
            configuration.databasePassword = *password;
        }
        if (const auto password = Common::environmentValue("EDGEPULSE_REDIS_PASSWORD"); password.has_value())
        {
            configuration.redisPassword = *password;
        }
        // 指纹允许通过环境变量覆盖，run-dev.sh 会在生成证书后导出
        if (configuration.agentFingerprint.empty())
        {
            if (const std::optional<std::string> fingerprint = Common::environmentValue("EDGEPULSE_AGENT_FINGERPRINT"); fingerprint.has_value())
            {
                configuration.agentFingerprint = *fingerprint;
            }
        }

        if (configuration.databaseDriver != "sqlite" && configuration.databaseDriver != "mysql")
        {
            return std::unexpected(ConfigurationError{"database.driver 只能是 sqlite 或 mysql，当前值：" + configuration.databaseDriver});
        }
        if (configuration.threads < 1 || configuration.threads > 64)
        {
            return std::unexpected(ConfigurationError{"edgepulse.threads 必须在 1 到 64 之间"});
        }
        // TTL 参与定时器与过期判定，非正数会让条目永不过期或立即过期
        if (configuration.captchaTtlSeconds < kMinimumTtl || configuration.sessionTtlSeconds < kMinimumTtl)
        {
            return std::unexpected(ConfigurationError{"captcha/session 的 TTL 必须是正数"});
        }

        const auto httpPort = readPort(configurationManager, "edgepulse.http_port", configuration.httpPort);
        if (!httpPort)
        {
            return std::unexpected(httpPort.error());
        }
        configuration.httpPort = *httpPort;

        const auto httpsPort = readPort(configurationManager, "edgepulse.https_port", configuration.httpsPort);
        if (!httpsPort)
        {
            return std::unexpected(httpsPort.error());
        }
        configuration.httpsPort = *httpsPort;

        const auto agentPort = readPort(configurationManager, "edgepulse.agent_port", configuration.agentPort);
        if (!agentPort)
        {
            return std::unexpected(agentPort.error());
        }
        configuration.agentPort = *agentPort;

        const auto opsPort = readPort(configurationManager, "edgepulse.ops_port", configuration.opsPort);
        if (!opsPort)
        {
            return std::unexpected(opsPort.error());
        }
        configuration.opsPort = *opsPort;

        const auto databasePort = readPort(configurationManager, "edgepulse.database.port", configuration.databasePort);
        if (!databasePort)
        {
            return std::unexpected(databasePort.error());
        }
        configuration.databasePort = *databasePort;
        const auto redisPort       = readPort(configurationManager, "edgepulse.redis.port", configuration.redisPort);
        if (!redisPort)
        {
            return std::unexpected(redisPort.error());
        }
        configuration.redisPort = *redisPort;

        return configuration;
    }
} // namespace EdgePulse::App
