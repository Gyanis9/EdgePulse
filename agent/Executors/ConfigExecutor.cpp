#include "Executors/ConfigExecutor.h"

#include "AgentClient.h"
#include "ProcessRunner.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Platform/FileSystem/AtomicFileWriter.h"
#include "Platform/FileSystem/FileSystem.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/EventLoop/EventLoop.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace EdgePulse::Agent
{
    namespace
    {
        constexpr std::chrono::seconds kApplyHookTimeout{30}; ///< 配置应用钩子的时限

        /**
         * @brief 一次配置落盘的结论
         */
        struct ApplyOutcome
        {
            bool        succeeded{false}; ///< 是否成功
            std::string message;          ///< 中文说明
        };

        /**
         * @brief 读取文件全部字节
         * @param path 文件路径
         * @param bytes 成功时写入内容
         * @return 成功为空；失败返回中文原因
         */
        [[nodiscard]] std::expected<void, std::string> readFileBytes(const std::filesystem::path &path, std::string &bytes)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream.is_open())
            {
                return std::unexpected("无法读取原配置文件 " + AsynGyanis::Platform::FileSystem::utf8FromPath(path));
            }
            bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            if (!stream.good() && !stream.eof())
            {
                return std::unexpected("读取原配置文件未完成 " + AsynGyanis::Platform::FileSystem::utf8FromPath(path));
            }
            return {};
        }

        /**
         * @brief 在阻塞执行器上完成备份、原子写入、钩子与回滚
         * @param configuration 已校验的配置
         * @param canonical 规范化后的配置正文
         * @return ApplyOutcome 结论与中文说明
         */
        [[nodiscard]] ApplyOutcome applyConfig(const AgentConfiguration &configuration, const std::string &canonical)
        {
            if (configuration.configPath.empty())
            {
                return ApplyOutcome{false, "未配置 agent.executors.config.path，无法写入配置"};
            }

            const std::filesystem::path target = configuration.configPath;
            std::filesystem::path       backup = target;
            backup += ".backup";

            std::error_code filesystemError;
            const bool      hadOldFile = std::filesystem::exists(target, filesystemError);
            if (filesystemError)
            {
                return ApplyOutcome{false, "无法检查原配置文件：" + filesystemError.message()};
            }

            std::string oldBytes;
            if (hadOldFile)
            {
                const auto read = readFileBytes(target, oldBytes);
                if (!read)
                {
                    return ApplyOutcome{false, read.error()};
                }
                std::string backupError;
                if (!AsynGyanis::Platform::AtomicFileWriter::writeText(backup, oldBytes, std::nullopt, &backupError))
                {
                    return ApplyOutcome{false, "无法备份原配置文件：" + backupError};
                }
            }

            const auto removeBackup = [&backup, hadOldFile]()
            {
                if (!hadOldFile)
                {
                    return;
                }
                std::error_code error;
                std::filesystem::remove(backup, error);
                if (error)
                {
                    LOG_WARN_FMT("删除配置备份 {} 失败：{}", AsynGyanis::Platform::FileSystem::utf8FromPath(backup), error.message());
                }
            };

            std::string writeError;
            if (!AsynGyanis::Platform::AtomicFileWriter::writeText(target, canonical, std::nullopt, &writeError))
            {
                removeBackup();
                return ApplyOutcome{false, "写入配置文件失败：" + writeError};
            }

            if (!configuration.configApplyHook.empty())
            {
                const std::string      pathText = AsynGyanis::Platform::FileSystem::utf8FromPath(target);
                const ProcessRunResult hook     = runProcess(configuration.configApplyHook, {pathText}, kApplyHookTimeout);
                if (!hook.succeeded())
                {
                    std::string restoreError;
                    bool        restoreSucceeded = true;
                    if (hadOldFile)
                    {
                        std::string writeBackError;
                        if (!AsynGyanis::Platform::AtomicFileWriter::writeText(target, oldBytes, std::nullopt, &writeBackError))
                        {
                            restoreSucceeded = false;
                            restoreError     = "；此外恢复旧配置失败：" + writeBackError + "，旧配置备份保留在 " + AsynGyanis::Platform::FileSystem::utf8FromPath(backup);
                            LOG_WARN_FMT("恢复旧配置失败，旧配置备份保留在 {}", AsynGyanis::Platform::FileSystem::utf8FromPath(backup));
                        }
                    } else
                    {
                        std::error_code error;
                        std::filesystem::remove(target, error);
                        if (error)
                        {
                            restoreError = "；此外删除未生效的新配置失败：" + error.message();
                        }
                    }
                    if (restoreSucceeded)
                    {
                        removeBackup();
                    }
                    return ApplyOutcome{false, "配置应用钩子失败：" + hook.failure + restoreError};
                }
            }

            removeBackup();
            return ApplyOutcome{true, "配置版本已写入 " + AsynGyanis::Platform::FileSystem::utf8FromPath(target)};
        }
    } // namespace

    ConfigExecutor::ConfigExecutor(AgentClient &client, AgentConfiguration configuration, const std::int64_t nodeId) :
        m_client(client), m_configuration(std::move(configuration)), m_nodeId(nodeId)
    {
    }

    bool ConfigExecutor::supports(const std::string_view type) const
    {
        return type == "config.apply";
    }

    std::expected<std::string, std::string> ConfigExecutor::canonicalContent(const AsynGyanis::Base::ConfigValue &content) const
    {
        const auto serialized = AsynGyanis::Base::serializeConfigValue(content);
        if (!serialized.has_value())
        {
            return std::unexpected("配置正文无法序列化为 JSON");
        }
        const auto parsed = AsynGyanis::Base::parseConfigValue(*serialized);
        if (!parsed.has_value() || !parsed->is_object())
        {
            return std::unexpected("配置正文不是 JSON 对象");
        }
        const auto canonical = AsynGyanis::Base::serializeConfigValue(*parsed);
        if (!canonical.has_value())
        {
            return std::unexpected("规范化后的配置正文无法序列化为 JSON");
        }
        if (canonical->size() > static_cast<std::size_t>(m_configuration.maximumConfigBytes))
        {
            return std::unexpected("配置正文 " + std::to_string(canonical->size()) + " 字节，超过上限 " + std::to_string(m_configuration.maximumConfigBytes) + " 字节");
        }
        return *canonical;
    }

    AsynGyanis::Core::Task<CommandResult> ConfigExecutor::finish(const std::int64_t version, const AsynGyanis::Base::ConfigValue &content, const bool succeeded,
                                                                 std::string message)
    {
        const std::string status = succeeded ? "applied" : "failed";
        const auto        reported = co_await m_client.reportConfig(m_nodeId, version, content, status, message);
        if (!reported.has_value())
        {
            message += "；配置上报失败：" + reported.error();
            co_return CommandResult{false, "{}", message};
        }
        if (!succeeded)
        {
            co_return CommandResult{false, "{}", message};
        }
        const AsynGyanis::Base::ConfigValue result = AsynGyanis::Base::ConfigValue::object({{"version", version}});
        co_return CommandResult{true, result.dump(), message};
    }

    AsynGyanis::Core::Task<CommandResult> ConfigExecutor::execute(AsynGyanis::Core::EventLoop &loop, const AgentCommand &command)
    {
        const AsynGyanis::Base::ConfigValue emptyContent = AsynGyanis::Base::ConfigValue::object();

        const auto payload = AsynGyanis::Base::parseConfigValue(command.payload);
        if (!payload.has_value() || !payload->is_object())
        {
            co_return CommandResult{false, "{}", "config.apply 的 payload 不是 JSON 对象"};
        }
        const auto versionField = payload->find("version");
        if (versionField == payload->end())
        {
            co_return CommandResult{false, "{}", "config.apply 的 payload 缺少 version"};
        }
        const auto requested = AsynGyanis::Base::configValueAs<std::int64_t>(*versionField);
        if (!requested.has_value() || *requested <= 0)
        {
            co_return CommandResult{false, "{}", "config.apply 的 version 不是正整数"};
        }

        const auto fetched = co_await m_client.fetchConfig(m_nodeId);
        if (!fetched.has_value())
        {
            co_return co_await finish(*requested, emptyContent, false, "拉取配置失败：" + fetched.error());
        }

        const std::int64_t version = fetched->version;
        if (version <= 0)
        {
            co_return co_await finish(*requested, emptyContent, false, "服务端尚未下发配置（版本 0），未写入本地文件");
        }
        if (version < *requested)
        {
            co_return co_await finish(version, fetched->content, false,
                                      "服务端配置版本 " + std::to_string(version) + " 低于命令要求的 " + std::to_string(*requested) + "，未写入本地文件");
        }

        const auto canonical = canonicalContent(fetched->content);
        if (!canonical.has_value())
        {
            co_return co_await finish(version, fetched->content, false, canonical.error());
        }

        ApplyOutcome applied;
        try
        {
            applied = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<ApplyOutcome>(loop, [configuration = m_configuration, content = *canonical]()
                                                                                             { return applyConfig(configuration, content); });
        } catch (const std::exception &failure)
        {
            applied = ApplyOutcome{false, "配置落盘任务提交失败：" + std::string{failure.what()}};
        }

        const auto parsed = AsynGyanis::Base::parseConfigValue(*canonical);
        const AsynGyanis::Base::ConfigValue content = parsed.has_value() && parsed->is_object() ? *parsed : emptyContent;
        co_return co_await finish(version, content, applied.succeeded, applied.message);
    }
} // namespace EdgePulse::Agent
