#include "App/Application.h"

#include "App/ListenerAssembly.h"
#include "Base/Config/ConfigValue.h"
#include "Plugins/AuthPlugin/AuthController.h"
#include "Plugins/AuthPlugin/MfaController.h"
#include "Plugins/AuthPlugin/OidcController.h"
#include "Core/Auth/ApiTokenService.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/OidcService.h"
#include "Core/Auth/CaptchaService.h"
#include "Core/Auth/LoginAudit.h"
#include "Core/Auth/MfaService.h"
#include "Core/Auth/SessionStore.h"
#include "Core/Auth/DataScopeService.h"
#include "Core/Cache/MemoryKeyValueStore.h"
#include "Core/Cache/RedisKeyValueStore.h"
#include "Core/Http/AuditMiddleware.h"
#include "Core/Http/IdempotencyRepository.h"
#include "Base/Api/ApiResponse.h"
#include "Base/Config/ConfigManager.h"
#include "Base/Log/LogMacros.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"
#include "Net/Metrics/MetricsExportConfiguration.h"
#include "Core/Coroutine/Scheduler.h"
#include "Net/Http/Server/ReadinessCheck.h"
#include "Base/Database/Database.h"
#include "Base/Runtime/Environment.h"
#include "Base/Runtime/TimeFormat.h"
#include "Base/Security/OutboundUrlGuard.h"
#include "Plugins/DevicePlugin/AgentController.h"
#include "Core/Device/AgentCertificateAuthority.h"
#include "Core/Device/AgentRegistry.h"
#include "Core/Device/EnrollmentRepository.h"
#include "Core/Device/EnrollmentService.h"
#include "Core/Device/NodeConfigRepository.h"
#include "Core/Device/NodeConfigService.h"
#include "Core/Device/NodeLifecycleService.h"
#include "Plugins/DevicePlugin/DeviceController.h"
#include "Plugins/DevicePlugin/EnrollmentController.h"
#include "Plugins/DevicePlugin/NodeConfigController.h"
#include "Plugins/DevicePlugin/NodeJson.h"
#include "Core/Device/NodeService.h"
#include "Core/Metrics/BusinessMetrics.h"
#include "Core/System/LeaderGate.h"
#include "Core/System/LeaderTick.h"
#include "Core/System/LeaderLease.h"
#include "Core/System/MigrationLock.h"
#include "Core/System/MigrationRunner.h"
#include "Core/System/DictionaryRepository.h"
#include "Core/System/LogRepository.h"
#include "Core/System/MenuRepository.h"
#include "Core/System/OrgRepository.h"
#include "Core/System/RoleRepository.h"
#include "Core/System/UserRepository.h"
#include "Core/System/TenantRepository.h"
#include "Core/Device/NodeGroupRepository.h"
#include "Core/Telemetry/TelemetryRepository.h"
#include "Core/Telemetry/UdpTelemetryEndpoint.h"
#include "Core/Alarm/AlarmRepository.h"
#include "Core/Alarm/AlarmService.h"
#include "Core/Firmware/FirmwareRepository.h"
#include "Core/Firmware/FirmwareSignatureVerifier.h"
#include "Core/Firmware/FirmwareSigningPolicy.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Command/CommandPublishThrottle.h"
#include "Core/Command/AgentChannelRegistry.h"
#include "Core/Command/AgentCommandDelivery.h"
#include "Core/Ops/OpsRepository.h"
#include "Core/Http/SseBroadcaster.h"
#include "Core/Http/AlarmEventStream.h"
#include "Core/Http/AgentWebSocket.h"
#include "Plugins/DevicePlugin/NodeGroupController.h"
#include "Plugins/TelemetryPlugin/TelemetryController.h"
#include "Plugins/AlarmPlugin/AlarmController.h"
#include "Plugins/FirmwarePlugin/FirmwareController.h"
#include "Plugins/CommandPlugin/CommandController.h"
#include "Plugins/OpsPlugin/OpsController.h"
#include "Plugins/DashboardPlugin/DashboardController.h"
#include "Plugins/MonitorPlugin/MonitorController.h"
#include "Plugins/SystemPlugin/SystemController.h"
#include "Plugins/SystemPlugin/TenantController.h"
#include "Plugins/SystemPlugin/ApiTokenController.h"
#include "Plugins/SystemPlugin/NotificationController.h"
#include "Core/Notification/NotificationDispatcher.h"
#include "Core/Notification/NotificationRepository.h"
#include "Core/Notification/NotificationService.h"
#include "Core/Http/MiddlewareSetup.h"
#include "Core/Http/RealtimeBus.h"
#include "Core/Http/RealtimeTopics.h"
#include "Core/Http/ReverseProxy.h"

#include "Core/EventLoop/IoContext.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/Config/HttpServerLimits.h"
#include "Net/Http/Message/HttpParserLimits.h"
#include "Net/Http/Server/HttpServer.h"
#include "Net/Http/Server/HttpServerAssembly.h"
#include "Net/Http/Server/HttpsServer.h"
#include "Net/Http/Routing/Middleware.h"
#include "Net/Http/Routing/Router.h"
#include "Net/Quic/QuicServer.h"
#include "Core/Http/Authorization.h"
#include "Core/Http/RequestId.h"
#include "Core/Http/WebSocketEndpoints.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>

