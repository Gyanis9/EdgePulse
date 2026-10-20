#include "App/Application.h"
#include "App/ApplicationConfiguration.h"

#include "Base/Config/ConfigManager.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/LoggerConfigLoader.h"
#include "Core/Process/GracefulShutdown.h"

#include <exception>
#include <expected>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

int main(const int argc, char **argv)
{
    std::string configurationPath = "config/dev.yaml";
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--config" && index + 1 < argc)
        {
            configurationPath = argv[++index];
        } else if (argument == "--help")
        {
            std::cout << "用法：edgepulse-server [--config <yaml>]\n";
            return 0;
        } else
        {
            std::cerr << "未知参数：" << argument << '\n';
            return 2;
        }
    }

    try
    {
        // 必须早于日志器与应用线程：日志器的异步写线程同样会继承信号屏蔽字，
        // 晚一步设置就会让 SIGTERM 落到未屏蔽线程上按默认动作直接结束进程
        static_cast<void>(AsynGyanis::Core::GracefulShutdown::blockStopSignals());

        const std::expected<EdgePulse::App::ApplicationConfiguration, EdgePulse::App::ConfigurationError> configuration =
                EdgePulse::App::ApplicationConfiguration::load(configurationPath);
        if (!configuration.has_value())
        {
            std::cerr << configuration.error().message << '\n';
            return 1;
        }

        AsynGyanis::Base::LoggerConfigLoader::loadFromConfig("logging", std::filesystem::current_path());

        std::string                 error;
        EdgePulse::App::Application application(*configuration);
        if (!application.start(error))
        {
            std::cerr << error << '\n';
            return 1;
        }
        LOG_INFO_FMT("EdgePulse 已启动，HTTP 端口 {}", configuration->httpPort);

        AsynGyanis::Core::GracefulShutdown shutdown;
        shutdown.onShutdown([&application] { application.requestStop(); });
        return application.run();
    } catch (const std::exception &failure)
    {
        std::cerr << "启动失败：" << failure.what() << '\n';
        return 1;
    }
}
