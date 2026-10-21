#include "App/Application.h"

#include "App/ListenerAssembly.h"
#include "Base/Config/ConfigValue.h"
#include "Plugins/AuthPlugin/AuthController.h"
#include "Plugins/AuthPlugin/OidcController.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/OidcService.h"
#include "Core/Auth/CaptchaService.h"
#include "Core/Auth/LoginAudit.h"
#include "Core/Auth/SessionStore.h"
#include "Core/Auth/DataScopeService.h"
#include "Core/Cache/MemoryKeyValueStore.h"
#include "Core/Cache/RedisKeyValueStore.h"
#include "Core/Http/AuditMiddleware.h"
#include "Base/Api/ApiResponse.h"
#include "Base/Config/ConfigManager.h"
#include "Base/Log/LogMacros.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"
#include "Net/Metrics/MetricsExportConfiguration.h"
#include "Core/Coroutine/Scheduler.h"
#include "Net/Http/ReadinessCheck.h"
#include "Base/Database/Database.h"
#include "Base/Runtime/Environment.h"
#include "Plugins/DevicePlugin/AgentController.h"
#include "Core/Device/AgentRegistry.h"
#include "Plugins/DevicePlugin/DeviceController.h"
#include "Plugins/DevicePlugin/NodeJson.h"
#include "Core/Device/NodeService.h"
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
#include "Core/Command/CommandRepository.h"
#include "Core/Command/AgentChannelRegistry.h"
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
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpServerAssembly.h"
#include "Net/Http/HttpsServer.h"
#include "Net/Http/Middleware.h"
#include "Net/Http/Router.h"
#include "Net/Quic/QuicServer.h"
#include "Core/Http/Authorization.h"
#include "Core/Http/RequestId.h"
#include "Core/Http/WebSocketEndpoints.h"

