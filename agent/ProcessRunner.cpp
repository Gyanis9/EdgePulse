#include "ProcessRunner.h"

#include "Platform/FileSystem/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/System/TextEncoding.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <expected>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !ASYN_PLATFORM_WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace EdgePulse::Agent
{
    namespace
    {
        constexpr std::size_t kMaximumOutputBytes = 64U * 1024U; ///< 标准输出与标准错误合计保留的上限

        /**
         * @brief 追加一段子进程输出，超出上限的部分只计数不保留
         * @param output 已保留的输出
         * @param data 新读到的字节
         * @param length 字节数
         * @param truncated 是否发生过截断
         */
        void appendOutput(std::string &output, const char *data, const std::size_t length, bool &truncated)
        {
            const std::size_t room = output.size() < kMaximumOutputBytes ? kMaximumOutputBytes - output.size() : 0;
            const std::size_t kept = std::min(length, room);
            output.append(data, kept);
            if (kept < length)
            {
                truncated = true;
            }
        }

        /**
         * @brief 取输出末尾的一小段，用于失败原因
         * @param output 合并输出
         * @return std::string 末尾最多 512 字节
         */
        [[nodiscard]] std::string outputTail(const std::string &output)
        {
            constexpr std::size_t kTailBytes = 512;
            if (output.size() <= kTailBytes)
            {
                return output;
            }
            return output.substr(output.size() - kTailBytes);
        }

        /**
         * @brief 按超时状态与退出码生成中文失败原因
         * @param result 已填好退出码与输出
         * @param timeout 等待上限
         * @note 超时与非零退出都附加输出末尾最多 512 字节，便于定位钩子卡在哪里
         */
        void fillFailure(ProcessRunResult &result, const std::chrono::milliseconds timeout)
        {
            if (result.timedOut)
            {
                result.failure = "外部程序在 " + std::to_string(timeout.count()) + " 毫秒内未结束，已被强制终止";
            } else if (result.exitCode == 0)
            {
                return;
            } else
            {
                result.failure = "外部程序以退出码 " + std::to_string(result.exitCode) + " 结束";
            }
            const std::string tail = outputTail(result.output);
            if (!tail.empty())
            {
                result.failure += "：" + tail;
            }
        }

#if ASYN_PLATFORM_WIN32
        /**
         * @brief 按 MSVCRT 解析规则给 Windows 命令行参数加引号
         * @param argument 原始参数
         * @return std::wstring 可直接拼进命令行的形式
         */
        [[nodiscard]] std::wstring quoteWindowsArgument(const std::wstring &argument)
        {
            if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos)
            {
                return argument;
            }

            std::wstring quoted      = L"\"";
            std::size_t  backslashes = 0;
            for (const wchar_t character: argument)
            {
                if (character == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (character == L'"')
                {
                    quoted.append(backslashes * 2 + 1, L'\\');
                    quoted.push_back(L'"');
                    backslashes = 0;
                    continue;
                }
                quoted.append(backslashes, L'\\');
                backslashes = 0;
                quoted.push_back(character);
            }
            quoted.append(backslashes * 2, L'\\');
            quoted.push_back(L'"');
            return quoted;
        }

        /**
         * @brief 把可执行文件与参数拼成 CreateProcessW 的命令行
         * @param executable 可执行文件
         * @param arguments 参数表
         * @return std::wstring 命令行
         */
        [[nodiscard]] std::wstring buildCommandLine(const std::wstring &executable, const std::vector<std::wstring> &arguments)
        {
            std::wstring commandLine = quoteWindowsArgument(executable);
            for (const std::wstring &argument: arguments)
            {
                commandLine.push_back(L' ');
                commandLine += quoteWindowsArgument(argument);
            }
            return commandLine;
        }

        /**
         * @brief 取小写扩展名
         * @param path 文件路径
         * @return std::wstring 带点的扩展名；无扩展名时为空
         */
        [[nodiscard]] std::wstring lowerExtension(const std::filesystem::path &path)
        {
            std::wstring extension = path.extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](const wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
            return extension;
        }

        /**
         * @brief 取 cmd.exe 路径
         * @return std::wstring ComSpec；未设置时返回 cmd.exe
         */
        [[nodiscard]] std::wstring commandInterpreter()
        {
            std::array<wchar_t, 32768> buffer{};
            const DWORD               length = ::GetEnvironmentVariableW(L"ComSpec", buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length > 0 && length < buffer.size())
            {
                return std::wstring{buffer.data(), length};
            }
            return L"cmd.exe";
        }

        /**
         * @brief 把脚本与参数变成 cmd.exe 的 /c 命令串
         * @param script 脚本路径
         * @param arguments 参数表
         * @param command 成功时写入命令串
         * @return true 全部成分都能安全引用
         */
        [[nodiscard]] bool buildCmdCommand(const std::wstring &script, const std::vector<std::wstring> &arguments, std::wstring &command)
        {
            std::vector<std::wstring> parts;
            parts.reserve(arguments.size() + 1);
            parts.push_back(script);
            parts.insert(parts.end(), arguments.begin(), arguments.end());
            for (const std::wstring &part: parts)
            {
                // cmd 没有可靠的转义规则：百分号在引号内仍会被展开成环境变量，
                // 含引号、换行或百分号的成分直接拒绝，不猜
                if (part.find_first_of(L"\"\r\n%") != std::wstring::npos)
                {
                    return false;
                }
                if (!command.empty())
                {
                    command.push_back(L' ');
                }
                command.push_back(L'"');
                command += part;
                command.push_back(L'"');
            }
            return true;
        }

        /**
         * @brief 判断子进程是否是 Windows PowerShell
         * @param program 可执行文件名或路径
         * @return true 表示需要为它重建模块搜索路径
         */
        [[nodiscard]] bool launchesWindowsPowerShell(const std::wstring &program)
        {
            std::wstring name = std::filesystem::path{program}.filename().wstring();
            std::transform(name.begin(), name.end(), name.begin(), [](const wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
            return name == L"powershell.exe";
        }

        /**
         * @brief 复制当前环境块并去掉 PSModulePath
         * @return 双 null 结尾的环境块；读取失败时为空
         */
        [[nodiscard]] std::vector<wchar_t> environmentWithoutModulePath()
        {
            std::vector<wchar_t> block;
            LPWCH                environment = ::GetEnvironmentStringsW();
            if (environment == nullptr)
            {
                return block;
            }
            constexpr wchar_t     kModulePathPrefix[] = L"PSModulePath=";
            constexpr std::size_t kPrefixLength       = sizeof(kModulePathPrefix) / sizeof(wchar_t) - 1;
            for (const wchar_t *entry = environment; *entry != L'\0'; entry += std::wcslen(entry) + 1)
            {
                if (std::wcslen(entry) >= kPrefixLength &&
                    ::CompareStringOrdinal(entry, static_cast<int>(kPrefixLength), kModulePathPrefix, static_cast<int>(kPrefixLength), TRUE) == CSTR_EQUAL)
                {
                    continue;
                }
                block.insert(block.end(), entry, entry + std::wcslen(entry) + 1);
            }
            ::FreeEnvironmentStringsW(environment);
            block.push_back(L'\0');
            return block;
        }

        /**
         * @brief 为子进程准备环境块
         * @param program 可执行文件名或路径
         * @param storage 保存自建环境块，调用期间必须存活
         * @param creationFlags 需要时追加 CREATE_UNICODE_ENVIRONMENT
         * @return 环境块指针；nullptr 表示继承当前进程环境
         * @note 从 PowerShell 7 启动 powershell.exe 时，子进程会继承 pwsh 的 PSModulePath。
         *       Windows PowerShell 5.1 会优先找到面向 .NET Core 的 Microsoft.PowerShell.Utility
         *       并加载失败，Get-FileHash 等命令随之消失；去掉该变量后 5.1 按自己的默认路径重建。
         */
        [[nodiscard]] LPVOID environmentForChild(const std::wstring &program, std::vector<wchar_t> &storage, DWORD &creationFlags)
        {
            if (!launchesWindowsPowerShell(program))
            {
                return nullptr;
            }
            storage = environmentWithoutModulePath();
            if (storage.empty())
            {
                return nullptr;
            }
            creationFlags |= CREATE_UNICODE_ENVIRONMENT;
            return storage.data();
        }
#endif
    } // namespace

    ProcessRunResult runProcess(const std::filesystem::path &executable, const std::vector<std::string> &arguments, const std::chrono::milliseconds timeout)
    {
        ProcessRunResult result;
        if (executable.empty())
        {
            result.failure = "外部程序路径为空，无法执行";
            return result;
        }

        const auto deadline = std::chrono::steady_clock::now() + timeout;

#if ASYN_PLATFORM_WIN32
        const std::wstring executableWide = AsynGyanis::Platform::TextEncoding::toWideString(AsynGyanis::Platform::FileSystem::utf8FromPath(executable));
        if (executableWide.empty())
        {
            result.failure = "外部程序路径无法转换为 Windows 路径：" + AsynGyanis::Platform::FileSystem::utf8FromPath(executable);
            return result;
        }

        std::vector<std::wstring> wideArguments;
        wideArguments.reserve(arguments.size());
        for (const std::string &argument: arguments)
        {
            wideArguments.push_back(AsynGyanis::Platform::TextEncoding::toWideString(argument));
        }

        std::wstring program;
        std::wstring commandLine;
        const std::wstring extension = lowerExtension(executable);
        if (extension == L".cmd" || extension == L".bat")
        {
            std::wstring command;
            if (!buildCmdCommand(executableWide, wideArguments, command))
            {
                result.failure = "配置的脚本路径或参数含有 cmd 无法安全引用的字符（双引号、换行或百分号），已拒绝执行";
                return result;
            }
            program     = commandInterpreter();
            commandLine = quoteWindowsArgument(program) + L" /d /s /c \"" + command + L"\"";
        } else if (extension == L".ps1")
        {
            program = L"powershell.exe";
            std::vector<std::wstring> powerShellArguments{L"-NoProfile", L"-ExecutionPolicy", L"Bypass", L"-File", executableWide};
            powerShellArguments.insert(powerShellArguments.end(), wideArguments.begin(), wideArguments.end());
            commandLine = buildCommandLine(program, powerShellArguments);
        } else
        {
            program     = executableWide;
            commandLine = buildCommandLine(program, wideArguments);
        }

        std::vector<wchar_t> childEnvironment;
        DWORD                creationFlags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW;
        const LPVOID         environment   = environmentForChild(program, childEnvironment, creationFlags);

        SECURITY_ATTRIBUTES security{};
        security.nLength        = sizeof(security);
        security.bInheritHandle = TRUE;

        HANDLE readPipe  = nullptr;
        HANDLE writePipe = nullptr;
        if (::CreatePipe(&readPipe, &writePipe, &security, 0) == 0)
        {
            result.failure = "无法创建子进程输出管道：Windows 错误 " + std::to_string(static_cast<int>(::GetLastError()));
            return result;
        }
        ::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        HANDLE nullInput = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
        if (nullInput == INVALID_HANDLE_VALUE)
        {
            nullInput = nullptr;
        }

        std::vector<HANDLE> inheritedHandles;
        if (nullInput != nullptr)
        {
            inheritedHandles.push_back(nullInput);
        }
        inheritedHandles.push_back(writePipe);

        STARTUPINFOEXW               startupInfo{};
        startupInfo.StartupInfo.cb         = sizeof(startupInfo);
        startupInfo.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
        startupInfo.StartupInfo.hStdInput  = nullInput != nullptr ? nullInput : ::GetStdHandle(STD_INPUT_HANDLE);
        startupInfo.StartupInfo.hStdOutput = writePipe;
        startupInfo.StartupInfo.hStdError  = writePipe;

        SIZE_T attributeListSize = 0;
        static_cast<void>(::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeListSize));
        std::vector<std::uint64_t> attributeListStorage((attributeListSize + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t), 0);
        startupInfo.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeListStorage.data());
        if (attributeListSize == 0 || ::InitializeProcThreadAttributeList(startupInfo.lpAttributeList, 1, 0, &attributeListSize) == 0)
        {
            if (nullInput != nullptr)
            {
                ::CloseHandle(nullInput);
            }
            ::CloseHandle(readPipe);
            ::CloseHandle(writePipe);
            result.failure = "无法准备子进程句柄继承清单：Windows 错误 " + std::to_string(static_cast<int>(::GetLastError()));
            return result;
        }
        const BOOL listed = ::UpdateProcThreadAttribute(startupInfo.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedHandles.data(),
                                                        inheritedHandles.size() * sizeof(HANDLE), nullptr, nullptr);
        if (listed == 0)
        {
            const int failureCode = static_cast<int>(::GetLastError());
            ::DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
            if (nullInput != nullptr)
            {
                ::CloseHandle(nullInput);
            }
            ::CloseHandle(readPipe);
            ::CloseHandle(writePipe);
            result.failure = "无法准备子进程句柄继承清单：Windows 错误 " + std::to_string(failureCode);
            return result;
        }

        PROCESS_INFORMATION processInformation{};
        const BOOL          created = ::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, creationFlags, environment, nullptr,
                                                       &startupInfo.StartupInfo, &processInformation);
        const int           createError = created != 0 ? 0 : static_cast<int>(::GetLastError());
        ::DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
        ::CloseHandle(writePipe);
        if (nullInput != nullptr)
        {
            ::CloseHandle(nullInput);
        }
        if (created == 0)
        {
            ::CloseHandle(readPipe);
            result.failure = "无法启动外部程序 " + AsynGyanis::Platform::FileSystem::utf8FromPath(executable) + "：Windows 错误 " + std::to_string(createError);
            return result;
        }

        result.started = true;
        bool processExited = false;
        for (;;)
        {
            DWORD available = 0;
            if (::PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) != 0 && available > 0)
            {
                char         buffer[8192];
                DWORD        readBytes = 0;
                const DWORD  toRead    = available < sizeof(buffer) ? available : static_cast<DWORD>(sizeof(buffer));
                if (::ReadFile(readPipe, buffer, toRead, &readBytes, nullptr) != 0 && readBytes > 0)
                {
                    appendOutput(result.output, buffer, static_cast<std::size_t>(readBytes), result.outputTruncated);
                    continue;
                }
            }

            if (!processExited)
            {
                const DWORD waited = ::WaitForSingleObject(processInformation.hProcess, 20);
                if (waited == WAIT_OBJECT_0)
                {
                    processExited = true;
                } else if (waited == WAIT_FAILED)
                {
                    result.failure = "等待外部程序结束时失败：Windows 错误 " + std::to_string(static_cast<int>(::GetLastError()));
                    break;
                }
            }

            if (processExited)
            {
                DWORD remaining = 0;
                if (::PeekNamedPipe(readPipe, nullptr, 0, nullptr, &remaining, nullptr) != 0 && remaining > 0)
                {
                    continue;
                }
                break;
            }

            if (std::chrono::steady_clock::now() >= deadline)
            {
                static_cast<void>(::TerminateProcess(processInformation.hProcess, 1));
                static_cast<void>(::WaitForSingleObject(processInformation.hProcess, INFINITE));
                result.timedOut = true;
                processExited   = true;
            }
        }

        DWORD exitCode = 0;
        if (::GetExitCodeProcess(processInformation.hProcess, &exitCode) != 0)
        {
            result.exitCode = static_cast<int>(exitCode);
        } else if (result.failure.empty())
        {
            result.failure = "无法读取外部程序退出码：Windows 错误 " + std::to_string(static_cast<int>(::GetLastError()));
        }

        ::CloseHandle(processInformation.hThread);
        ::CloseHandle(processInformation.hProcess);
        ::CloseHandle(readPipe);
        if (result.failure.empty())
        {
            fillFailure(result, timeout);
        }
        return result;
