#include "AgentApplication.h"
#include "AgentConfiguration.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#ifndef EDGEPULSE_AGENT_VERSION
#define EDGEPULSE_AGENT_VERSION "unknown"
#endif

namespace
{
    /**
     * @brief 命令行解析结果
     */
    struct CommandLineOptions
    {
        std::filesystem::path configurationPath{"agent.yaml"}; ///< 配置文件路径
        bool                  console{false};                  ///< 是否前台运行
        bool                  service{false};                  ///< 是否以系统服务方式运行
        bool                  checkConfiguration{false};       ///< 是否只校验配置
        bool                  showVersion{false};              ///< 是否只打印版本
    };

    /**
     * @brief 打印用法
     * @param stream 输出目标
     */
    void printUsage(std::ostream &stream)
    {
        stream << "用法：edgepulse-agent [--config <yaml>] [--console] [--service] [--check-config] [--version]\n";
    }
} // namespace

int main(const int argc, char **argv)
{
    CommandLineOptions options;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--config")
        {
            if (index + 1 >= argc)
            {
                std::cerr << "--config 缺少配置文件路径\n";
                return 2;
            }
            options.configurationPath = argv[++index];
        } else if (argument == "--console")
        {
            options.console = true;
        } else if (argument == "--service")
        {
            options.service = true;
        } else if (argument == "--check-config")
        {
            options.checkConfiguration = true;
        } else if (argument == "--version")
        {
            options.showVersion = true;
        } else if (argument == "--help" || argument == "-h")
        {
            printUsage(std::cout);
            return 0;
        } else
        {
            std::cerr << "未知参数：" << argument << '\n';
            printUsage(std::cerr);
            return 2;
        }
    }

    if (options.showVersion)
    {
        std::cout << "edgepulse-agent " << EDGEPULSE_AGENT_VERSION << '\n';
        return 0;
    }

    if (options.service)
    {
        std::cerr << "服务托管尚未接入：当前版本请使用 --console 前台运行，或在系统服务配置中等待后续版本提供 --service\n";
        return 1;
    }

    try
    {
        const auto configuration = EdgePulse::Agent::AgentConfiguration::load(options.configurationPath);
        if (!configuration.has_value())
        {
            std::cerr << configuration.error() << '\n';
            return 1;
        }

        if (options.checkConfiguration)
        {
            std::cout << "配置校验通过：" << configuration->configurationFile.string() << '\n';
            return 0;
        }

        EdgePulse::Agent::AgentApplication application(*configuration);
        return application.run();
    } catch (const std::exception &failure)
    {
        std::cerr << "启动失败：" << failure.what() << '\n';
        return 1;
    }
}
