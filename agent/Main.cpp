#include "AgentApplication.h"
#include "AgentConfiguration.h"
#include "AgentIdentity.h"
#include "AgentVersion.h"

#ifdef _WIN32
#include "ServiceHost.h"
#endif

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

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
        bool                  printFingerprint{false};         ///< 是否只打印证书指纹
    };

    /**
     * @brief 打印用法
     * @param stream 输出目标
     */
    void printUsage(std::ostream &stream)
    {
        stream << "用法：edgepulse-agent [--config <yaml>] [--console] [--service] [--check-config] [--print-fingerprint] [--version]\n";
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
        } else if (argument == "--print-fingerprint")
        {
            options.printFingerprint = true;
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
        std::cout << "edgepulse-agent " << EdgePulse::Agent::kAgentVersion << '\n';
        return 0;
    }

    if (options.service && options.console)
    {
        std::cerr << "--service 与 --console 不能同时使用：服务模式由 SCM 启动，前台调试请只保留 --console\n";
        return 2;
    }

#ifndef _WIN32
    // 配置校验与指纹打印是跨平台的诊断动作，保留其优先级；单纯的 --service 在非 Windows 构建中不可用
    if (options.service && !options.checkConfiguration && !options.printFingerprint)
    {
        std::cerr << "当前平台不支持 --service：Windows 服务托管只在 Windows 构建中提供，请改用 --console 前台运行\n";
        return 1;
    }
#endif

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

        if (options.printFingerprint)
        {
            const auto identity = EdgePulse::Agent::AgentIdentity::load(*configuration);
            if (!identity.has_value())
            {
                std::cerr << identity.error() << '\n';
                return 1;
            }
            std::cout << identity->fingerprint << '\n';
            return 0;
        }

#ifdef _WIN32
        if (options.service)
        {
            return EdgePulse::Agent::ServiceHost::run(*configuration);
        }
#endif

        EdgePulse::Agent::AgentApplication application(*configuration, options.console);
        return application.run();
    } catch (const std::exception &failure)
    {
        std::cerr << "启动失败：" << failure.what() << '\n';
        return 1;
    }
}