#else
        int pipeHandles[2] = {-1, -1};
        if (::pipe(pipeHandles) != 0)
        {
            result.failure = "无法创建子进程输出管道：" + std::string{std::strerror(errno)};
            return result;
        }

        const std::string executableText = AsynGyanis::Platform::FileSystem::utf8FromPath(executable);
        const pid_t       childProcessId = ::fork();
        if (childProcessId < 0)
        {
            ::close(pipeHandles[0]);
            ::close(pipeHandles[1]);
            result.failure = "无法创建子进程：" + std::string{std::strerror(errno)};
            return result;
        }
        if (childProcessId == 0)
        {
            ::close(pipeHandles[0]);
            static_cast<void>(::dup2(pipeHandles[1], STDOUT_FILENO));
            static_cast<void>(::dup2(pipeHandles[1], STDERR_FILENO));
            ::close(pipeHandles[1]);

            std::vector<std::string> argumentStrings;
            argumentStrings.reserve(arguments.size() + 1);
            argumentStrings.push_back(executableText);
            argumentStrings.insert(argumentStrings.end(), arguments.begin(), arguments.end());
            std::vector<char *> argumentPointers;
            argumentPointers.reserve(argumentStrings.size() + 1);
            for (std::string &argument: argumentStrings)
            {
                argumentPointers.push_back(argument.data());
            }
            argumentPointers.push_back(nullptr);

            // 全路径用 execv，只有裸名字才交给 execvp 搜索 PATH；两条路径都不经过 shell
            if (executableText.find('/') != std::string::npos)
            {
                ::execv(executableText.c_str(), argumentPointers.data());
            } else
            {
                ::execvp(executableText.c_str(), argumentPointers.data());
            }
            ::_exit(127);
        }

        ::close(pipeHandles[1]);
        const int flags = ::fcntl(pipeHandles[0], F_GETFL, 0);
        if (flags >= 0)
        {
            static_cast<void>(::fcntl(pipeHandles[0], F_SETFL, flags | O_NONBLOCK));
        }

        result.started = true;
        bool        childExited = false;
        int         waitStatus  = 0;
        const auto  drain       = [&result, &pipeHandles]()
        {
            char buffer[8192];
            for (;;)
            {
                const ssize_t readBytes = ::read(pipeHandles[0], buffer, sizeof(buffer));
                if (readBytes > 0)
                {
                    appendOutput(result.output, buffer, static_cast<std::size_t>(readBytes), result.outputTruncated);
                    continue;
                }
                break;
            }
        };

        for (;;)
        {
            drain();
            const pid_t reaped = ::waitpid(childProcessId, &waitStatus, WNOHANG);
            if (reaped == childProcessId)
            {
                childExited = true;
                drain();
                break;
            }
            if (reaped < 0)
            {
                result.failure = "等待外部程序结束时失败：" + std::string{std::strerror(errno)};
                childExited    = true;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                static_cast<void>(::kill(childProcessId, SIGKILL));
                static_cast<void>(::waitpid(childProcessId, &waitStatus, 0));
                result.timedOut = true;
                childExited     = true;
                drain();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        ::close(pipeHandles[0]);

        if (childExited && result.failure.empty())
        {
            if (WIFEXITED(waitStatus))
            {
                result.exitCode = WEXITSTATUS(waitStatus);
            } else if (WIFSIGNALED(waitStatus))
            {
                result.exitCode = 128 + WTERMSIG(waitStatus);
            }
        }
        if (result.failure.empty())
        {
            fillFailure(result, timeout);
        }
        return result;
#endif
    }

    std::expected<std::filesystem::path, std::string> currentExecutablePath()
    {
#if ASYN_PLATFORM_WIN32
        std::vector<wchar_t> buffer(32768);
        for (;;)
        {
            const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
            {
                return std::unexpected("无法读取当前进程的可执行文件路径：Windows 错误 " + std::to_string(static_cast<int>(::GetLastError())));
            }
            if (static_cast<std::size_t>(length) < buffer.size())
            {
                const std::string utf8Path = AsynGyanis::Platform::TextEncoding::toUtf8String(std::wstring{buffer.data(), length});
                if (utf8Path.empty())
                {
                    return std::unexpected("当前进程的可执行文件路径无法转换为 UTF-8");
                }
                return AsynGyanis::Platform::FileSystem::pathFromUtf8(utf8Path);
            }
            buffer.resize(buffer.size() * 2);
        }
#else
        std::vector<char> buffer(4096);
        for (;;)
        {
            const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
            if (length < 0)
            {
                return std::unexpected("无法读取当前进程的可执行文件路径：" + std::string{std::strerror(errno)});
            }
            if (static_cast<std::size_t>(length) < buffer.size())
            {
                return std::filesystem::path{std::string{buffer.data(), static_cast<std::size_t>(length)}};
            }
            buffer.resize(buffer.size() * 2);
        }
#endif
    }

    std::expected<void, std::string> launchDetached(const std::filesystem::path &executable, const std::vector<std::string> &arguments)
    {
        if (executable.empty())
        {
            return std::unexpected("启动器路径为空，无法创建子进程");
        }

#if ASYN_PLATFORM_WIN32
        const std::wstring executableWide = AsynGyanis::Platform::TextEncoding::toWideString(AsynGyanis::Platform::FileSystem::utf8FromPath(executable));
        if (executableWide.empty())
        {
            return std::unexpected("启动器路径无法转换为 Windows 路径：" + AsynGyanis::Platform::FileSystem::utf8FromPath(executable));
        }
        std::vector<std::wstring> wideArguments;
        wideArguments.reserve(arguments.size());
        for (const std::string &argument: arguments)
        {
            wideArguments.push_back(AsynGyanis::Platform::TextEncoding::toWideString(argument));
        }

        std::wstring         commandLine = buildCommandLine(executableWide, wideArguments);
        std::vector<wchar_t> childEnvironment;
        DWORD                creationFlags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
        const LPVOID         environment   = environmentForChild(executableWide, childEnvironment, creationFlags);
        STARTUPINFOW        startupInfo{};
        startupInfo.cb        = sizeof(startupInfo);
        PROCESS_INFORMATION processInformation{};
        const BOOL          created = ::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, creationFlags, environment, nullptr,
                                                       &startupInfo, &processInformation);
        if (created == 0)
        {
            return std::unexpected("无法创建升级安装器进程 " + AsynGyanis::Platform::FileSystem::utf8FromPath(executable) + "：Windows 错误 " +
                                   std::to_string(static_cast<int>(::GetLastError())));
        }
        ::CloseHandle(processInformation.hThread);
        ::CloseHandle(processInformation.hProcess);
        return {};
#else
        const ProcessRunResult started = runProcess(executable, arguments, std::chrono::seconds{30});
        if (!started.started)
        {
            return std::unexpected(started.failure);
        }
        if (started.timedOut)
        {
            return std::unexpected("启动器在 30 秒内没有返回，无法确认安装器已经托管给 systemd");
        }
        if (!started.succeeded())
        {
            return std::unexpected("启动器以退出码 " + std::to_string(started.exitCode) + " 结束" +
                                   (started.output.empty() ? std::string{} : "：" + started.output.substr(0, std::min<std::size_t>(started.output.size(), 512))));
        }
        return {};
#endif
    }
} // namespace EdgePulse::Agent
