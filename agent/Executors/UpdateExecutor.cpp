#include "Executors/UpdateExecutor.h"

#include "AgentClient.h"
#include "AgentState.h"
#include "AgentVersion.h"
#include "Executors/DownloadSink.h"
#include "Executors/Sha256Text.h"
#include "ProcessRunner.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Core/Crypto/Digest.h"
#include "Core/Firmware/FirmwareSignatureVerifier.h"
#include "Platform/FileSystem/FileSystem.h"
#include "Platform/Platform.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/EventLoop/EventLoop.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if !ASYN_PLATFORM_WIN32
#include <unistd.h>
#endif

namespace EdgePulse::Agent
{
    namespace
    {
        /**
         * @brief 取对象里的必填非空字符串字段
         * @param object JSON 对象
         * @param key 字段名
         * @param description 写进中文原因的名称
         * @return 成功返回字段文本；缺失、类型不符或为空时返回中文原因
         */
        [[nodiscard]] std::expected<std::string, std::string> requiredString(const AsynGyanis::Base::ConfigValue &object, const std::string_view key,
                                                                             const std::string_view description)
        {
            const auto iterator = object.find(std::string{key});
            if (iterator == object.end() || !iterator->is_string())
            {
                return std::unexpected("agent.update 的 " + std::string{description} + " 缺失或不是字符串");
            }
            std::string value = iterator->get<std::string>();
            if (value.empty())
            {
                return std::unexpected("agent.update 的 " + std::string{description} + " 为空");
            }
            return value;
        }

        /**
         * @brief 把版本净化成可安全拼进文件名的形式
         * @param version 服务端给出的版本
         * @return std::string 只保留字母、数字、点、下划线、连字符与加号的文本
         * @note 版本来自网络，直接拼路径会带上分隔符与 ..；字符集外的字节一律换成下划线
         */
        [[nodiscard]] std::string sanitizedVersion(const std::string_view version)
        {
            std::string sanitized;
            sanitized.reserve(version.size());
            for (const char character: version)
            {
                const bool isDigit = character >= '0' && character <= '9';
                const bool isLower = character >= 'a' && character <= 'z';
                const bool isUpper = character >= 'A' && character <= 'Z';
                const bool isPunctuation = character == '.' || character == '_' || character == '-' || character == '+';
                sanitized.push_back(isDigit || isLower || isUpper || isPunctuation ? character : '_');
            }
            if (sanitized.empty())
            {
                return "unknown";
            }
            return sanitized;
        }

        /**
         * @brief 推导当前安装前缀
         * @return 成功返回 Linux 的 <prefix>（bin 的上级）或 Windows 的可执行文件目录；失败返回中文原因
         */
        [[nodiscard]] std::expected<std::filesystem::path, std::string> installationPrefix()
        {
            const auto executable = currentExecutablePath();
            if (!executable.has_value())
            {
                return std::unexpected(executable.error());
            }
#if ASYN_PLATFORM_WIN32
            const std::filesystem::path prefix = executable->parent_path();
            if (prefix.empty())
            {
                return std::unexpected("无法从可执行文件路径 " + AsynGyanis::Platform::FileSystem::utf8FromPath(*executable) + " 推导安装目录");
            }
            return prefix;
#else
            const std::filesystem::path binaryDirectory = executable->parent_path();
            if (binaryDirectory.filename() != "bin" || !binaryDirectory.has_parent_path())
            {
                return std::unexpected("无法从可执行文件路径 " + AsynGyanis::Platform::FileSystem::utf8FromPath(*executable) +
                                       " 推导安装前缀：二进制应位于 <prefix>/bin/edgepulse-agent");
            }
            return binaryDirectory.parent_path();
#endif
        }

        /**
         * @brief 取当前进程标识，用于拼出唯一的 systemd 单元名
         * @return std::uint64_t 进程标识
         */
        [[nodiscard]] std::uint64_t currentProcessId()
        {
#if ASYN_PLATFORM_WIN32
            return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
            return static_cast<std::uint64_t>(::getpid());
#endif
        }

