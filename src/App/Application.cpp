#include "App/Application.h"

#include "App/ListenerAssembly.h"
#include "Base/Config/ConfigValue.h"
#include "Plugins/AuthPlugin/AuthController.h"
#include "Core/Auth/AuthService.h"
#include "Core/Auth/CaptchaService.h"
#include "Core/Auth/LoginAudit.h"
#include "Core/Auth/SessionStore.h"
#include "Base/Api/ApiResponse.h"
#include "Base/Database/Database.h"
#include "Base/Runtime/Environment.h"
#include "Plugins/DevicePlugin/AgentController.h"
#include "Core/Device/AgentRegistry.h"
#include "Plugins/DevicePlugin/DeviceController.h"
#include "Plugins/DevicePlugin/NodeJson.h"
#include "Core/Device/NodeService.h"
#include "Core/System/MigrationRunner.h"
#include "Core/Http/MiddlewareSetup.h"

#include "Core/EventLoop/IoContext.h"
#include "Core/Socket/InetAddress.h"
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

    Application::~Application() = default;

    bool Application::start(std::string &error)
    {
        // P0 单进程单循环：所有监听器共享同一条事件循环，跨线程投递与锁的开销都不发生
        m_ioContext                       = std::make_unique<AsynGyanis::Core::IoContext>(1);
        AsynGyanis::Core::EventLoop &loop = m_ioContext->threadPool().eventLoop(0);

        try
        {
            // 启动期在独立连接上应用迁移；失败原因带文件名与语句序号，直接拒绝启动
            {
                const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = Common::Database::openDirectSqlite(m_configuration.databasePath);
                System::MigrationRunner::apply(*connection, m_configuration.migrationsDirectory);
            }
            m_database         = Common::Database::openSqlite(m_configuration.databasePath);
            m_sessionStore     = std::make_shared<Auth::SessionStore>(m_configuration.sessionTtlSeconds);
            m_captchaService   = std::make_shared<Auth::CaptchaService>(Auth::CaptchaOptions{m_configuration.captchaTtlSeconds, m_configuration.captchaRequestsPerMinute});
            m_loginAudit       = std::make_shared<Auth::LoginAudit>(m_database);
            m_authService      = std::make_shared<Auth::AuthService>(m_database, m_sessionStore, m_captchaService, m_loginAudit);
            m_browserWebSocket = std::make_shared<Web::BrowserWebSocket>(loop);
            m_nodeRepository   = std::make_shared<Device::NodeRepository>(m_database);
            m_nodeService      = std::make_shared<Device::NodeService>(m_nodeRepository);
            Device::AgentRegistry::Options agentOptions;
            if (!m_configuration.agentFingerprint.empty())
            {
                agentOptions.allowedFingerprints.push_back(m_configuration.agentFingerprint);
            }
            agentOptions.publisher = [webSocket = m_browserWebSocket](const Device::NodeRecord &node) -> AsynGyanis::Core::Task<>
            {
                const std::string payload = AsynGyanis::Base::ConfigValue::object({{"type", "node.status"}, {"data", Device::nodeToJson(node)}}).dump();
                co_await webSocket->hub().publish("nodes", payload);
            };
            m_agentRegistry = std::make_shared<Device::AgentRegistry>(m_nodeRepository, std::move(agentOptions));
        } catch (const std::exception &failure)
        {
            // 数据库或迁移不可用时没有可服务的业务，直接把原因交给进程入口
            error = std::string{"数据库初始化失败："} + failure.what();
            return false;
        }

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
            m_httpServer = std::make_unique<AsynGyanis::Net::HttpServer>(loop, *httpAddress);
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
                Device::registerRoutes(m_agentServer->router(), loop, *m_agentRegistry);
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
                opsConfiguration.exposeMetrics  = true;
                opsConfiguration.opsBearerToken = opsToken;
                m_opsServer                     = std::make_unique<AsynGyanis::Net::HttpServer>(loop, *opsAddress);
                AsynGyanis::Net::registerOperationEndpoints(*m_opsServer, opsConfiguration);
                m_opsAccept.emplace(m_opsServer->start());
                loop.scheduler().schedule(m_opsAccept->handle());
            }
        } catch (const std::exception &failure)
        {
            error = std::string{"监听器启动失败："} + failure.what();
            return false;
        }
        return true;
    }

    void Application::registerPublicRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
    {
        // 中间件必须先于业务路由注册，异常兜底才能包住所有处理器
        Web::applySecurityHeaders(router);
        registerHealthRoutes(router);
        Auth::registerRoutes(router, loop, *m_authService, *m_captchaService, m_configuration.exposeCaptchaCode);
        const auto accessResolver = [auth = m_authService](const AsynGyanis::Net::HttpRequest &request) -> std::optional<Device::AccessContext>
        {
            const std::optional<std::string> token = Web::bearerToken(request);
            if (!token.has_value())
            {
                return std::nullopt;
            }
            const std::optional<Auth::SessionRecord> session = auth->authenticate(*token);
            if (!session.has_value())
            {
                return std::nullopt;
            }
            return Device::AccessContext{session->permissions};
        };
        Device::registerRoutes(router, loop, *m_nodeService, accessResolver);
        m_browserWebSocket->registerRoute(
                router, [auth = m_authService](const std::string_view token) { return auth->authenticate(token).has_value(); }, m_configuration.allowedOrigins);
        registerFallbackRoute(router);
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
        if (m_ioContext)
        {
            m_ioContext->stop();
        }
    }

    std::uint16_t Application::httpPort() const noexcept
    {
        // 端口 0 表示内核分配；服务器尚未进入监听时返回 0，调用方据此判断是否就绪
        return m_httpServer ? m_httpServer->listeningPort() : 0;
    }
} // namespace EdgePulse::App