namespace EdgePulse::App
{
    namespace
    {
        /**
         * @brief 取运维口 Bearer 令牌：环境变量优先，其次读未跟踪文件
         * @param tokenFile 令牌文件路径
         * @return 令牌文本；两处都没有时为空
         */
        [[nodiscard]] std::string readOpsToken(const std::string &tokenFile)
        {
            if (const std::optional<std::string> environmentToken = Common::environmentValue("EDGEPULSE_OPS_BEARER_TOKEN"); environmentToken.has_value())
            {
                return *environmentToken;
            }
            std::ifstream file(tokenFile);
            if (!file)
            {
                return {};
            }
            std::string token;
            std::getline(file, token);
            // 去掉行尾空白（含 Windows CRLF）
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())) != 0)
            {
                token.pop_back();
            }
            return token;
        }
    } // namespace

    Application::Application(ApplicationConfiguration configuration) : m_configuration(std::move(configuration))
    {
    }

    Application::~Application()
    {
        m_readiness.setServiceReady(false);
        // 先释放控制面租约，其他实例不必等 TTL 过期；循环已经停止，这里只做一次直连清理
        if (m_leaderLease)
        {
            m_leaderLease->stop();
        }
        AsynGyanis::Base::ConfigManager::instance().disableHotReload();
        if (m_healthProbe.joinable())
        {
            m_healthProbe.request_stop();
            m_healthProbe.join();
        }
        if (m_firmwareProbe.joinable())
        {
            m_firmwareProbe.request_stop();
            m_firmwareProbe.join();
        }
        if (m_metricsExporter)
        {
            m_metricsExporter->shutdown();
        }
        m_shutdownTask.reset();
        if (m_stopThread.has_value() && m_stopThread->joinable())
        {
            m_stopThread->join();
        }
    }

    bool Application::start(std::string &error)
    {
        // P0 单进程单循环：所有监听器共享同一条事件循环，跨线程投递与锁的开销都不发生
        m_ioContext                       = std::make_unique<AsynGyanis::Core::IoContext>(static_cast<std::size_t>(m_configuration.threads));
        AsynGyanis::Core::EventLoop &loop = m_ioContext->threadPool().eventLoop(0);

        Common::DatabaseOptions databaseOptions;
        databaseOptions.driver          = m_configuration.databaseDriver;
        databaseOptions.path            = m_configuration.databasePath;
        databaseOptions.host            = m_configuration.databaseHost;
        databaseOptions.port            = m_configuration.databasePort;
        databaseOptions.userName        = m_configuration.databaseUser;
        databaseOptions.password        = m_configuration.databasePassword;
        databaseOptions.database        = m_configuration.databaseName;
        databaseOptions.maximumPoolSize = 8;

        try
        {
            // 启动期在独立连接上应用迁移；MySQL 先拿命名锁，SQLite 按单写者直接放行
            {
                const std::expected<std::unique_ptr<System::MigrationLock>, std::string> migrationLock = System::MigrationLock::acquire(databaseOptions, 30);
                if (!migrationLock)
                {
                    error = "获取迁移锁失败，实例拒绝启动：" + migrationLock.error();
                    return false;
                }
                const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = Common::Database::openDirect(databaseOptions);
                System::MigrationRunner::apply(*connection, m_configuration.migrationsDirectory);
            }
            m_database    = Common::Database::open(databaseOptions);
            m_leaderLease = std::make_unique<System::LeaderLease>(m_database, m_configuration.instanceId);
            LOG_INFO_FMT("控制面租约已装配：instance={} HA={}", m_configuration.instanceId, m_configuration.haEnabled ? "启用" : "关闭");
            if (m_configuration.redisEnabled)
            {
                m_cache = std::make_shared<Cache::RedisKeyValueStore>(m_configuration.redisHost, m_configuration.redisPort, m_configuration.redisPassword,
                                                                      m_configuration.redisDatabase);
            } else
            {
                m_cache = std::make_shared<Cache::MemoryKeyValueStore>();
            }
            m_tenantRepository = std::make_shared<System::TenantRepository>(m_database);
            m_sessionStore     = std::make_shared<Auth::SessionStore>(m_cache, m_configuration.sessionTtlSeconds);
            m_captchaService   = std::make_shared<Auth::CaptchaService>(m_cache, Auth::CaptchaOptions{m_configuration.captchaTtlSeconds, m_configuration.captchaRequestsPerMinute});
            m_loginAudit       = std::make_shared<Auth::LoginAudit>(m_database);
            Auth::AuthOptions authOptions;
            authOptions.passwordMinimumLength     = m_configuration.passwordMinimumLength;
            authOptions.passwordRequireComplexity = m_configuration.passwordRequireComplexity;
            authOptions.loginFailureWindowSeconds = m_configuration.loginFailureWindowSeconds;
            authOptions.loginMaximumFailures      = m_configuration.loginMaximumFailures;
            authOptions.loginLockSeconds          = m_configuration.loginLockSeconds;
            m_authService = std::make_shared<Auth::AuthService>(m_database, m_sessionStore, m_captchaService, m_loginAudit, m_cache, m_tenantRepository, authOptions);
            m_mfaService  = std::make_shared<Auth::MfaService>(m_database, m_cache, m_configuration.mfaEncryptionKey);
            // 登录与外部身份签发共用同一个两步验证服务，装配阶段绑定一次
            m_authService->setMfaService(m_mfaService);
            // 只报告是否配置；密钥内容与长度都不进日志
            LOG_INFO_FMT("两步验证加密密钥：{}", m_configuration.mfaEncryptionKey.empty() ? "未配置" : "已配置");
            m_browserWebSocket = std::make_shared<Web::BrowserWebSocket>();
            m_sseBroadcaster   = std::make_shared<Web::SseBroadcaster>();
            Web::RealtimeBusOptions realtimeOptions;
            realtimeOptions.enabled    = m_configuration.redisEnabled;
            realtimeOptions.instanceId = m_configuration.instanceId;
            realtimeOptions.host       = m_configuration.redisHost;
            realtimeOptions.port       = m_configuration.redisPort;
            realtimeOptions.password   = m_configuration.redisPassword;
            realtimeOptions.database   = m_configuration.redisDatabase;
            m_realtimeBus              = std::make_unique<Web::RealtimeBus>(loop, *m_browserWebSocket, *m_sseBroadcaster, std::move(realtimeOptions));
            m_nodeRepository           = std::make_shared<Device::NodeRepository>(m_database);
            // 离线判定按进程汇总；仓储默认只看默认租户，周期扫描必须显式取全租户视图
            m_fleetNodes           = m_nodeRepository->scoped({0, true});
            m_nodeConfigRepository = std::make_shared<Device::NodeConfigRepository>(m_database);
            m_enrollmentRepository = std::make_shared<Device::EnrollmentRepository>(m_database);
            m_enrollmentService    = std::make_shared<Device::EnrollmentService>(m_enrollmentRepository);
            // CA 私钥只在装配阶段读入；未启用时也保留一个禁用实例，后续路由按 enabled() 短路
            Device::AgentCaOptions agentCaOptions;
            agentCaOptions.enabled         = m_configuration.agentCaEnabled;
            agentCaOptions.certificateFile = m_configuration.agentCaCertificateFile;
            agentCaOptions.keyFile         = m_configuration.agentCaKeyFile;
            agentCaOptions.validityDays    = m_configuration.agentCaValidityDays;
            agentCaOptions.overlapHours    = m_configuration.agentCaOverlapHours;
            // 9443 的客户端信任锚必须包含签发 CA，否则新证书在 TLS 握手阶段会被判成陌生 CA；监听器关闭时不做这项比对
            if (m_configuration.agentPort != 0)
            {
                agentCaOptions.trustedCertificateFile = std::filesystem::path{m_configuration.certificateDirectory} / "ca.pem";
            }
            auto agentCertificateAuthority = Device::AgentCertificateAuthority::load(agentCaOptions);
            if (!agentCertificateAuthority)
            {
                error = "Agent 内部 CA 初始化失败：" + agentCertificateAuthority.error();
                return false;
            }
            m_agentCa = std::make_unique<Device::AgentCertificateAuthority>(std::move(*agentCertificateAuthority));
            LOG_INFO_FMT("Agent 内部 CA：{}", m_agentCa->enabled() ? "已启用" : "未启用");
            m_nodeService            = std::make_shared<Device::NodeService>(m_nodeRepository);
            m_alarmRepository        = std::make_shared<Alarm::AlarmRepository>(m_database);
            m_notificationRepository = std::make_shared<Notification::NotificationRepository>(m_database);
            m_outboundUrlGuard       = std::make_shared<Common::OutboundUrlGuard>(Common::OutboundUrlGuardOptions{.allowPrivateNetworks = m_configuration.allowPrivateWebhooks});
            m_notificationService    = std::make_shared<Notification::NotificationService>(m_notificationRepository, m_outboundUrlGuard);
            m_notificationDispatcher = std::make_shared<Notification::NotificationDispatcher>(loop, m_notificationService);
            m_alarmService           = std::make_shared<Alarm::AlarmService>(
                    m_alarmRepository,
                    [bus = m_realtimeBus.get(), dispatcher = m_notificationDispatcher](const Alarm::AlarmRecord &alarm) -> AsynGyanis::Core::Task<>
                    {
                        const std::string payload = AsynGyanis::Base::ConfigValue::object({{"type", "alarm"},
                                                                                           {"data", AsynGyanis::Base::ConfigValue::object({{"id", alarm.id},
                                                                                                                                           {"ruleId", alarm.ruleId},
                                                                                                                                           {"nodeId", alarm.nodeId},
                                                                                                                                           {"nodeName", alarm.nodeName},
                                                                                                                                           {"level", alarm.level},
                                                                                                                                           {"metric", alarm.metric},
                                                                                                                                           {"value", alarm.value},
                                                                                                                                           {"threshold", alarm.threshold},
                                                                                                                                           {"status", alarm.status},
                                                                                                                                           {"message", alarm.message},
                                                                                                                                           {"triggeredAt", alarm.triggeredAt},
                                                                                                                                           {"resolvedAt", alarm.resolvedAt}})}})
                                                            .dump();
                        co_await bus->publish("alarms", alarm.tenantId, payload);
                        dispatcher->enqueue(alarm);
                        co_return;
                    });
            m_lifecycle = std::make_shared<Device::NodeLifecycleService>(
                    *m_fleetNodes, *m_alarmService, [this](const Device::NodeRecord &node) -> AsynGyanis::Core::Task<> { co_await publishNodeStatus(node); },
                    m_configuration.heartbeatTimeoutSeconds);
            Device::AgentRegistry::Options agentOptions;
            if (!m_configuration.agentFingerprint.empty())
            {
                agentOptions.allowedFingerprints.push_back(m_configuration.agentFingerprint);
            }
            agentOptions.enrollmentMode = m_configuration.enrollmentMode;
            agentOptions.enrollments    = m_enrollmentService.get();
            agentOptions.publisher      = [lifecycle = m_lifecycle, &loop](const Device::NodeRecord &node, const bool becameOnline) -> AsynGyanis::Core::Task<>
            { co_await lifecycle->onHeartbeat(loop, node, becameOnline); };
            m_agentRegistry  = std::make_shared<Device::AgentRegistry>(m_nodeRepository, std::move(agentOptions));
            m_userRepository = std::make_shared<System::UserRepository>(m_database);
            // 令牌服务依赖用户仓储，装配顺序在认证服务之后，用迟绑定挂回唯一的鉴权入口
            m_apiTokenService = std::make_shared<Auth::ApiTokenService>(m_database, m_userRepository, m_tenantRepository);
            m_authService->setApiTokenService(m_apiTokenService);
            if (m_configuration.oidcEnabled)
            {
                Auth::OidcConfiguration oidcConfiguration;
                oidcConfiguration.enabled             = true;
                oidcConfiguration.issuer              = m_configuration.oidcIssuer;
                oidcConfiguration.clientId            = m_configuration.oidcClientId;
                oidcConfiguration.clientSecret        = m_configuration.oidcClientSecret;
                oidcConfiguration.redirectUri         = m_configuration.oidcRedirectUri;
                oidcConfiguration.frontendRedirect    = m_configuration.oidcFrontendRedirect;
                oidcConfiguration.scopes              = m_configuration.oidcScopes;
                oidcConfiguration.autoProvision       = m_configuration.oidcAutoProvision;
                oidcConfiguration.defaultRoleId       = m_configuration.oidcDefaultRoleId;
                oidcConfiguration.defaultDepartmentId = m_configuration.oidcDefaultDepartmentId;
                oidcConfiguration.allowedEmailDomains = m_configuration.oidcAllowedEmailDomains;
                oidcConfiguration.tenantCode          = m_configuration.oidcTenantCode;
                m_oidcService                         = std::make_unique<Auth::OidcService>(oidcConfiguration, *m_userRepository, *m_tenantRepository, *m_cache, *m_authService);
            }
            m_roleRepository       = std::make_shared<System::RoleRepository>(m_database);
            m_menuRepository       = std::make_shared<System::MenuRepository>(m_database);
            m_orgRepository        = std::make_shared<System::OrgRepository>(m_database);
            m_dictionaryRepository = std::make_shared<System::DictionaryRepository>(m_database);
            m_logRepository        = std::make_shared<System::LogRepository>(m_database);
            // 中间件与保留期清理共用同一个仓储，过期幂等行才能被周期扫描物理删掉
            m_idempotencyRepository = std::make_shared<Web::IdempotencyRepository>(m_database);
            m_dataScopeService      = std::make_shared<Auth::DataScopeService>(m_database);
            m_nodeGroupRepository   = std::make_shared<Device::NodeGroupRepository>(m_database);
            m_telemetryRepository   = std::make_shared<Telemetry::TelemetryRepository>(m_database);
            m_firmwareRepository    = std::make_shared<Firmware::FirmwareRepository>(m_database);
            // 信任键在装配阶段一次读入；强制签名但目录无键时直接拒绝启动，避免上线后才发现门禁形同虚设
            auto verifier = Firmware::FirmwareSignatureVerifier::loadFromDirectory(m_configuration.firmwareTrustedKeysDirectory);
            if (!verifier.has_value())
            {
                error = "固件信任键加载失败：" + verifier.error();
                return false;
            }
            if (m_configuration.firmwareRequireSignature && verifier->empty())
            {
                error = "require_signature=true 但信任键目录为空：" + m_configuration.firmwareTrustedKeysDirectory;
                return false;
            }
            auto verifierPointer = std::make_shared<Firmware::FirmwareSignatureVerifier>(std::move(*verifier));
            m_signingPolicy =
                    std::make_shared<Firmware::FirmwareSigningPolicy>(Firmware::FirmwareSigningPolicy{m_configuration.firmwareRequireSignature, std::move(verifierPointer)});
            m_commandRepository = std::make_shared<Command::CommandRepository>(m_database);
            m_agentChannels     = std::make_shared<Command::AgentChannelRegistry>();
            // Agent 消息的本地投递口：通道写出成功后才把命令从 pending 推进到 running；
            // 本实例没有连接时保持待执行，跨实例转发由总线继续完成。推进成功后补发一次
            // command.status，浏览器不必等下一次列表刷新才能看到 running
            auto agentCommandDelivery = std::make_shared<Command::AgentCommandDelivery>(*m_agentChannels, m_commandRepository->scoped({0, true}),
                                                                                        [this](const Command::CommandRecord &command) -> AsynGyanis::Core::Task<>
                                                                                        { co_await publishCommandStatus(command); });
            m_realtimeBus->setAgentMessageHandler([agentCommandDelivery, &loop](const Web::AgentMessage &message) -> AsynGyanis::Core::Task<>
                                                  { co_await agentCommandDelivery->deliver(loop, message); });
            // 投递口先就位再开订阅：启动窗口里到达的远端 Agent 命令不会因 handler 为空被丢弃
            m_realtimeBus->start();
            m_nodeConfigService = std::make_shared<Device::NodeConfigService>(m_nodeConfigRepository, m_nodeRepository, m_commandRepository, m_realtimeBus.get(),
                                                                              [this](const Command::CommandRecord &command) -> AsynGyanis::Core::Task<>
                                                                              { co_await publishCommandStatus(command); });
            m_opsRepository     = std::make_shared<Ops::OpsRepository>(m_database);

            if (m_configuration.udpTelemetryEnabled)
            {
                Telemetry::UdpTelemetryEndpoint::Configuration udpConfiguration;
                udpConfiguration.address = m_configuration.udpTelemetryAddress;
                udpConfiguration.port    = m_configuration.udpTelemetryPort;
                udpConfiguration.secret  = m_configuration.udpTelemetrySecret;
                m_udpTelemetry           = std::make_unique<Telemetry::UdpTelemetryEndpoint>(loop, udpConfiguration, *m_nodeRepository, *m_telemetryRepository, *m_alarmService);
                m_udpListen.emplace(m_udpTelemetry->listen());
                loop.scheduler().schedule(m_udpListen->handle());
                LOG_INFO_FMT("UDP 遥测监听已排入事件循环：{}:{}", m_configuration.udpTelemetryAddress, m_configuration.udpTelemetryPort);
            }

            m_readiness.setDatabaseReady(m_database->probe());
            // 固件目录在首次上传前可能不存在；启动时补建一次，失败留给就绪探针持续报告
            std::error_code firmwareDirectoryError;
            std::filesystem::create_directories(m_configuration.firmwareDirectory, firmwareDirectoryError);
            if (firmwareDirectoryError)
            {
                LOG_WARN_FMT("创建固件目录失败：{}：{}", m_configuration.firmwareDirectory, firmwareDirectoryError.message());
            }
            m_readiness.setFirmwareDirectoryReady(Web::probeDirectoryWritable(m_configuration.firmwareDirectory));
            if (!m_readiness.snapshot().firmwareDirectoryReady)
            {
                LOG_WARN_FMT("固件目录不可写：{}；/readyz 将在目录恢复前返回 503", m_configuration.firmwareDirectory);
            }
            m_healthProbe = std::jthread(
                    [this](const std::stop_token stopToken)
                    {
                        while (!stopToken.stop_requested())
                        {
                            m_readiness.setDatabaseReady(m_database->probe());
                            for (int slice = 0; slice < 20 && !stopToken.stop_requested(); ++slice)
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds{100});
                            }
                        }
                    });
            // 目录探针涉及磁盘写入，单独一条线程：数据库探针卡住时固件目录结论仍按十秒周期更新
            m_firmwareProbe = std::jthread(
                    [this](const std::stop_token stopToken)
                    {
                        for (int slice = 0; slice < 100 && !stopToken.stop_requested(); ++slice)
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds{100});
                        }
                        while (!stopToken.stop_requested())
                        {
                            m_readiness.setFirmwareDirectoryReady(Web::probeDirectoryWritable(m_configuration.firmwareDirectory));
                            for (int slice = 0; slice < 100 && !stopToken.stop_requested(); ++slice)
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds{100});
                            }
                        }
                    });

            auto &configurationManager = AsynGyanis::Base::ConfigManager::instance();
            if (configurationManager.getBool("edgepulse.hot_reload", true))
            {
                configurationManager.enableHotReload(
                        [this](const AsynGyanis::Base::ConfigLoadResult &result)
                        {
                            if (!result)
                            {
                                LOG_WARN("配置热重载失败：保留上一版配置");
                                return;
                            }
                            auto &manager = AsynGyanis::Base::ConfigManager::instance();
                            m_sessionStore->setTtlSeconds(static_cast<int>(manager.getInt("edgepulse.session.ttl_seconds", m_sessionStore->ttlSeconds())));
                            m_captchaService->setOptions(Auth::CaptchaOptions{static_cast<int>(manager.getInt("edgepulse.captcha.ttl_seconds", 120)),
                                                                              static_cast<int>(manager.getInt("edgepulse.captcha.requests_per_minute", 30))});
                            LOG_INFO("配置已热重载：会话与验证码参数生效；监听地址与端口需要重启");
                        });
            }

            m_buildInfoMetric =
                    AsynGyanis::Core::ProcessMetricsRegistry::registerMetric("edgepulse_build_info", "EdgePulse 构建信息（恒为 1）", AsynGyanis::Core::ProcessMetricKind::Gauge,
                                                                             AsynGyanis::Core::ProcessMetricMerge::Sum, [] { return std::uint64_t{1}; });
            Metrics::BusinessMetrics::instance().registerMetrics();

            if (configurationManager.getBool("metrics.enabled", false))
            {
                AsynGyanis::Net::MetricsExportConfiguration metricsConfiguration;
                metricsConfiguration.enabled            = true;
                metricsConfiguration.serviceName        = configurationManager.getString("metrics.service_name", "EdgePulse");
                metricsConfiguration.serviceVersion     = configurationManager.getString("metrics.service_version", "1.0.0");
                metricsConfiguration.otlpEndpoint       = configurationManager.getString("metrics.otlp.endpoint", "");
                metricsConfiguration.exportInterval     = std::chrono::milliseconds{configurationManager.getInt("metrics.export_interval_ms", 15000)};
                metricsConfiguration.otlpRequestTimeout = std::chrono::milliseconds{configurationManager.getInt("metrics.otlp.timeout_ms", 3000)};
                m_metricsExporter                       = AsynGyanis::Net::buildMetricsExporter(metricsConfiguration);
            }
        } catch (const std::exception &failure)
        {
            // 数据库或迁移不可用时没有可服务的业务，直接把原因交给进程入口
            error = std::string{"数据库初始化失败："} + failure.what();
            return false;
        }

        // 公开 HTTP/HTTPS 与 h3 接受固件上传，解析器上限给固件留出余量；Agent 与运维口只收小正文
        AsynGyanis::Net::HttpServerLimits limits;
        limits.idleTimeout                          = std::chrono::milliseconds{m_configuration.idleTimeoutMs};
        limits.readTimeout                          = std::chrono::milliseconds{m_configuration.readTimeoutMs};
        limits.writeTimeout                         = std::chrono::milliseconds{m_configuration.writeTimeoutMs};
        const std::size_t configuredMaximumBodySize = static_cast<std::size_t>(m_configuration.maximumRequestBodyBytes);
        const std::size_t uploadMaximumBodySize     = std::max(configuredMaximumBodySize, static_cast<std::size_t>(m_configuration.maximumFirmwareBytes));
        LOG_INFO_FMT("连接限额已生效：空闲 {} ms、读取 {} ms、写入 {} ms；Agent 与运维口正文上限 {} 字节，公开口与 h3 正文上限 {} 字节", m_configuration.idleTimeoutMs,
                     m_configuration.readTimeoutMs, m_configuration.writeTimeoutMs, configuredMaximumBodySize, uploadMaximumBodySize);

        if (m_configuration.requestsPerSecond > 0.0)
        {
            // 全进程只建一个桶：每个监听器各持一份会让实际速率乘以监听器数量
            m_rateLimitBucket = std::make_shared<AsynGyanis::Net::TokenBucket>(m_configuration.requestsPerSecond, m_configuration.rateLimitBurstCapacity);
            LOG_INFO_FMT("全局限流已启用：每秒 {} 个请求，桶容量 {}", m_configuration.requestsPerSecond, m_configuration.rateLimitBurstCapacity);
        } else
        {
            LOG_INFO("全局限流已关闭：server.rate_limit.requests_per_second 为 0");
        }

        const auto applyParserLimits = [](auto &server, const std::size_t maximumBodySize)
        {
            AsynGyanis::Net::HttpParserLimits parserLimits = server.parserLimits();
            parserLimits.maximumBodySize                   = maximumBodySize;
            server.setParserLimits(parserLimits);
        };

        const auto resolveAddress = [&error](const std::string &host, const std::uint16_t port, const std::string_view label) -> std::optional<AsynGyanis::Core::InetAddress>
        {
            const std::optional<AsynGyanis::Core::InetAddress> address = AsynGyanis::Core::InetAddress::resolve(host, port);
            if (!address.has_value())
            {
                error = "无法解析" + std::string{label} + "监听地址：" + host + ":" + std::to_string(port);
            }
            return address;
        };

        try
        {
            const std::optional<AsynGyanis::Core::InetAddress> httpAddress = resolveAddress(m_configuration.listenAddress, m_configuration.httpPort, "HTTP");
            if (!httpAddress.has_value())
            {
                return false;
            }

            // 8080：开发档明文口，Vite 代理目标
            std::error_code webRootError;
            m_webRoot    = std::filesystem::path(m_configuration.webRoot);
            m_httpServer = std::make_unique<AsynGyanis::Net::HttpServer>(loop, *httpAddress);
            m_httpServer->setLimits(limits);
            applyParserLimits(*m_httpServer, uploadMaximumBodySize);
            registerPublicRoutes(m_httpServer->router(), loop);
            m_acceptTask.emplace(m_httpServer->start());
            loop.scheduler().schedule(m_acceptTask->handle());

            const std::filesystem::path certificateDirectory = m_configuration.certificateDirectory;
            const std::filesystem::path serverCertificate    = certificateDirectory / "server.pem";
            const std::filesystem::path serverKey            = certificateDirectory / "server-key.pem";
            const std::filesystem::path caCertificate        = certificateDirectory / "ca.pem";
            const auto                  requireCertificate   = [&error](const std::filesystem::path &path, const std::string_view label)
            {
                if (!std::filesystem::exists(path))
                {
                    error = "缺少" + std::string{label} + "：" + path.string() + "；请先运行 scripts/dev-certs.sh";
                    return false;
                }
                return true;
            };

            if (m_configuration.httpsPort != 0)
            {
                if (!requireCertificate(serverCertificate, "服务器证书") || !requireCertificate(serverKey, "服务器私钥"))
                {
                    return false;
                }
                const std::optional<AsynGyanis::Core::InetAddress> httpsAddress = resolveAddress(m_configuration.listenAddress, m_configuration.httpsPort, "HTTPS");
                if (!httpsAddress.has_value())
                {
                    return false;
                }

                // 8443/TCP：HTTP/2 控制台，Alt-Svc 通告同端口 UDP 上的 h3
                m_httpsServer = std::make_unique<AsynGyanis::Net::HttpsServer>(loop, *httpsAddress, serverCertificate.string(), serverKey.string());
                m_httpsServer->setLimits(limits);
                applyParserLimits(*m_httpsServer, uploadMaximumBodySize);
                m_httpsServer->router().addMiddleware(AsynGyanis::Net::altSvcMiddleware(m_configuration.httpsPort));
                registerPublicRoutes(m_httpsServer->router(), loop);
                m_httpsAccept.emplace(m_httpsServer->start());
                loop.scheduler().schedule(m_httpsAccept->handle());

                // 8443/UDP：独立 QUIC 路由器；/api/probe/h3 只在这里存在，TCP 回落必然 404
                AsynGyanis::Net::QuicServer::Configuration quicConfiguration;
                quicConfiguration.certificateFile = serverCertificate.string();
                quicConfiguration.privateKeyFile  = serverKey.string();
                quicConfiguration.idleTimeout     = std::chrono::seconds{m_configuration.h3IdleTimeoutSeconds};
                m_h3Router                        = std::make_unique<AsynGyanis::Net::Router>();
                m_h3Router->get("/api/probe/h3",
                                [](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    Common::writeSuccess(response, AsynGyanis::Base::ConfigValue::object({{"proto", "h3"}}), Web::resolveRequestId(request));
                                    co_return;
                                });
                registerPublicRoutes(*m_h3Router, loop);
                m_quicServer = std::make_unique<AsynGyanis::Net::QuicServer>(loop, quicConfiguration);
                m_quicServer->setLimits(limits);
                applyParserLimits(*m_quicServer, uploadMaximumBodySize);
                m_quicServer->setRouter(*m_h3Router);
                m_quicListen.emplace(m_quicServer->listen(*httpsAddress));
                loop.scheduler().schedule(m_quicListen->handle());
            }

            if (m_configuration.agentPort != 0)
            {
                if (!requireCertificate(serverCertificate, "服务器证书") || !requireCertificate(serverKey, "服务器私钥") || !requireCertificate(caCertificate, "客户端 CA"))
                {
                    return false;
                }
                const std::optional<AsynGyanis::Core::InetAddress> agentAddress = resolveAddress(m_configuration.listenAddress, m_configuration.agentPort, "Agent mTLS");
                if (!agentAddress.has_value())
                {
                    return false;
                }
                // 9443：只挂 Agent 路由；要求客户端证书，公共浏览器路由不得出现在这里
                m_agentServer = std::make_unique<AsynGyanis::Net::HttpsServer>(loop, *agentAddress, serverCertificate.string(), serverKey.string());
                m_agentServer->setLimits(limits);
                applyParserLimits(*m_agentServer, configuredMaximumBodySize);
                if (!m_agentServer->loadClientCertificateAuthority(caCertificate.string()))
                {
                    error = "加载客户端 CA 失败：" + caCertificate.string();
                    return false;
                }
                m_agentServer->setClientCertificateRequired(true);
                Web::applySecurityHeaders(m_agentServer->router());
                Device::registerRoutes(m_agentServer->router(), loop, *m_agentRegistry, *m_nodeRepository, *m_tenantRepository, m_configuration.heartbeatIntervalSeconds,
                                       m_nodeConfigRepository.get(), m_agentCa.get());
                Device::NodeConfigDependencies nodeConfigAgentDependencies;
                nodeConfigAgentDependencies.nodes   = m_nodeRepository.get();
                nodeConfigAgentDependencies.configs = m_nodeConfigRepository.get();
                nodeConfigAgentDependencies.service = m_nodeConfigService.get();
                Device::registerNodeConfigAgentRoutes(m_agentServer->router(), loop, nodeConfigAgentDependencies);
                Telemetry::TelemetryDependencies telemetryAgentDependencies;
                telemetryAgentDependencies.auth      = m_authService.get();
                telemetryAgentDependencies.nodes     = m_nodeRepository.get();
                telemetryAgentDependencies.telemetry = m_telemetryRepository.get();
                telemetryAgentDependencies.alarms    = m_alarmService.get();
                Telemetry::registerTelemetryAgentRoutes(m_agentServer->router(), loop, telemetryAgentDependencies);
                Command::CommandDependencies commandAgentDependencies;
                commandAgentDependencies.auth        = m_authService.get();
                commandAgentDependencies.commands    = m_commandRepository.get();
                commandAgentDependencies.realtimeBus = m_realtimeBus.get();
                commandAgentDependencies.nodes       = m_nodeRepository.get();
                Command::registerCommandAgentRoutes(m_agentServer->router(), loop, commandAgentDependencies);
                Firmware::FirmwareDependencies firmwareAgentDependencies;
                firmwareAgentDependencies.auth               = m_authService.get();
                firmwareAgentDependencies.firmware           = m_firmwareRepository.get();
                firmwareAgentDependencies.nodes              = m_nodeRepository.get();
                firmwareAgentDependencies.commands           = m_commandRepository.get();
                firmwareAgentDependencies.realtimeBus        = m_realtimeBus.get();
                firmwareAgentDependencies.storageDirectory   = m_configuration.firmwareDirectory;
                firmwareAgentDependencies.maximumUploadBytes = static_cast<std::size_t>(m_configuration.maximumFirmwareBytes);
                firmwareAgentDependencies.signingPolicy      = m_signingPolicy;
                Firmware::registerFirmwareAgentRoutes(m_agentServer->router(), loop, firmwareAgentDependencies);
                Web::registerAgentWebSocket(m_agentServer->router(), loop, *m_nodeRepository, *m_commandRepository, *m_agentChannels, m_lifecycle.get());
                m_agentAccept.emplace(m_agentServer->start());
                loop.scheduler().schedule(m_agentAccept->handle());
            }

            if (m_configuration.opsPort != 0)
            {
                const std::optional<AsynGyanis::Core::InetAddress> opsAddress = resolveAddress(m_configuration.opsAddress, m_configuration.opsPort, "运维");
                if (!opsAddress.has_value())
                {
                    return false;
                }
                const std::string opsToken = readOpsToken(m_configuration.opsTokenFile);
                if (opsToken.empty())
                {
                    error = "缺少运维口 Bearer 令牌：请设置 EDGEPULSE_OPS_BEARER_TOKEN 或写入 " + m_configuration.opsTokenFile;
                    return false;
                }
                AsynGyanis::Net::HttpServerConfiguration opsConfiguration;
                opsConfiguration.exposeMetrics                  = true;
                opsConfiguration.opsBearerToken                 = opsToken;
                const AsynGyanis::Net::ReadinessCheck readiness = [this]() -> AsynGyanis::Net::ReadinessCheckResult
                {
                    if (m_realtimeBus)
                    {
                        m_readiness.setRealtimeHealthy(m_realtimeBus->isHealthy());
                    }
                    // 只读上一轮探针结论与原子量；真正的数据库、缓存、目录探测都在后台跑
                    return m_readiness.check();
                };
                m_opsServer = std::make_unique<AsynGyanis::Net::HttpServer>(loop, *opsAddress);
                m_opsServer->setLimits(limits);
                applyParserLimits(*m_opsServer, configuredMaximumBodySize);
                AsynGyanis::Net::registerOperationEndpoints(*m_opsServer, opsConfiguration, readiness);
                m_opsAccept.emplace(m_opsServer->start());
                loop.scheduler().schedule(m_opsAccept->handle());
            }
        } catch (const std::exception &failure)
        {
            error = std::string{"监听器启动失败："} + failure.what();
            return false;
        }
        // 租约循环先于周期任务启动：start 返回首次获取结果，之后由循环按续约周期维护
        m_leaderLeaseStart.emplace(m_leaderLease->start(loop));
        loop.scheduler().schedule(m_leaderLeaseStart->handle());
        m_commandScheduler = runCommandScheduler(loop);
        loop.scheduler().schedule(m_commandScheduler->handle());
        m_fleetLifecycle = runFleetLifecycle(loop);
        loop.scheduler().schedule(m_fleetLifecycle->handle());
        // 幂等记录固定 24 小时 TTL；清理与其它保留期任务一样只在 leader 上执行
        m_dataRetention = runDataRetention(loop);
        loop.scheduler().schedule(m_dataRetention->handle());
        m_readinessProbe = runReadinessProbe(loop);
        loop.scheduler().schedule(m_readinessProbe->handle());
        if (m_configuration.telemetryRetentionDays <= 0 && m_configuration.logRetentionDays <= 0 && m_configuration.deliveryRetentionDays <= 0)
        {
            LOG_WARN("遥测、日志与投递记录的保留天数都不是正数，三类记录不做过期清理；幂等记录仍按 TTL 清理");
        }
        // serviceReady 由首轮缓存探针置位：先探测再对外宣布就绪，/readyz 不会把「尚未探测」误报成「缓存探针失败」
        return true;
    }

    AsynGyanis::Core::Task<> Application::runCommandScheduler(AsynGyanis::Core::EventLoop &loop)
    {
        AsynGyanis::Core::Timer                           ticker(loop);
        const std::shared_ptr<Command::CommandRepository> allCommands = m_commandRepository->scoped({0, true});
        System::LeaderGate                                leaderGate(
                "计划命令调度", m_configuration.instanceId, [this] { return m_leaderLease != nullptr && m_leaderLease->isLeader(); },
                [this] { return m_leaderLease != nullptr ? m_leaderLease->fence() : 0; });
        // 没有 Agent 连接时到期命令会一直留在 pending；同一命令每 2 秒重发一次只会制造无意义的
        // 跨实例扇出，因此 leader 在内存里按命令标识节流。领导权切换后新 leader 从空表开始
        Command::CommandPublishThrottle dueThrottle;
        // follower 只等待：计划命令是控制面单飞工作，多实例同时推进会把同一条命令重复下发
        System::LeaderTick leaderTick(leaderGate,
                                      [this, &loop, allCommands, &dueThrottle]() -> AsynGyanis::Core::Task<>
                                      {
                                          std::vector<Command::CommandRecord> due;
                                          try
                                          {
                                              due = co_await allCommands->listDue(loop, 100);
                                          } catch (const std::exception &failure)
                                          {
                                              LOG_WARN_FMT("计划命令扫描失败：{}", failure.what());
                                              co_return;
                                          }
                                          std::unordered_set<std::int64_t> dueIds;
                                          dueIds.reserve(due.size());
                                          for (const Command::CommandRecord &command: due)
                                          {
                                              dueIds.insert(command.id);
                                          }
                                          // 已执行、已取消或计划时间未到的命令不会再出现在 due 里，
                                          // 对应节流记录随之清掉，标识表不会无限增长
                                          dueThrottle.prune(dueIds);
                                          for (const Command::CommandRecord &command: due)
                                          {
                                              try
                                              {
                                                  if (m_realtimeBus == nullptr)
                                                  {
                                                      continue;
                                                  }
                                                  if (!dueThrottle.shouldPublish(command.id))
                                                  {
                                                      continue;
                                                  }
                                                  // 由持有该节点连接的实例写出；本实例没有连接时消息仍会跨实例转发
                                                  co_await m_realtimeBus->publishAgent(loop, Command::makeAgentCommandMessage(command));
                                                  const auto updated = co_await allCommands->find(loop, command.id);
                                                  if (!updated.has_value() || updated->status == command.status)
                                                  {
                                                      continue;
                                                  }
                                                  co_await publishCommandStatus(*updated);
                                              } catch (const std::exception &failure)
                                              {
                                                  LOG_WARN_FMT("计划命令下发失败：command={} error={}", command.id, failure.what());
                                              }
                                          }
                                      });
        while (true)
        {
            co_await ticker.waitFor(std::chrono::seconds{2});
            co_await leaderTick.run();
        }
    }

    AsynGyanis::Core::Task<> Application::runDataRetention(AsynGyanis::Core::EventLoop &loop)
    {
        AsynGyanis::Core::Timer                                     ticker(loop);
        const std::shared_ptr<Telemetry::TelemetryRepository>       allTelemetry  = m_telemetryRepository->scoped({0, true});
        const std::shared_ptr<Notification::NotificationRepository> allDeliveries = m_notificationRepository->scoped({0, true});
        System::LeaderGate                                          leaderGate(
                "数据保留期清理", m_configuration.instanceId, [this] { return m_leaderLease != nullptr && m_leaderLease->isLeader(); },
                [this] { return m_leaderLease != nullptr ? m_leaderLease->fence() : 0; });
        bool               firstRun = true;
        System::LeaderTick retentionTick(leaderGate,
                                         [this, &loop, &firstRun, allTelemetry, allDeliveries]() -> AsynGyanis::Core::Task<>
                                         {
                                             firstRun = false;
                                             // 三张表各自兜底，一张失败不拖住其余清理
                                             try
                                             {
                                                 static_cast<void>(co_await allTelemetry->prune(loop, m_configuration.telemetryRetentionDays));
                                             } catch (const std::exception &failure)
                                             {
                                                 LOG_WARN_FMT("遥测清理失败：{}", failure.what());
                                             }
                                             try
                                             {
                                                 static_cast<void>(co_await m_logRepository->prune(loop, m_configuration.logRetentionDays));
                                             } catch (const std::exception &failure)
                                             {
                                                 LOG_WARN_FMT("日志清理失败：{}", failure.what());
                                             }
                                             try
                                             {
                                                 static_cast<void>(co_await allDeliveries->prune(loop, m_configuration.deliveryRetentionDays));
                                             } catch (const std::exception &failure)
                                             {
                                                 LOG_WARN_FMT("投递记录清理失败：{}", failure.what());
                                             }
                                             try
                                             {
                                                 const std::size_t pruned = co_await m_idempotencyRepository->pruneExpired(loop);
                                                 if (pruned > 0)
                                                 {
                                                     LOG_INFO_FMT("幂等记录清理完成：删除 {} 条到期记录", pruned);
                                                 }
                                             } catch (const std::exception &failure)
                                             {
                                                 LOG_WARN_FMT("幂等记录清理失败：{}", failure.what());
                                             }
                                             LOG_INFO_FMT("数据清理完成（保留天数，0 表示关闭）：遥测 {}、日志 {}、投递记录 {}", m_configuration.telemetryRetentionDays,
                                                          m_configuration.logRetentionDays, m_configuration.deliveryRetentionDays);
                                         });
        while (true)
        {
            // 重启后先尽快清一次，避免积压等到首个六小时周期；尚未当选时保持首轮标记，当选后一分钟内补跑
            co_await ticker.waitFor(firstRun ? std::chrono::minutes{1} : std::chrono::hours{6});
            co_await retentionTick.run();
        }
    }

    AsynGyanis::Core::Task<> Application::runReadinessProbe(AsynGyanis::Core::EventLoop &loop)
    {
        AsynGyanis::Core::Timer ticker(loop);
        bool                    lastReady  = true;
        bool                    firstProbe = true;
        while (true)
        {
            bool ready = false;
            try
            {
                ready = co_await m_cache->probe(loop);
            } catch (const std::exception &failure)
            {
                LOG_WARN_FMT("缓存探针异常：backend={} error={}", m_cache->backendName(), failure.what());
            }
            // 只在状态翻转时记录，Redis 持续不可用期间不刷屏
            if (ready != lastReady)
            {
                if (ready)
                {
                    LOG_INFO_FMT("缓存探针恢复：backend={}", m_cache->backendName());
                } else
                {
                    LOG_WARN_FMT("缓存探针失败：backend={}；/readyz 将返回 503", m_cache->backendName());
                }
                lastReady = ready;
            }
            m_readiness.setCacheReady(ready);
            if (firstProbe)
            {
                firstProbe = false;
                // 首轮探针完成后才置服务就绪。停机请求可能正好落在检查与置位之间，置位后再查一次
                // 并回滚，避免把已经开始排空的进程重新宣布为就绪
                if (!m_stopRequested.load(std::memory_order_acquire))
                {
                    m_readiness.setServiceReady(true);
                    if (m_stopRequested.load(std::memory_order_acquire))
                    {
                        m_readiness.setServiceReady(false);
                    }
                }
            }
            // 每个实例都探测自己的缓存；Redis 启用时这是一次轻量 PING
            co_await ticker.waitFor(std::chrono::seconds{10});
        }
    }

    AsynGyanis::Core::Task<> Application::runFleetLifecycle(AsynGyanis::Core::EventLoop &loop)
    {
        AsynGyanis::Core::Timer ticker(loop);
        const auto              allAlarms   = m_alarmRepository->scoped({0, true});
        const auto              allCommands = m_commandRepository->scoped({0, true});
        const auto              allFirmware = m_firmwareRepository->scoped({0, true});
        System::LeaderGate      sweepGate(
                "离线扫描与告警评估", m_configuration.instanceId, [this] { return m_leaderLease != nullptr && m_leaderLease->isLeader(); },
                [this] { return m_leaderLease != nullptr ? m_leaderLease->fence() : 0; });
        bool firstRun = true;
        // 离线判定与离线告警只由 leader 执行；指标读数便宜，follower 也刷新，面板能看到每台实例的最新视图
        System::LeaderTick fleetTick(
                sweepGate,
                [this, &loop]() -> AsynGyanis::Core::Task<>
                {
                    try
                    {
                        const std::string cutoff = Common::formatDateTimeUtc(std::chrono::system_clock::now() - std::chrono::seconds{m_configuration.heartbeatTimeoutSeconds});
                        co_await m_lifecycle->sweep(loop, cutoff);
                    } catch (const std::exception &failure)
                    {
                        // 单轮失败（数据库抖动、临时锁冲突）不能终止周期任务，否则服务会永久停止判离线
                        LOG_WARN_FMT("离线扫描失败：{}", failure.what());
                    }
                },
                [this, &loop, allAlarms, allCommands, allFirmware]() -> AsynGyanis::Core::Task<>
                {
                    try
                    {
                        const Device::NodeStatusCounts counts = co_await m_fleetNodes->countByStatus(loop);
                        Metrics::BusinessMetrics::instance().setNodeCounts(counts.online, counts.offline);
                        Metrics::BusinessMetrics::instance().setAgentCertificatesExpiring(co_await m_fleetNodes->countExpiringCertificates(loop, 7));
                        Metrics::BusinessMetrics::instance().setAlarmsOpen(co_await allAlarms->openCount(loop));
                        Metrics::BusinessMetrics::instance().setCommandsPending(co_await allCommands->pendingCount(loop));
                        Metrics::BusinessMetrics::instance().setUnsignedFirmware(co_await allFirmware->unsignedCount(loop));
                    } catch (const std::exception &failure)
                    {
                        LOG_WARN_FMT("业务指标刷新失败：{}", failure.what());
                    }
                });
        while (true)
        {
            // 首次延后十秒：给启动中的 Agent 留出重连窗口，避免把正在恢复的节点误判离线
            co_await ticker.waitFor(firstRun ? std::chrono::seconds{10} : std::chrono::seconds{m_configuration.offlineSweepSeconds});
            firstRun = false;
            co_await fleetTick.run();
        }
    }

    AsynGyanis::Core::Task<> Application::publishNodeStatus(const Device::NodeRecord &node)
    {
        const std::string payload = AsynGyanis::Base::ConfigValue::object({{"type", "node.status"}, {"data", Device::nodeToJson(node)}}).dump();
        co_await m_realtimeBus->publish("nodes", node.tenantId, payload);
        co_return;
    }

    AsynGyanis::Core::Task<> Application::publishCommandStatus(const Command::CommandRecord &command)
    {
        const std::string payload =
                AsynGyanis::Base::ConfigValue::object(
                        {{"type", "command.status"},
                         {"data", AsynGyanis::Base::ConfigValue::object(
                                          {{"id", command.id}, {"nodeId", command.nodeId}, {"type", command.type}, {"status", command.status}, {"message", command.message}})}})
                        .dump();
        co_await m_realtimeBus->publish("commands", command.tenantId, payload);
        co_return;
    }

    void Application::registerPublicRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
    {
        // 中间件必须先于业务路由注册，异常兜底才能包住所有处理器
        Web::applySecurityHeaders(router);
        Web::applyRateLimit(router, m_rateLimitBucket);
        Web::registerAuditMiddleware(router, loop, *m_authService, *m_logRepository);
        // 幂等键紧贴业务处理器：审计在外，重放请求也照常留下操作记录；令牌校验在中间件前置，
        // 吊销后的会话既拿不到重放响应也写不出占位行
        Web::applyIdempotency(router, loop, m_idempotencyRepository, *m_authService, m_configuration.reverseProxyPrefix);
        registerHealthRoutes(router);
        registerOpenApiRoute(router, m_webRoot);
        Auth::registerRoutes(router, loop, *m_authService, *m_captchaService, m_configuration.exposeCaptchaCode);
        Auth::registerMfaRoutes(router, loop, *m_authService, *m_mfaService, *m_logRepository);
        if (m_oidcService)
        {
            Auth::registerOidcRoutes(router, loop, *m_oidcService);
        }
        System::SystemDependencies systemDependencies;
        systemDependencies.auth         = m_authService.get();
        systemDependencies.dataScopes   = m_dataScopeService.get();
        systemDependencies.mfa          = m_mfaService.get();
        systemDependencies.users        = m_userRepository.get();
        systemDependencies.roles        = m_roleRepository.get();
        systemDependencies.menus        = m_menuRepository.get();
        systemDependencies.org          = m_orgRepository.get();
        systemDependencies.dictionaries = m_dictionaryRepository.get();
        systemDependencies.tenants      = m_tenantRepository.get();
        System::registerSystemRoutes(router, loop, systemDependencies);
        System::ApiTokenDependencies tokenDependencies;
        tokenDependencies.auth   = m_authService.get();
        tokenDependencies.tokens = m_apiTokenService.get();
        System::registerApiTokenRoutes(router, loop, tokenDependencies);
        System::TenantDependencies tenantDependencies;
        tenantDependencies.auth    = m_authService.get();
        tenantDependencies.tenants = m_tenantRepository.get();
        System::registerTenantRoutes(router, loop, tenantDependencies);
        Notification::NotificationDependencies notificationDependencies;
        notificationDependencies.auth     = m_authService.get();
        notificationDependencies.channels = m_notificationRepository.get();
        notificationDependencies.tenants  = m_tenantRepository.get();
        notificationDependencies.guard    = m_outboundUrlGuard;
        Notification::registerNotificationRoutes(router, loop, notificationDependencies);
        Monitor::MonitorDependencies monitorDependencies;
        monitorDependencies.auth       = m_authService.get();
        monitorDependencies.dataScopes = m_dataScopeService.get();
        monitorDependencies.logs       = m_logRepository.get();
        Monitor::registerMonitorRoutes(router, loop, monitorDependencies);
        Dashboard::DashboardDependencies dashboardDependencies;
        dashboardDependencies.auth       = m_authService.get();
        dashboardDependencies.dataScopes = m_dataScopeService.get();
        dashboardDependencies.logs       = m_logRepository.get();
        dashboardDependencies.roles      = m_roleRepository.get();
        Dashboard::registerDashboardRoutes(router, loop, dashboardDependencies);

        Device::NodeGroupDependencies groupDependencies;
        groupDependencies.auth   = m_authService.get();
        groupDependencies.groups = m_nodeGroupRepository.get();
        Device::registerNodeGroupRoutes(router, loop, groupDependencies);

        Telemetry::TelemetryDependencies telemetryDependencies;
        telemetryDependencies.auth      = m_authService.get();
        telemetryDependencies.nodes     = m_nodeRepository.get();
        telemetryDependencies.telemetry = m_telemetryRepository.get();
        telemetryDependencies.alarms    = m_alarmService.get();
        Telemetry::registerTelemetryBrowserRoutes(router, loop, telemetryDependencies);

        Alarm::AlarmDependencies alarmDependencies;
        alarmDependencies.auth   = m_authService.get();
        alarmDependencies.alarms = m_alarmService.get();
        Alarm::registerAlarmRoutes(router, loop, alarmDependencies);
        Web::registerAlarmEventStream(router, loop, *m_authService, *m_sseBroadcaster);

        const auto                   commandNotifier = [this](const Command::CommandRecord &command) -> AsynGyanis::Core::Task<> { co_await publishCommandStatus(command); };
        Command::CommandDependencies commandDependencies;
        commandDependencies.auth        = m_authService.get();
        commandDependencies.commands    = m_commandRepository.get();
        commandDependencies.realtimeBus = m_realtimeBus.get();
        commandDependencies.notifier    = commandNotifier;
        Command::registerCommandRoutes(router, loop, commandDependencies);

        Firmware::FirmwareDependencies firmwareDependencies;
        firmwareDependencies.auth               = m_authService.get();
        firmwareDependencies.firmware           = m_firmwareRepository.get();
        firmwareDependencies.nodes              = m_nodeRepository.get();
        firmwareDependencies.commands           = m_commandRepository.get();
        firmwareDependencies.realtimeBus        = m_realtimeBus.get();
        firmwareDependencies.notifier           = commandNotifier;
        firmwareDependencies.tenants            = m_tenantRepository.get();
        firmwareDependencies.storageDirectory   = m_configuration.firmwareDirectory;
        firmwareDependencies.maximumUploadBytes = static_cast<std::size_t>(m_configuration.maximumFirmwareBytes);
        firmwareDependencies.signingPolicy      = m_signingPolicy;
        Firmware::registerFirmwareRoutes(router, loop, firmwareDependencies);

        Ops::OpsDependencies opsDependencies;
        opsDependencies.auth       = m_authService.get();
        opsDependencies.repository = m_opsRepository.get();
        opsDependencies.httpPort   = m_configuration.httpPort;
        opsDependencies.httpsPort  = m_configuration.httpsPort;
        opsDependencies.agentPort  = m_configuration.agentPort;
        opsDependencies.opsPort    = m_configuration.opsPort;
        Ops::registerOpsRoutes(router, loop, opsDependencies);
        Device::NodeConfigDependencies nodeConfigDependencies;
        nodeConfigDependencies.auth       = m_authService.get();
        nodeConfigDependencies.configs    = m_nodeConfigRepository.get();
        nodeConfigDependencies.service    = m_nodeConfigService.get();
        nodeConfigDependencies.nodes      = m_nodeRepository.get();
        nodeConfigDependencies.dataScopes = m_dataScopeService.get();
        Device::registerNodeConfigRoutes(router, loop, nodeConfigDependencies);
        const auto accessResolver = [auth = m_authService, &loop](AsynGyanis::Core::EventLoop  &resolverLoop,
                                                                  AsynGyanis::Net::HttpRequest &request) -> AsynGyanis::Core::Task<std::optional<Device::AccessContext>>
        {
            const std::optional<std::string> token = Web::bearerToken(request);
            if (!token.has_value())
            {
                co_return std::nullopt;
            }
            const std::optional<Auth::SessionRecord> session = co_await auth->authenticate(resolverLoop, *token);
            if (!session.has_value())
            {
                co_return std::nullopt;
            }
            co_return Device::AccessContext{session->userId,       session->tenantId, session->platformAdmin, session->username,
                                            session->departmentId, session->roles,    session->permissions};
        };
        Device::registerRoutes(router, loop, *m_nodeService, accessResolver, m_dataScopeService.get(), m_tenantRepository.get(), m_lifecycle.get(), m_realtimeBus.get(),
                               m_nodeConfigRepository.get());
        Device::EnrollmentDependencies enrollmentDependencies;
        enrollmentDependencies.auth        = m_authService.get();
        enrollmentDependencies.enrollments = m_enrollmentService.get();
        enrollmentDependencies.tenants     = m_tenantRepository.get();
        Device::registerEnrollmentRoutes(router, loop, enrollmentDependencies);
        m_browserWebSocket->registerRoute(router, Web::browserTokenValidator(m_authService, loop), m_configuration.allowedOrigins);
        if (m_configuration.reverseProxyEnabled)
        {
            Web::ReverseProxyConfiguration proxyConfiguration;
            proxyConfiguration.prefix                   = m_configuration.reverseProxyPrefix;
            proxyConfiguration.upstream                 = m_configuration.reverseProxyUpstream;
            proxyConfiguration.stripPrefix              = m_configuration.reverseProxyStripPrefix;
            proxyConfiguration.requireSession           = m_configuration.reverseProxyRequireSession;
            proxyConfiguration.permission               = m_configuration.reverseProxyPermission;
            proxyConfiguration.timeoutMilliseconds      = m_configuration.reverseProxyTimeoutMs;
            proxyConfiguration.maximumRequestBodyBytes  = static_cast<std::size_t>(m_configuration.reverseProxyMaximumRequestBytes);
            proxyConfiguration.maximumResponseBodyBytes = static_cast<std::size_t>(m_configuration.reverseProxyMaximumResponseBytes);
            Web::registerReverseProxy(router, loop, std::move(proxyConfiguration), m_authService.get());
        }
        // 站点兜底必须是最后一条通配路由：注册早了会吞掉所有参数化路由（如 /api/firmware/:id/deploy）
        registerSiteFallback(router, m_webRoot);
    }

    int Application::run()
    {
        // 未成功 start 时没有可运行的循环，退出码直接表达「没跑起来」
        if (!m_ioContext)
        {
            return 1;
        }
        m_ioContext->run();
        return 0;
    }

    void Application::requestStop()
    {
        // 停止请求可能来自信号线程与业务线程两处，exchange 保证只停一次
        if (m_stopRequested.exchange(true))
        {
            return;
        }
        // 先让 /readyz 变 503，再停止接新连接并排空在途请求
        m_readiness.setServiceReady(false);
        // 停止请求一到达就释放租约；stop 幂等，析构时再调不会重复清库
        if (m_leaderLease)
        {
            m_leaderLease->stop();
        }
        if (!m_ioContext)
        {
            return;
        }
        AsynGyanis::Core::EventLoop &loop = m_ioContext->threadPool().eventLoop(0);
        loop.scheduler().postRemote(
                [this, &loop]
                {
                    AsynGyanis::Core::Task<> shutdown = [](Application *application, AsynGyanis::Core::EventLoop *eventLoop) -> AsynGyanis::Core::Task<>
                    {
                        constexpr std::chrono::milliseconds drainBudget{5000};
                        if (application->m_httpServer)
                        {
                            co_await application->m_httpServer->drain(drainBudget);
                        }
                        if (application->m_httpsServer)
                        {
                            co_await application->m_httpsServer->drain(drainBudget);
                        }
                        if (application->m_agentServer)
                        {
                            co_await application->m_agentServer->drain(drainBudget);
                        }
                        if (application->m_quicServer)
                        {
                            co_await application->m_quicServer->drain(drainBudget);
                        }
                        if (application->m_opsServer)
                        {
                            co_await application->m_opsServer->drain(drainBudget);
                        }
                        if (application->m_udpTelemetry)
                        {
                            application->m_udpTelemetry->stop();
                        }
                        application->m_stopThread = std::jthread([application] { application->m_ioContext->stop(); });
                        static_cast<void>(eventLoop);
                        co_return;
                    }(this, &loop);
                    m_shutdownTask.emplace(std::move(shutdown));
                    loop.scheduler().schedule(m_shutdownTask->handle());
                });
    }

    std::uint16_t Application::httpPort() const noexcept
    {
        // 端口 0 表示内核分配；服务器尚未进入监听时返回 0，调用方据此判断是否就绪
        return m_httpServer ? m_httpServer->listeningPort() : 0;
    }
} // namespace EdgePulse::App