#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

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
        m_ready.store(false);
        AsynGyanis::Base::ConfigManager::instance().disableHotReload();
        if (m_healthProbe.joinable())
        {
            m_healthProbe.request_stop();
            m_healthProbe.join();
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

        try
        {
            // 启动期在独立连接上应用迁移；失败原因带文件名与语句序号，直接拒绝启动
            {
                Common::DatabaseOptions options;
                options.driver                                                             = m_configuration.databaseDriver;
                options.path                                                               = m_configuration.databasePath;
                options.host                                                               = m_configuration.databaseHost;
                options.port                                                               = m_configuration.databasePort;
                options.userName                                                           = m_configuration.databaseUser;
                options.password                                                           = m_configuration.databasePassword;
                options.database                                                           = m_configuration.databaseName;
                const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = Common::Database::openDirect(options);
                System::MigrationRunner::apply(*connection, m_configuration.migrationsDirectory);
            }
            Common::DatabaseOptions databaseOptions;
            databaseOptions.driver          = m_configuration.databaseDriver;
            databaseOptions.path            = m_configuration.databasePath;
            databaseOptions.host            = m_configuration.databaseHost;
            databaseOptions.port            = m_configuration.databasePort;
            databaseOptions.userName        = m_configuration.databaseUser;
            databaseOptions.password        = m_configuration.databasePassword;
            databaseOptions.database        = m_configuration.databaseName;
            databaseOptions.maximumPoolSize = 8;
            m_database                      = Common::Database::open(databaseOptions);
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
            m_authService      = std::make_shared<Auth::AuthService>(m_database, m_sessionStore, m_captchaService, m_loginAudit, m_cache, m_tenantRepository);
            m_browserWebSocket = std::make_shared<Web::BrowserWebSocket>(loop);
            m_sseBroadcaster   = std::make_shared<Web::SseBroadcaster>();
            Web::RealtimeBusOptions realtimeOptions;
            realtimeOptions.enabled  = m_configuration.redisEnabled;
            realtimeOptions.host     = m_configuration.redisHost;
            realtimeOptions.port     = m_configuration.redisPort;
            realtimeOptions.password = m_configuration.redisPassword;
            realtimeOptions.database = m_configuration.redisDatabase;
            m_realtimeBus            = std::make_unique<Web::RealtimeBus>(loop, *m_browserWebSocket, *m_sseBroadcaster, std::move(realtimeOptions));
            m_realtimeBus->start();
            m_nodeRepository = std::make_shared<Device::NodeRepository>(m_database);
            m_nodeService    = std::make_shared<Device::NodeService>(m_nodeRepository);
            Device::AgentRegistry::Options agentOptions;
            if (!m_configuration.agentFingerprint.empty())
            {
                agentOptions.allowedFingerprints.push_back(m_configuration.agentFingerprint);
            }
            agentOptions.publisher = [bus = m_realtimeBus.get()](const Device::NodeRecord &node) -> AsynGyanis::Core::Task<>
            {
                const std::string payload = AsynGyanis::Base::ConfigValue::object({{"type", "node.status"}, {"data", Device::nodeToJson(node)}}).dump();
                co_await bus->publish("nodes", node.tenantId, payload);
                co_return;
            };
            m_agentRegistry  = std::make_shared<Device::AgentRegistry>(m_nodeRepository, std::move(agentOptions));
            m_userRepository = std::make_shared<System::UserRepository>(m_database);
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
            m_roleRepository         = std::make_shared<System::RoleRepository>(m_database);
            m_menuRepository         = std::make_shared<System::MenuRepository>(m_database);
            m_orgRepository          = std::make_shared<System::OrgRepository>(m_database);
            m_dictionaryRepository   = std::make_shared<System::DictionaryRepository>(m_database);
            m_logRepository          = std::make_shared<System::LogRepository>(m_database);
            m_dataScopeService       = std::make_shared<Auth::DataScopeService>(m_database);
            m_nodeGroupRepository    = std::make_shared<Device::NodeGroupRepository>(m_database);
            m_telemetryRepository    = std::make_shared<Telemetry::TelemetryRepository>(m_database);
            m_alarmRepository        = std::make_shared<Alarm::AlarmRepository>(m_database);
            m_notificationRepository = std::make_shared<Notification::NotificationRepository>(m_database);
            m_notificationService    = std::make_shared<Notification::NotificationService>(m_notificationRepository);
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
            m_firmwareRepository = std::make_shared<Firmware::FirmwareRepository>(m_database);
            m_commandRepository  = std::make_shared<Command::CommandRepository>(m_database);
            m_agentChannels      = std::make_shared<Command::AgentChannelRegistry>();
            m_opsRepository      = std::make_shared<Ops::OpsRepository>(m_database);

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

            m_databaseReady.store(m_database->probe(), std::memory_order_release);
            m_healthProbe = std::jthread(
                    [this](const std::stop_token stopToken)
                    {
                        while (!stopToken.stop_requested())
                        {
                            m_databaseReady.store(m_database->probe(), std::memory_order_release);
                            for (int tick = 0; tick < 20 && !stopToken.stop_requested(); ++tick)
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

        // 固件上传走流式正文，解析器仍按正文总量累计；默认 8 MiB 会把 100 MiB 固件挡在门口
        const auto applyUploadLimits = [](auto &server)
        {
            AsynGyanis::Net::HttpParserLimits limits = server.parserLimits();
            limits.maximumBodySize                   = 256ULL * 1024ULL * 1024ULL;
            server.setParserLimits(limits);
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
            applyUploadLimits(*m_httpServer);
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
                applyUploadLimits(*m_httpsServer);
                m_httpsServer->router().addMiddleware(AsynGyanis::Net::altSvcMiddleware(m_configuration.httpsPort));
                registerPublicRoutes(m_httpsServer->router(), loop);
                m_httpsAccept.emplace(m_httpsServer->start());
                loop.scheduler().schedule(m_httpsAccept->handle());

                // 8443/UDP：独立 QUIC 路由器；/api/probe/h3 只在这里存在，TCP 回落必然 404
                AsynGyanis::Net::QuicServer::Configuration quicConfiguration;
                quicConfiguration.certificateFile = serverCertificate.string();
                quicConfiguration.privateKeyFile  = serverKey.string();
                m_h3Router                        = std::make_unique<AsynGyanis::Net::Router>();
                m_h3Router->get("/api/probe/h3",
                                [](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    Common::writeSuccess(response, AsynGyanis::Base::ConfigValue::object({{"proto", "h3"}}), Web::resolveRequestId(request));
                                    co_return;
                                });
                registerPublicRoutes(*m_h3Router, loop);
                m_quicServer = std::make_unique<AsynGyanis::Net::QuicServer>(loop, quicConfiguration);
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
                if (!m_agentServer->loadClientCertificateAuthority(caCertificate.string()))
                {
                    error = "加载客户端 CA 失败：" + caCertificate.string();
                    return false;
                }
                m_agentServer->setClientCertificateRequired(true);
                Web::applySecurityHeaders(m_agentServer->router());
                Device::registerRoutes(m_agentServer->router(), loop, *m_agentRegistry, *m_tenantRepository);
                Telemetry::TelemetryDependencies telemetryAgentDependencies;
                telemetryAgentDependencies.auth      = m_authService.get();
                telemetryAgentDependencies.nodes     = m_nodeRepository.get();
                telemetryAgentDependencies.telemetry = m_telemetryRepository.get();
                telemetryAgentDependencies.alarms    = m_alarmService.get();
                Telemetry::registerTelemetryAgentRoutes(m_agentServer->router(), loop, telemetryAgentDependencies);
                Command::CommandDependencies commandAgentDependencies;
                commandAgentDependencies.auth     = m_authService.get();
                commandAgentDependencies.commands = m_commandRepository.get();
                commandAgentDependencies.channels = m_agentChannels.get();
                Command::registerCommandAgentRoutes(m_agentServer->router(), loop, commandAgentDependencies);
                Firmware::FirmwareDependencies firmwareAgentDependencies;
                firmwareAgentDependencies.auth               = m_authService.get();
                firmwareAgentDependencies.firmware           = m_firmwareRepository.get();
                firmwareAgentDependencies.nodes              = m_nodeRepository.get();
                firmwareAgentDependencies.commands           = m_commandRepository.get();
                firmwareAgentDependencies.channels           = m_agentChannels.get();
                firmwareAgentDependencies.storageDirectory   = m_configuration.firmwareDirectory;
                firmwareAgentDependencies.maximumUploadBytes = static_cast<std::size_t>(m_configuration.maximumFirmwareBytes);
                Firmware::registerFirmwareAgentRoutes(m_agentServer->router(), loop, firmwareAgentDependencies);
                Web::registerAgentWebSocket(m_agentServer->router(), loop, *m_nodeRepository, *m_commandRepository, *m_agentChannels);
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
                    if (!m_databaseReady.load(std::memory_order_acquire))
                    {
                        return {false, "数据库探针失败"};
                    }
                    if (!m_ready.load(std::memory_order_acquire))
                    {
                        return {false, "服务正在停机或尚未就绪"};
                    }
                    return {true, ""};
                };
                m_opsServer = std::make_unique<AsynGyanis::Net::HttpServer>(loop, *opsAddress);
                AsynGyanis::Net::registerOperationEndpoints(*m_opsServer, opsConfiguration, readiness);
                m_opsAccept.emplace(m_opsServer->start());
                loop.scheduler().schedule(m_opsAccept->handle());
            }
        } catch (const std::exception &failure)
        {
            error = std::string{"监听器启动失败："} + failure.what();
            return false;
        }
        m_commandScheduler = runCommandScheduler(loop);
        loop.scheduler().schedule(m_commandScheduler->handle());
        m_ready.store(true, std::memory_order_release);
        return true;
    }

    AsynGyanis::Core::Task<> Application::runCommandScheduler(AsynGyanis::Core::EventLoop &loop)
    {
        AsynGyanis::Core::Timer                           ticker(loop);
        const std::shared_ptr<Command::CommandRepository> allCommands = m_commandRepository->scoped({0, true});
        while (true)
        {
            co_await ticker.waitFor(std::chrono::seconds{2});
            std::vector<Command::CommandRecord> due;
            try
            {
                due = co_await allCommands->listDue(loop, 100);
            } catch (const std::exception &failure)
            {
                LOG_WARN_FMT("计划命令扫描失败：{}", failure.what());
                continue;
            }
            for (const Command::CommandRecord &command: due)
            {
                if (!m_agentChannels->isConnected(command.nodeId))
                {
                    continue;
                }
                try
                {
                    const AsynGyanis::Base::ConfigValue commandValue = AsynGyanis::Base::ConfigValue::object(
                            {{"id", command.id}, {"nodeId", command.nodeId}, {"type", command.type}, {"payload", command.payload}, {"timeoutSeconds", command.timeoutSeconds}});
                    const std::string message = AsynGyanis::Base::ConfigValue::object({{"type", "command"}, {"command", commandValue}}).dump();
                    if (!co_await m_agentChannels->send(command.nodeId, message))
                    {
                        continue;
                    }
                    if (!co_await allCommands->markDispatched(loop, command.id))
                    {
                        continue;
                    }
                    const auto updated = co_await allCommands->find(loop, command.id);
                    if (!updated.has_value())
                    {
                        continue;
                    }
                    const std::string statusPayload = AsynGyanis::Base::ConfigValue::object({{"type", "command.status"},
                                                                                             {"data", AsynGyanis::Base::ConfigValue::object({{"id", updated->id},
                                                                                                                                             {"nodeId", updated->nodeId},
                                                                                                                                             {"type", updated->type},
                                                                                                                                             {"status", updated->status},
                                                                                                                                             {"message", updated->message}})}})
                                                              .dump();
                    co_await m_realtimeBus->publish("commands", updated->tenantId, statusPayload);
                } catch (const std::exception &failure)
                {
                    LOG_WARN_FMT("计划命令下发失败：command={} error={}", command.id, failure.what());
                }
            }
        }
    }

    void Application::registerPublicRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
    {
        // 中间件必须先于业务路由注册，异常兜底才能包住所有处理器
        Web::applySecurityHeaders(router);
        Web::registerAuditMiddleware(router, loop, *m_authService, *m_logRepository);
        registerHealthRoutes(router);
        Auth::registerRoutes(router, loop, *m_authService, *m_captchaService, m_configuration.exposeCaptchaCode);
        if (m_oidcService)
        {
            Auth::registerOidcRoutes(router, loop, *m_oidcService);
        }
        System::SystemDependencies systemDependencies;
        systemDependencies.auth         = m_authService.get();
        systemDependencies.dataScopes   = m_dataScopeService.get();
        systemDependencies.users        = m_userRepository.get();
        systemDependencies.roles        = m_roleRepository.get();
        systemDependencies.menus        = m_menuRepository.get();
        systemDependencies.org          = m_orgRepository.get();
        systemDependencies.dictionaries = m_dictionaryRepository.get();
        systemDependencies.tenants      = m_tenantRepository.get();
        System::registerSystemRoutes(router, loop, systemDependencies);
        System::TenantDependencies tenantDependencies;
        tenantDependencies.auth    = m_authService.get();
        tenantDependencies.tenants = m_tenantRepository.get();
        System::registerTenantRoutes(router, loop, tenantDependencies);
        Notification::NotificationDependencies notificationDependencies;
        notificationDependencies.auth     = m_authService.get();
        notificationDependencies.channels = m_notificationRepository.get();
        notificationDependencies.tenants  = m_tenantRepository.get();
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

        const auto commandNotifier = [bus = m_realtimeBus.get()](const Command::CommandRecord &command) -> AsynGyanis::Core::Task<>
        {
            const std::string payload =
                    AsynGyanis::Base::ConfigValue::object(
                            {{"type", "command.status"},
                             {"data", AsynGyanis::Base::ConfigValue::object(
                                              {{"id", command.id}, {"nodeId", command.nodeId}, {"type", command.type}, {"status", command.status}, {"message", command.message}})}})
                            .dump();
            co_await bus->publish("commands", command.tenantId, payload);
            co_return;
        };
        Command::CommandDependencies commandDependencies;
        commandDependencies.auth     = m_authService.get();
        commandDependencies.commands = m_commandRepository.get();
        commandDependencies.channels = m_agentChannels.get();
        commandDependencies.notifier = commandNotifier;
        Command::registerCommandRoutes(router, loop, commandDependencies);

        Firmware::FirmwareDependencies firmwareDependencies;
        firmwareDependencies.auth               = m_authService.get();
        firmwareDependencies.firmware           = m_firmwareRepository.get();
        firmwareDependencies.nodes              = m_nodeRepository.get();
        firmwareDependencies.commands           = m_commandRepository.get();
        firmwareDependencies.channels           = m_agentChannels.get();
        firmwareDependencies.notifier           = commandNotifier;
        firmwareDependencies.tenants            = m_tenantRepository.get();
        firmwareDependencies.storageDirectory   = m_configuration.firmwareDirectory;
        firmwareDependencies.maximumUploadBytes = static_cast<std::size_t>(m_configuration.maximumFirmwareBytes);
        Firmware::registerFirmwareRoutes(router, loop, firmwareDependencies);

        Ops::OpsDependencies opsDependencies;
        opsDependencies.auth       = m_authService.get();
        opsDependencies.repository = m_opsRepository.get();
        opsDependencies.httpPort   = m_configuration.httpPort;
        opsDependencies.httpsPort  = m_configuration.httpsPort;
        opsDependencies.agentPort  = m_configuration.agentPort;
        opsDependencies.opsPort    = m_configuration.opsPort;
        Ops::registerOpsRoutes(router, loop, opsDependencies);
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
        Device::registerRoutes(router, loop, *m_nodeService, accessResolver, m_dataScopeService.get(), m_tenantRepository.get());
        m_browserWebSocket->registerRoute(
                router,
                [auth = m_authService, &loop](const std::string_view token) -> AsynGyanis::Core::Task<std::optional<Web::BrowserSubscriptionScope>>
                {
                    const auto session = co_await auth->authenticate(loop, token);
                    if (!session.has_value())
                    {
                        co_return std::nullopt;
                    }
                    co_return Web::BrowserSubscriptionScope{session->tenantId, session->platformAdmin};
                },
                m_configuration.allowedOrigins);
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
        m_ready.store(false, std::memory_order_release);
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