        /**
         * @brief 确认安装器存在且可执行
         * @param installer 安装脚本路径
         * @return 成功为空；失败返回中文原因
         * @note 在阻塞执行器上调用。脱离启动只能证明启动器进程已经创建，安装器缺失时安装不会发生，
         *       必须在写升级状态之前拒绝
         */
        [[nodiscard]] std::expected<void, std::string> checkInstaller(const std::filesystem::path &installer)
        {
            if (installer.empty())
            {
                return std::unexpected("未配置 agent.update.installer_script，拒绝执行升级");
            }
            const std::string installerText = AsynGyanis::Platform::FileSystem::utf8FromPath(installer);
            std::error_code   filesystemError;
            if (!std::filesystem::is_regular_file(installer, filesystemError))
            {
                if (filesystemError)
                {
                    return std::unexpected("检查升级安装器 " + installerText + " 失败：" + filesystemError.message());
                }
                return std::unexpected("升级安装器 " + installerText + " 不存在或不是普通文件");
            }
#if !ASYN_PLATFORM_WIN32
            if (::access(installer.c_str(), X_OK) != 0)
            {
                return std::unexpected("升级安装器 " + installerText + " 不可执行：" + std::string{std::strerror(errno)});
            }
#endif
            return {};
        }

        /**
         * @brief 一次待执行的安装器启动
         */
        struct LaunchCommand
        {
            std::filesystem::path    executable; ///< 启动器路径
            std::vector<std::string> arguments;  ///< 启动参数
        };

        /**
         * @brief 按平台拼出安装器启动命令
         * @param installer 安装脚本路径
         * @param package 暂存升级包路径
         * @param configuration 已校验的配置
         * @param prefix 安装前缀
         * @return 成功返回启动器与参数；失败返回中文原因
         */
        [[nodiscard]] std::expected<LaunchCommand, std::string> buildLaunchCommand(const std::filesystem::path &installer, const std::filesystem::path &package,
                                                                                   const AgentConfiguration &configuration, const std::filesystem::path &prefix)
        {
            const std::string installerText = AsynGyanis::Platform::FileSystem::utf8FromPath(installer);
            const std::string packageText   = AsynGyanis::Platform::FileSystem::utf8FromPath(package);
            const std::string stateText     = AsynGyanis::Platform::FileSystem::utf8FromPath(configuration.stateDirectory);
            const std::string prefixText    = AsynGyanis::Platform::FileSystem::utf8FromPath(prefix);
            const std::string timeoutText   = std::to_string(configuration.updateHealthTimeoutSeconds);

#if ASYN_PLATFORM_WIN32
            LaunchCommand command;
            command.executable = "powershell.exe";
            command.arguments  = {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", installerText, "-Upgrade", packageText, "-HealthTimeout", timeoutText, "-StateDir", stateText,
                                  "-InstallDir", prefixText};
            return command;
#else
            LaunchCommand command;
            command.executable = "systemd-run";
            // Type=exec 让 systemd-run 等主进程 exec 成功后才报告启动完成；默认的 Type=simple 在单元
            // 入队时就算成功，安装器无法执行时这里仍会拿到成功并留下待上报状态
            command.arguments  = {"--collect",
                                  "--property=Type=exec",
                                  "--unit=edgepulse-agent-update-" + std::to_string(currentProcessId()) + "-" +
                                          std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()),
                                  installerText,
                                  "--upgrade",
                                  packageText,
                                  "--health-timeout",
                                  timeoutText,
                                  "--state-dir",
                                  stateText,
                                  "--prefix",
                                  prefixText};
            return command;
#endif
        }

