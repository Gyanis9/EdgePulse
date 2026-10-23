#include "App/ApplicationConfiguration.h"

#include "Base/Config/ConfigManager.h"
#include "Base/Runtime/Environment.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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
        configuration.configurationFile      = absoluteConfigurationPath;
        configuration.listenAddress          = configurationManager.getString("edgepulse.listen_address", configuration.listenAddress);
        configuration.opsAddress             = configurationManager.getString("edgepulse.ops_address", configuration.opsAddress);
        configuration.databaseDriver         = configurationManager.getString("edgepulse.database.driver", configuration.databaseDriver);
        configuration.databasePath           = configurationManager.getString("edgepulse.database.path", configuration.databasePath);
        configuration.databaseHost           = configurationManager.getString("edgepulse.database.host", configuration.databaseHost);
        configuration.databaseName           = configurationManager.getString("edgepulse.database.name", configuration.databaseName);
        configuration.databaseUser           = configurationManager.getString("edgepulse.database.user", configuration.databaseUser);
        configuration.redisEnabled           = configurationManager.getBool("edgepulse.redis.enabled", configuration.redisEnabled);
        configuration.redisHost              = configurationManager.getString("edgepulse.redis.host", configuration.redisHost);
        configuration.redisDatabase          = static_cast<int>(configurationManager.getInt("edgepulse.redis.database", configuration.redisDatabase));
        configuration.threads                = static_cast<int>(configurationManager.getInt("edgepulse.threads", configuration.threads));
        configuration.migrationsDirectory    = configurationManager.getString("edgepulse.migrations_dir", configuration.migrationsDirectory);
        configuration.webRoot                = configurationManager.getString("edgepulse.web_root", configuration.webRoot);
        configuration.firmwareDirectory            = configurationManager.getString("edgepulse.firmware_dir", configuration.firmwareDirectory);
        configuration.maximumFirmwareBytes         = configurationManager.getInt("edgepulse.maximum_firmware_bytes", configuration.maximumFirmwareBytes);
        configuration.firmwareRequireSignature     = configurationManager.getBool("edgepulse.firmware.require_signature", configuration.firmwareRequireSignature);
        configuration.firmwareTrustedKeysDirectory = configurationManager.getString("edgepulse.firmware.trusted_keys_dir", configuration.firmwareTrustedKeysDirectory);
        configuration.telemetryRetentionDays       = static_cast<int>(configurationManager.getInt("edgepulse.telemetry_retention_days", configuration.telemetryRetentionDays));
        configuration.logRetentionDays       = static_cast<int>(configurationManager.getInt("edgepulse.log_retention_days", configuration.logRetentionDays));
        configuration.deliveryRetentionDays  = static_cast<int>(configurationManager.getInt("edgepulse.delivery_retention_days", configuration.deliveryRetentionDays));
        configuration.certificateDirectory   = configurationManager.getString("edgepulse.cert_dir", configuration.certificateDirectory);
        configuration.agentFingerprint       = configurationManager.getString("edgepulse.agent.fingerprint", configuration.agentFingerprint);
        configuration.enrollmentMode         = configurationManager.getString("edgepulse.agent.enrollment_mode", configuration.enrollmentMode);
        configuration.heartbeatIntervalSeconds =
                static_cast<int>(configurationManager.getInt("edgepulse.agent.heartbeat_interval_seconds", configuration.heartbeatIntervalSeconds));
        configuration.heartbeatTimeoutSeconds = static_cast<int>(configurationManager.getInt("edgepulse.agent.heartbeat_timeout_seconds", configuration.heartbeatTimeoutSeconds));
        configuration.offlineSweepSeconds     = static_cast<int>(configurationManager.getInt("edgepulse.agent.offline_sweep_seconds", configuration.offlineSweepSeconds));
        configuration.udpTelemetryEnabled     = configurationManager.getBool("edgepulse.udp_telemetry.enabled", configuration.udpTelemetryEnabled);
        configuration.udpTelemetryAddress     = configurationManager.getString("edgepulse.udp_telemetry.address", configuration.udpTelemetryAddress);
        configuration.reverseProxyEnabled     = configurationManager.getBool("edgepulse.reverse_proxy.enabled", configuration.reverseProxyEnabled);
        configuration.reverseProxyPrefix      = configurationManager.getString("edgepulse.reverse_proxy.prefix", configuration.reverseProxyPrefix);
        configuration.reverseProxyUpstream    = configurationManager.getString("edgepulse.reverse_proxy.upstream", configuration.reverseProxyUpstream);
        configuration.reverseProxyStripPrefix = configurationManager.getBool("edgepulse.reverse_proxy.strip_prefix", configuration.reverseProxyStripPrefix);
        configuration.reverseProxyRequireSession      = configurationManager.getBool("edgepulse.reverse_proxy.require_session", configuration.reverseProxyRequireSession);
        configuration.reverseProxyPermission          = configurationManager.getString("edgepulse.reverse_proxy.permission", configuration.reverseProxyPermission);
        configuration.reverseProxyTimeoutMs           = configurationManager.getInt("edgepulse.reverse_proxy.timeout_ms", configuration.reverseProxyTimeoutMs);
        configuration.reverseProxyMaximumRequestBytes = configurationManager.getInt("edgepulse.reverse_proxy.maximum_request_bytes", configuration.reverseProxyMaximumRequestBytes);
        configuration.reverseProxyMaximumResponseBytes =
                configurationManager.getInt("edgepulse.reverse_proxy.maximum_response_bytes", configuration.reverseProxyMaximumResponseBytes);
        configuration.oidcEnabled              = configurationManager.getBool("edgepulse.oidc.enabled", configuration.oidcEnabled);
        configuration.oidcIssuer               = configurationManager.getString("edgepulse.oidc.issuer", configuration.oidcIssuer);
        configuration.oidcClientId             = configurationManager.getString("edgepulse.oidc.client_id", configuration.oidcClientId);
        configuration.oidcRedirectUri          = configurationManager.getString("edgepulse.oidc.redirect_uri", configuration.oidcRedirectUri);
        configuration.oidcFrontendRedirect     = configurationManager.getString("edgepulse.oidc.frontend_redirect", configuration.oidcFrontendRedirect);
        configuration.oidcScopes               = configurationManager.getString("edgepulse.oidc.scopes", configuration.oidcScopes);
        configuration.oidcAllowedEmailDomains  = configurationManager.getString("edgepulse.oidc.allowed_email_domains", configuration.oidcAllowedEmailDomains);
        configuration.oidcAutoProvision        = configurationManager.getBool("edgepulse.oidc.auto_provision", configuration.oidcAutoProvision);
        configuration.oidcDefaultRoleId        = configurationManager.getInt("edgepulse.oidc.default_role_id", configuration.oidcDefaultRoleId);
        configuration.oidcDefaultDepartmentId  = configurationManager.getInt("edgepulse.oidc.default_dept_id", configuration.oidcDefaultDepartmentId);
        configuration.oidcTenantCode           = configurationManager.getString("edgepulse.oidc.tenant_code", configuration.oidcTenantCode);
        configuration.captchaTtlSeconds        = static_cast<int>(configurationManager.getInt("edgepulse.captcha.ttl_seconds", configuration.captchaTtlSeconds));
        configuration.captchaRequestsPerMinute = static_cast<int>(configurationManager.getInt("edgepulse.captcha.requests_per_minute", configuration.captchaRequestsPerMinute));
        configuration.sessionTtlSeconds        = static_cast<int>(configurationManager.getInt("edgepulse.session.ttl_seconds", configuration.sessionTtlSeconds));
        configuration.exposeCaptchaCode        = configurationManager.getBool("edgepulse.captcha.expose_code", configuration.exposeCaptchaCode);
        configuration.allowedOrigins           = splitByComma(configurationManager.getString("edgepulse.security.allowed_origins", ""));
        configuration.allowPrivateWebhooks     = configurationManager.getBool("edgepulse.security.allow_private_webhooks", configuration.allowPrivateWebhooks);

        configuration.requestsPerSecond         = configurationManager.getDouble("server.rate_limit.requests_per_second", configuration.requestsPerSecond);
        configuration.rateLimitBurstCapacity    = configurationManager.getDouble("server.rate_limit.burst_capacity", configuration.rateLimitBurstCapacity);
        configuration.idleTimeoutMs             = configurationManager.getInt("server.limits.idle_timeout_ms", configuration.idleTimeoutMs);
        configuration.readTimeoutMs             = configurationManager.getInt("server.limits.read_timeout_ms", configuration.readTimeoutMs);
        configuration.writeTimeoutMs            = configurationManager.getInt("server.limits.write_timeout_ms", configuration.writeTimeoutMs);
        configuration.maximumRequestBodyBytes   = configurationManager.getInt("server.parser_limits.maximum_body_size", configuration.maximumRequestBodyBytes);
        configuration.passwordMinimumLength     = static_cast<int>(configurationManager.getInt("edgepulse.security.password_min_length", configuration.passwordMinimumLength));
        configuration.passwordRequireComplexity = configurationManager.getBool("edgepulse.security.password_require_complexity", configuration.passwordRequireComplexity);
        configuration.loginFailureWindowSeconds =
                static_cast<int>(configurationManager.getInt("edgepulse.security.login_failure_window_seconds", configuration.loginFailureWindowSeconds));
        configuration.loginMaximumFailures = static_cast<int>(configurationManager.getInt("edgepulse.security.login_max_failures", configuration.loginMaximumFailures));
        configuration.loginLockSeconds     = static_cast<int>(configurationManager.getInt("edgepulse.security.login_lock_seconds", configuration.loginLockSeconds));

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
        if (const auto secret = Common::environmentValue("EDGEPULSE_UDP_TELEMETRY_SECRET"); secret.has_value())
        {
            configuration.udpTelemetrySecret = *secret;
        }

        if (configuration.databaseDriver != "sqlite" && configuration.databaseDriver != "mysql")
        {
            return std::unexpected(ConfigurationError{"database.driver 只能是 sqlite 或 mysql，当前值：" + configuration.databaseDriver});
        }
        if (configuration.threads < 1 || configuration.threads > 64)
        {
            return std::unexpected(ConfigurationError{"edgepulse.threads 必须在 1 到 64 之间"});
        }
        if (configuration.enrollmentMode != "off" && configuration.enrollmentMode != "required")
        {
            return std::unexpected(ConfigurationError{"edgepulse.agent.enrollment_mode 只能是 off 或 required，当前值：" + configuration.enrollmentMode});
        }
        if (configuration.firmwareRequireSignature && configuration.firmwareTrustedKeysDirectory.empty())
        {
            return std::unexpected(ConfigurationError{"启用固件签名校验时 edgepulse.firmware.trusted_keys_dir 不能为空"});
        }
        if (configuration.heartbeatIntervalSeconds < 1 || configuration.heartbeatIntervalSeconds > 600)
        {
            return std::unexpected(ConfigurationError{"edgepulse.agent.heartbeat_interval_seconds 必须在 1 到 600 之间"});
        }
        if (configuration.heartbeatTimeoutSeconds < 10 || configuration.heartbeatTimeoutSeconds > 3600)
        {
            return std::unexpected(ConfigurationError{"edgepulse.agent.heartbeat_timeout_seconds 必须在 10 到 3600 之间"});
        }
        if (configuration.offlineSweepSeconds < 1 || configuration.offlineSweepSeconds > 300)
        {
            return std::unexpected(ConfigurationError{"edgepulse.agent.offline_sweep_seconds 必须在 1 到 300 之间"});
        }
        // 间隔或扫描周期不小于超时时间时，节点会在下一次心跳或扫描前就被判离线
        if (configuration.heartbeatIntervalSeconds >= configuration.heartbeatTimeoutSeconds)
        {
            return std::unexpected(ConfigurationError{"edgepulse.agent.heartbeat_interval_seconds 必须小于 heartbeat_timeout_seconds"});
        }
        if (configuration.offlineSweepSeconds >= configuration.heartbeatTimeoutSeconds)
        {
            return std::unexpected(ConfigurationError{"edgepulse.agent.offline_sweep_seconds 必须小于 heartbeat_timeout_seconds"});
        }
        if (configuration.udpTelemetryEnabled && configuration.udpTelemetrySecret.size() < 16)
        {
            return std::unexpected(ConfigurationError{"启用 UDP 遥测时必须在 EDGEPULSE_UDP_TELEMETRY_SECRET 中配置至少 16 字节的共享密钥"});
        }
        if (const auto secret = Common::environmentValue("EDGEPULSE_OIDC_CLIENT_SECRET"); secret.has_value())
        {
            configuration.oidcClientSecret = *secret;
        }
        if (configuration.oidcEnabled)
        {
            if (configuration.oidcIssuer.empty() || configuration.oidcClientId.empty() || configuration.oidcRedirectUri.empty())
            {
                return std::unexpected(ConfigurationError{"启用 OIDC 时必须配置 issuer、client_id 与 redirect_uri"});
            }
            if ((!configuration.oidcIssuer.starts_with("https://") && !configuration.oidcIssuer.starts_with("http://127.0.0.1") &&
                 !configuration.oidcIssuer.starts_with("http://localhost")) ||
                (!configuration.oidcRedirectUri.starts_with("https://") && !configuration.oidcRedirectUri.starts_with("http://127.0.0.1") &&
                 !configuration.oidcRedirectUri.starts_with("http://localhost")))
            {
                return std::unexpected(ConfigurationError{"OIDC 的 issuer 与 redirect_uri 必须是 https，或指向本机回环地址"});
            }
            if (configuration.oidcScopes.find("openid") == std::string::npos)
            {
                return std::unexpected(ConfigurationError{"OIDC scopes 必须包含 openid"});
            }
        }
        if (configuration.reverseProxyEnabled)
        {
            if (configuration.reverseProxyUpstream.empty())
            {
                return std::unexpected(ConfigurationError{"启用反向代理时必须配置 edgepulse.reverse_proxy.upstream"});
            }
            if (configuration.reverseProxyPrefix.empty() || configuration.reverseProxyPrefix.front() != '/' || configuration.reverseProxyPrefix == "/" ||
                configuration.reverseProxyPrefix == "/api")
            {
                return std::unexpected(ConfigurationError{"反向代理 prefix 必须以 / 开头，且不能是 / 或 /api"});
            }
            if (configuration.reverseProxyTimeoutMs <= 0 || configuration.reverseProxyMaximumRequestBytes <= 0 || configuration.reverseProxyMaximumResponseBytes <= 0)
            {
                return std::unexpected(ConfigurationError{"反向代理的超时与正文上限必须是正数"});
            }
        }
        // TTL 参与定时器与过期判定，非正数会让条目永不过期或立即过期
        if (configuration.captchaTtlSeconds < kMinimumTtl || configuration.sessionTtlSeconds < kMinimumTtl)
        {
            return std::unexpected(ConfigurationError{"captcha/session 的 TTL 必须是正数"});
        }
        if (!std::isfinite(configuration.requestsPerSecond) || configuration.requestsPerSecond < 0.0)
        {
            return std::unexpected(ConfigurationError{"server.rate_limit.requests_per_second 必须是不小于 0 的有限数；关闭限流请写 0"});
        }
        // 令牌桶容量小于 1 时任何请求都取不到令牌，那是配置错误而不是更严格的限流
        if (!std::isfinite(configuration.rateLimitBurstCapacity) || configuration.rateLimitBurstCapacity < 1.0)
        {
            return std::unexpected(ConfigurationError{"server.rate_limit.burst_capacity 必须不小于 1.0"});
        }
        if (configuration.idleTimeoutMs < 0 || configuration.readTimeoutMs < 0 || configuration.writeTimeoutMs < 0)
        {
            return std::unexpected(ConfigurationError{"server.limits 下的超时毫秒数不能为负数；0 表示关闭该项保护"});
        }
        if (configuration.maximumRequestBodyBytes <= 0)
        {
            return std::unexpected(ConfigurationError{"server.parser_limits.maximum_body_size 必须是正数"});
        }
        if (configuration.passwordMinimumLength < 8 || configuration.passwordMinimumLength > 64)
        {
            return std::unexpected(ConfigurationError{"edgepulse.security.password_min_length 必须在 8 到 64 之间"});
        }
        if (configuration.loginFailureWindowSeconds < 60 || configuration.loginFailureWindowSeconds > 86400)
        {
            return std::unexpected(ConfigurationError{"edgepulse.security.login_failure_window_seconds 必须在 60 到 86400 之间"});
        }
        if (configuration.loginMaximumFailures < 1 || configuration.loginMaximumFailures > 100)
        {
            return std::unexpected(ConfigurationError{"edgepulse.security.login_max_failures 必须在 1 到 100 之间"});
        }
        // 锁定提示按分钟展示，短于 60 秒会让用户看到「请 0 分钟后再试」
        if (configuration.loginLockSeconds < 60 || configuration.loginLockSeconds > 86400)
        {
            return std::unexpected(ConfigurationError{"edgepulse.security.login_lock_seconds 必须在 60 到 86400 之间"});
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

        const auto udpTelemetryPort = readPort(configurationManager, "edgepulse.udp_telemetry.port", configuration.udpTelemetryPort);
        if (!udpTelemetryPort)
        {
            return std::unexpected(udpTelemetryPort.error());
        }
        configuration.udpTelemetryPort = *udpTelemetryPort;

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
