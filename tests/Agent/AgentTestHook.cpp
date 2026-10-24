/**
 * @file AgentTestHook.cpp
 * @brief 执行器用例使用的真实可执行钩子
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{
    /**
     * @brief 读取环境变量
     * @param name 变量名
     * @return std::string 变量值；未设置时为空
     */
    std::string environmentValue(const char *name)
    {
        const char *value = std::getenv(name);
        return value == nullptr ? std::string{} : std::string{value};
    }

    /**
     * @brief 把收到的参数逐行追加到标记文件
     * @param markerPath 标记文件路径
     * @param arguments 收到的参数表
     * @return bool 写入成功返回 true
     */
    bool appendMarker(const std::string &markerPath, const std::vector<std::string> &arguments)
    {
        if (markerPath.empty())
        {
            return true;
        }
        std::ofstream marker(markerPath, std::ios::binary | std::ios::app);
        if (!marker.is_open())
        {
            return false;
        }
        for (const std::string &argument: arguments)
        {
            marker << argument << '\n';
        }
        return static_cast<bool>(marker);
    }
} // namespace

int main(const int argc, char **argv)
{
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index)
    {
        arguments.emplace_back(argv[index]);
    }

    if (!appendMarker(environmentValue("EDGEPULSE_AGENT_TEST_HOOK_MARKER"), arguments))
    {
        std::cerr << "无法写入标记文件\n";
        return 2;
    }

    const std::string startupOutput = environmentValue("EDGEPULSE_AGENT_TEST_HOOK_OUTPUT");
    if (!startupOutput.empty())
    {
        std::cout << startupOutput << std::endl;
    }

    if (environmentValue("EDGEPULSE_AGENT_TEST_HOOK_REPLACE_WITH_DIRECTORY") == "1" && !arguments.empty())
    {
        std::error_code error;
        std::filesystem::remove(arguments.front(), error);
        if (error)
        {
            std::cerr << "无法删除钩子收到的路径：" << error.message() << '\n';
            return 2;
        }
        std::filesystem::create_directories(std::filesystem::path{arguments.front()} / "occupied", error);
        if (error)
        {
            std::cerr << "无法用目录占用钩子收到的路径：" << error.message() << '\n';
            return 2;
        }
    }

    int exitCode = 0;
    const std::string configuredExit = environmentValue("EDGEPULSE_AGENT_TEST_HOOK_EXIT");
    if (!configuredExit.empty())
    {
        try
        {
            exitCode = std::stoi(configuredExit);
        } catch (...)
        {
            std::cerr << "退出码不是整数\n";
            return 2;
        }
    }

    if (!arguments.empty() && arguments.front() == "--sleep")
    {
        const long long milliseconds = arguments.size() > 1 ? std::stoll(arguments[1]) : 1000;
        std::this_thread::sleep_for(std::chrono::milliseconds{milliseconds});
        return exitCode;
    }

    if (!arguments.empty() && arguments.front() == "--output")
    {
        const std::size_t bytes = arguments.size() > 1 ? static_cast<std::size_t>(std::stoll(arguments[1])) : 0;
        const std::string chunk(4096, 'x');
        std::size_t       written = 0;
        while (written < bytes)
        {
            const std::size_t remaining = bytes - written;
            const std::size_t take      = remaining < chunk.size() ? remaining : chunk.size();
            std::cout.write(chunk.data(), static_cast<std::streamsize>(take));
            written += take;
        }
        std::cout.flush();
    }

    if (exitCode != 0)
    {
        std::cerr << "钩子按配置返回失败\n";
    }
    return exitCode;
}