        /**
         * @brief 判断 manifest 里的某个路径是否是 Agent 二进制
         * @param path 清单里的文件路径
         * @return true 表示 Linux 的 edgepulse-agent 或 Windows 的 edgepulse-agent.exe
         */
        [[nodiscard]] bool isAgentBinaryPath(const std::string_view path)
        {
            return path == "edgepulse-agent" || path == "edgepulse-agent.exe";
        }
    } // namespace

    UpdateExecutor::UpdateExecutor(AgentClient &client, AgentState &state, AgentConfiguration configuration, Launcher launcher, StopCallback requestStop) :
        m_client(client), m_state(state), m_configuration(std::move(configuration)), m_launcher(std::move(launcher)), m_requestStop(std::move(requestStop))
    {
        if (!m_launcher)
        {
            m_launcher = [](const std::filesystem::path &executable, const std::vector<std::string> &arguments) { return launchDetached(executable, arguments); };
        }
    }

    bool UpdateExecutor::supports(const std::string_view type) const
    {
        return type == "agent.update";
    }

    std::expected<UpdateExecutor::UpdatePayload, std::string> UpdateExecutor::parsePayload(const std::string_view payload) const
    {
        const auto parsed = AsynGyanis::Base::parseConfigValue(payload);
        if (!parsed.has_value() || !parsed->is_object())
        {
            return std::unexpected("agent.update 的 payload 不是 JSON 对象");
        }

        UpdatePayload update;
        auto          version = requiredString(*parsed, "version", "version");
        if (!version.has_value())
        {
            return std::unexpected(version.error());
        }
        update.version = std::move(*version);

        auto url = requiredString(*parsed, "url", "url");
        if (!url.has_value())
        {
            return std::unexpected(url.error());
        }
        update.url = std::move(*url);

        auto sha256 = requiredString(*parsed, "sha256", "sha256");
        if (!sha256.has_value())
        {
            return std::unexpected(sha256.error());
        }
        if (!isSha256HexText(*sha256))
        {
            return std::unexpected("agent.update 的 sha256 不是 64 个十六进制字符");
        }
        update.sha256 = lowercaseHexText(*sha256);

        auto signature = requiredString(*parsed, "signature", "signature");
        if (!signature.has_value())
        {
            return std::unexpected(signature.error());
        }
        update.signature = std::move(*signature);

        auto signingKeyId = requiredString(*parsed, "signingKeyId", "signingKeyId");
        if (!signingKeyId.has_value())
        {
            return std::unexpected(signingKeyId.error());
        }
        update.signingKeyId = std::move(*signingKeyId);

        auto algorithm = requiredString(*parsed, "algorithm", "algorithm");
        if (!algorithm.has_value())
        {
            return std::unexpected(algorithm.error());
        }
        if (*algorithm != "ed25519-sha256")
        {
            return std::unexpected("agent.update 的 algorithm 只支持 ed25519-sha256，收到 " + *algorithm);
        }
        update.algorithm = std::move(*algorithm);

        auto manifest = requiredString(*parsed, "manifest", "manifest");
        if (!manifest.has_value())
        {
            return std::unexpected(manifest.error());
        }
        update.manifest = std::move(*manifest);
        return update;
    }

    std::expected<void, std::string> UpdateExecutor::validateManifest(const UpdatePayload &payload) const
    {
        const auto parsed = AsynGyanis::Base::parseConfigValue(payload.manifest);
        if (!parsed.has_value() || !parsed->is_object())
        {
            return std::unexpected("agent.update 的 manifest 不是 JSON 对象");
        }

        auto version = requiredString(*parsed, "version", "manifest 的 version");
        if (!version.has_value())
        {
            return std::unexpected(version.error());
        }
        if (*version != payload.version)
        {
            return std::unexpected("agent.update 的 manifest 版本 " + *version + " 与命令版本 " + payload.version + " 不一致");
        }

        auto platform = requiredString(*parsed, "platform", "manifest 的 platform");
        if (!platform.has_value())
        {
            return std::unexpected(platform.error());
        }
        if (*platform != kAgentPlatform)
        {
            return std::unexpected("agent.update 的 manifest 平台 " + *platform + " 与当前平台 " + std::string{kAgentPlatform} + " 不一致");
        }

        auto signingKeyId = requiredString(*parsed, "signingKeyId", "manifest 的 signingKeyId");
        if (!signingKeyId.has_value())
        {
            return std::unexpected(signingKeyId.error());
        }
        if (*signingKeyId != payload.signingKeyId)
        {
            return std::unexpected("agent.update 的 manifest signingKeyId " + *signingKeyId + " 与命令给出的 " + payload.signingKeyId + " 不一致");
        }

        auto algorithm = requiredString(*parsed, "signatureAlgorithm", "manifest 的 signatureAlgorithm");
        if (!algorithm.has_value())
        {
            return std::unexpected(algorithm.error());
        }
        if (*algorithm != "ed25519-sha256")
        {
            return std::unexpected("agent.update 的 manifest signatureAlgorithm 只支持 ed25519-sha256，收到 " + *algorithm);
        }

        const auto files = parsed->find("files");
        if (files == parsed->end() || !files->is_array())
        {
            return std::unexpected("agent.update 的 manifest 缺少 files 数组");
        }
        bool agentListed = false;
        for (const auto &file: *files)
        {
            if (!file.is_object())
            {
                return std::unexpected("agent.update 的 manifest files 含非对象条目");
            }
            const auto path = file.find("path");
            if (path == file.end() || !path->is_string())
            {
                return std::unexpected("agent.update 的 manifest files 条目缺少字符串 path");
            }
            if (isAgentBinaryPath(path->get<std::string>()))
            {
                agentListed = true;
            }
        }
        if (!agentListed)
        {
            return std::unexpected("agent.update 的 manifest 文件清单没有 edgepulse-agent");
        }
        return {};
    }

    std::filesystem::path UpdateExecutor::stagingPath(const UpdatePayload &payload) const
    {
        if (m_configuration.updateStagingDirectory.empty())
        {
            return {};
        }
        return m_configuration.updateStagingDirectory / ("edgepulse-agent-" + sanitizedVersion(payload.version) + "-" + payload.sha256 + ".pkg");
    }

    AsynGyanis::Core::Task<> UpdateExecutor::removeStagingFile(AsynGyanis::Core::EventLoop &loop, const std::filesystem::path &path)
    {
        if (path.empty())
        {
            co_return;
        }
        try
        {
            co_await AsynGyanis::Core::AsyncExecutor::shared().submit<int>(loop,
                                                                          [path]()
                                                                          {
                                                                              std::error_code filesystemError;
                                                                              std::filesystem::remove(path, filesystemError);
                                                                              if (filesystemError)
                                                                              {
                                                                                  LOG_WARN_FMT("删除升级包暂存文件 {} 失败：{}",
                                                                                               AsynGyanis::Platform::FileSystem::utf8FromPath(path), filesystemError.message());
                                                                              }
                                                                              return 0;
                                                                          });
        } catch (const std::exception &failure)
        {
            LOG_WARN_FMT("提交升级包暂存文件清理任务失败：{}", failure.what());
        }
        co_return;
    }

    AsynGyanis::Core::Task<CommandResult> UpdateExecutor::fail(AsynGyanis::Core::EventLoop &loop, const std::filesystem::path &staging, std::string message)
    {
        co_await removeStagingFile(loop, staging);
        co_return CommandResult{false, "{}", std::move(message)};
    }

    AsynGyanis::Core::Task<std::expected<UpdateExecutor::HealthMarkerSnapshot, std::string>> UpdateExecutor::takeHealthMarker(AsynGyanis::Core::EventLoop &loop,
                                                                                                                              const std::filesystem::path &path)
    {
        std::expected<HealthMarkerSnapshot, std::string> snapshot;
        try
        {
            snapshot = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::expected<HealthMarkerSnapshot, std::string>>(
                    loop,
                    [path]() -> std::expected<HealthMarkerSnapshot, std::string>
                    {
                        const std::string pathText = AsynGyanis::Platform::FileSystem::utf8FromPath(path);
                        HealthMarkerSnapshot result;
                        std::error_code      filesystemError;
                        if (!std::filesystem::exists(path, filesystemError))
                        {
                            if (filesystemError)
                            {
                                return std::unexpected("检查旧健康标记 " + pathText + " 失败：" + filesystemError.message());
                            }
                            return result;
                        }

                        std::ifstream stream(path, std::ios::binary);
                        if (!stream)
                        {
                            return std::unexpected("读取旧健康标记 " + pathText + " 失败");
                        }
                        result.content.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
                        // Windows 上删除仍被打开的文件会失败，先关闭再删
                        const bool readFailed = stream.bad();
                        stream.close();
                        if (readFailed || !stream)
                        {
                            return std::unexpected("读取旧健康标记 " + pathText + " 失败");
                        }
                        result.existed = true;

                        std::filesystem::remove(path, filesystemError);
                        if (filesystemError)
                        {
                            return std::unexpected("删除旧健康标记 " + pathText + " 失败：" + filesystemError.message());
                        }
                        return result;
                    });
        } catch (const std::exception &failure)
        {
            snapshot = std::unexpected("提交旧健康标记清理任务失败：" + std::string{failure.what()});
        }
        co_return snapshot;
    }

    AsynGyanis::Core::Task<> UpdateExecutor::restoreHealthMarker(AsynGyanis::Core::EventLoop &loop, const std::filesystem::path &path, HealthMarkerSnapshot snapshot)
    {
        if (!snapshot.existed)
        {
            co_return;
        }
        std::expected<void, std::string> restored;
        try
        {
            restored = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::expected<void, std::string>>(
                    loop,
                    [path, content = std::move(snapshot.content)]() -> std::expected<void, std::string>
                    {
                        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                        if (!stream)
                        {
                            return std::unexpected("写入旧健康标记 " + AsynGyanis::Platform::FileSystem::utf8FromPath(path) + " 失败");
                        }
                        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
                        stream.close();
                        if (!stream)
                        {
                            return std::unexpected("写入旧健康标记 " + AsynGyanis::Platform::FileSystem::utf8FromPath(path) + " 失败");
                        }
                        return {};
                    });
        } catch (const std::exception &failure)
        {
            restored = std::unexpected("提交健康标记恢复任务失败：" + std::string{failure.what()});
        }
        if (!restored.has_value())
        {
            LOG_ERROR_FMT("恢复旧健康标记 {} 失败：{}", AsynGyanis::Platform::FileSystem::utf8FromPath(path), restored.error());
        }
        co_return;
    }

    AsynGyanis::Core::Task<CommandResult> UpdateExecutor::execute(AsynGyanis::Core::EventLoop &loop, const AgentCommand &command)
    {
        const auto parsed = parsePayload(command.payload);
        if (!parsed.has_value())
        {
            co_return CommandResult{false, "{}", parsed.error()};
        }
        const UpdatePayload payload = *parsed;

        if (m_configuration.updateInstallerScript.empty())
        {
            co_return CommandResult{false, "{}", "未配置 agent.update.installer_script，拒绝执行升级"};
        }
        const std::filesystem::path staging = stagingPath(payload);
        if (staging.empty())
        {
            co_return CommandResult{false, "{}", "未配置 agent.update.staging_directory，无法下载升级包"};
        }

        const auto manifestValid = validateManifest(payload);
        if (!manifestValid.has_value())
        {
            co_return CommandResult{false, "{}", manifestValid.error()};
        }

        using Verifier = EdgePulse::Firmware::FirmwareSignatureVerifier;
        std::expected<std::shared_ptr<const Verifier>, std::string> loadedVerifier;
        try
        {
            loadedVerifier = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::expected<std::shared_ptr<const Verifier>, std::string>>(
                    loop,
                    [directory = m_configuration.packageTrustedKeysDirectory]() -> std::expected<std::shared_ptr<const Verifier>, std::string>
                    {
                        auto loaded = Verifier::loadFromDirectory(directory);
                        if (!loaded.has_value())
                        {
                            return std::unexpected("加载升级包信任键目录失败：" + loaded.error());
                        }
                        if (loaded->empty())
                        {
                            return std::unexpected("升级包信任键目录 " + AsynGyanis::Platform::FileSystem::utf8FromPath(directory) + " 没有可用的 Ed25519 公钥");
                        }
                        return std::shared_ptr<const Verifier>(new Verifier(std::move(*loaded)));
                    });
        } catch (const std::exception &failure)
        {
            loadedVerifier = std::unexpected("提交升级包信任键加载任务失败：" + std::string{failure.what()});
        }
        if (!loadedVerifier.has_value())
        {
            co_return co_await fail(loop, {}, loadedVerifier.error());
        }
        const std::shared_ptr<const Verifier> verifier = *loadedVerifier;

        const auto verified = verifier->verify(payload.signingKeyId, AsynGyanis::Core::Digest::sha256Hex(payload.manifest), payload.signature);
        if (!verified.has_value())
        {
            co_return co_await fail(loop, {}, "升级包签名校验失败：" + verified.error());
        }

        const std::shared_ptr<DownloadSink> sink = std::make_shared<DownloadSink>();
        sink->path                              = staging;
        sink->purpose                           = "升级包";

        const auto receiver = [loopPointer = &loop, sink](const AsynGyanis::Net::HttpResponseInfo &head, const std::string_view batch, bool) -> AsynGyanis::Core::Task<bool>
        {
            // 非 200 一律当场收口：错误正文不是升级包，既不落盘也不参与摘要
            if (head.statusCode != 200)
            {
                co_return false;
            }
            if (batch.empty())
            {
                co_return true;
            }
            try
            {
                co_return co_await AsynGyanis::Core::AsyncExecutor::shared().submit<bool>(*loopPointer, [sink, batch]() { return sink->append(batch); });
            } catch (const std::exception &failure)
            {
                sink->failed  = true;
                sink->failure = "提交升级包写入任务失败：" + std::string{failure.what()};
                co_return false;
            }
        };

        const auto downloaded = co_await m_client.downloadFile(payload.url, receiver);

        // 无论下载如何收场都先关闭文件：Windows 上删除仍被打开的文件会失败
        try
        {
            co_await AsynGyanis::Core::AsyncExecutor::shared().submit<int>(loop,
                                                                          [sink]()
                                                                          {
                                                                              static_cast<void>(sink->close());
                                                                              return 0;
                                                                          });
        } catch (const std::exception &failure)
        {
            LOG_WARN_FMT("提交升级包暂存文件关闭任务失败：{}", failure.what());
        }

        if (!downloaded.has_value())
        {
            co_return co_await fail(loop, staging, "升级包下载失败：" + downloaded.error());
        }
        if (downloaded->httpStatus != 200)
        {
            co_return co_await fail(loop, staging, "升级包下载返回 HTTP " + std::to_string(downloaded->httpStatus) + "：" + downloaded->message);
        }

        std::expected<std::pair<std::uint64_t, std::string>, std::string> finalized = std::unexpected("升级包下载未完成");
        try
        {
            finalized = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::expected<std::pair<std::uint64_t, std::string>, std::string>>(
                    loop, [sink]() { return sink->finish(); });
        } catch (const std::exception &failure)
        {
            finalized = std::unexpected("提交升级包摘要收尾任务失败：" + std::string{failure.what()});
        }
        if (!finalized.has_value())
        {
            co_return co_await fail(loop, staging, "升级包下载未完成：" + finalized.error());
        }
        const std::string receivedDigest = finalized->second;
        if (receivedDigest != payload.sha256)
        {
            co_return co_await fail(loop, staging, "升级包 SHA-256 与命令不符：收到 " + receivedDigest);
        }

        std::expected<void, std::string> installerReady;
        try
        {
            installerReady = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::expected<void, std::string>>(
                    loop, [installer = m_configuration.updateInstallerScript]() { return checkInstaller(installer); });
        } catch (const std::exception &failure)
        {
            installerReady = std::unexpected("提交升级安装器检查任务失败：" + std::string{failure.what()});
        }
        if (!installerReady.has_value())
        {
            co_return co_await fail(loop, staging, installerReady.error());
        }

        const auto executable = currentExecutablePath();
        if (!executable.has_value())
        {
            co_return co_await fail(loop, staging, executable.error());
        }
        const auto prefix = installationPrefix();
        if (!prefix.has_value())
        {
            co_return co_await fail(loop, staging, prefix.error());
        }
        const auto launchCommand = buildLaunchCommand(m_configuration.updateInstallerScript, staging, m_configuration, *prefix);
        if (!launchCommand.has_value())
        {
            co_return co_await fail(loop, staging, launchCommand.error());
        }

        const auto previousState = m_state.updateState();
        if (!previousState.has_value())
        {
            co_return co_await fail(loop, staging, "读取升级状态失败：" + previousState.error());
        }

        // 健康标记属于上一版进程的确认；旧标记不清掉，安装器会把未验证的新版本当成功。
        // 删除前留下快照，启动失败时按原样恢复
        const std::filesystem::path healthMarker   = m_configuration.stateDirectory / "update-health";
        const auto                  healthSnapshot = co_await takeHealthMarker(loop, healthMarker);
        if (!healthSnapshot.has_value())
        {
            co_return co_await fail(loop, staging, healthSnapshot.error());
        }

        const AsynGyanis::Base::ConfigValue pending = AsynGyanis::Base::ConfigValue::object({{"commandId", command.id}, {"version", payload.version}, {"status", "pending"}});
        UpdateStateRecord                 record;
        record.version        = payload.version;
        record.previousBinary = AsynGyanis::Platform::FileSystem::utf8FromPath(*executable);
        record.pendingResult  = pending.dump();
        const auto written    = m_state.setUpdateState(record);
        if (!written.has_value())
        {
            co_await restoreHealthMarker(loop, healthMarker, *healthSnapshot);
            co_return co_await fail(loop, staging, "写入升级状态失败：" + written.error());
        }

        // 启动器可能等待子进程或 systemd 启动结果（Linux 最长 30 秒），不能占住事件循环
        std::expected<void, std::string> launched;
        try
        {
            launched = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::expected<void, std::string>>(
                    loop,
                    [launcher = m_launcher, executable = launchCommand->executable, arguments = launchCommand->arguments]() -> std::expected<void, std::string>
                    {
                        try
                        {
                            return launcher(executable, arguments);
                        } catch (const std::exception &failure)
                        {
                            return std::unexpected("启动升级安装器时发生异常：" + std::string{failure.what()});
                        } catch (...)
                        {
                            return std::unexpected("启动升级安装器时发生未知异常");
                        }
                    });
        } catch (const std::exception &failure)
        {
            launched = std::unexpected("提交安装器启动任务失败：" + std::string{failure.what()});
        }
        if (!launched.has_value())
        {
            const auto reverted = m_state.setUpdateState(*previousState);
            if (!reverted.has_value())
            {
                LOG_ERROR_FMT("启动安装器失败后恢复升级状态失败：{}", reverted.error());
            }
            co_await restoreHealthMarker(loop, healthMarker, *healthSnapshot);
            co_return co_await fail(loop, staging, "启动升级安装器失败：" + launched.error());
        }

        if (m_requestStop)
        {
            try
            {
                m_requestStop();
            } catch (const std::exception &failure)
            {
                LOG_WARN_FMT("启动安装器后请求停止失败：{}；等待进程自然退出", failure.what());
            } catch (...)
            {
                LOG_WARN("启动安装器后请求停止失败：未知异常；等待进程自然退出");
            }
        }

        const AsynGyanis::Base::ConfigValue result =
                AsynGyanis::Base::ConfigValue::object({{"version", payload.version}, {"sha256", payload.sha256}, {"update", "launched"}});
        co_return CommandResult{true, result.dump(), "升级包 " + payload.version + " 已校验，安装器已启动"};
    }
} // namespace EdgePulse::Agent
