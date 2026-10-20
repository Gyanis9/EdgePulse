#include "App/Application.h"

#include "App/ListenerAssembly.h"
#include "Auth/AuthController.h"
#include "Auth/AuthService.h"
#include "Auth/CaptchaService.h"
#include "Auth/LoginAudit.h"
#include "Auth/SessionStore.h"
#include "Common/Database.h"
#include "System/MigrationRunner.h"
#include "Web/MiddlewareSetup.h"

#include "Core/EventLoop/IoContext.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/HttpServer.h"

#include <exception>
#include <string>

namespace EdgePulse::App
{
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
            m_database       = Common::Database::openSqlite(m_configuration.databasePath);
            m_sessionStore   = std::make_shared<Auth::SessionStore>(m_configuration.sessionTtlSeconds);
            m_captchaService = std::make_shared<Auth::CaptchaService>(Auth::CaptchaOptions{m_configuration.captchaTtlSeconds, m_configuration.captchaRequestsPerMinute});
            m_loginAudit     = std::make_shared<Auth::LoginAudit>(m_database);
            m_authService    = std::make_shared<Auth::AuthService>(m_database, m_sessionStore, m_captchaService, m_loginAudit);
        } catch (const std::exception &failure)
        {
            // 数据库或迁移不可用时没有可服务的业务，直接把原因交给进程入口
            error = std::string{"数据库初始化失败："} + failure.what();
            return false;
        }

        const std::optional<AsynGyanis::Core::InetAddress> address = AsynGyanis::Core::InetAddress::resolve(m_configuration.listenAddress, m_configuration.httpPort);
        // 地址解析失败属于启动期配置错误：写清主机与端口后直接失败，不带着空监听继续
        if (!address.has_value())
        {
            error = "无法解析监听地址：" + m_configuration.listenAddress + ":" + std::to_string(m_configuration.httpPort);
            return false;
        }

        m_httpServer                    = std::make_unique<AsynGyanis::Net::HttpServer>(loop, *address);
        AsynGyanis::Net::Router &router = m_httpServer->router();
        // 中间件必须先于业务路由注册，异常兜底才能包住所有处理器
        Web::applySecurityHeaders(router);
        registerHealthRoutes(router);
        Auth::registerRoutes(router, loop, *m_authService, *m_captchaService, m_configuration.exposeCaptchaCode);
        registerFallbackRoute(router);

        m_acceptTask.emplace(m_httpServer->start());
        // 循环尚未开始运行，本线程可安全地把接受协程放进本地就绪队列
        loop.scheduler().schedule(m_acceptTask->handle());
        return true;
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
