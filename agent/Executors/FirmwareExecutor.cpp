#include "Executors/FirmwareExecutor.h"

#include "AgentClient.h"
#include "AgentState.h"
#include "Executors/DownloadSink.h"
#include "Executors/Sha256Text.h"
#include "ProcessRunner.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Core/Firmware/FirmwareSignatureVerifier.h"
#include "Platform/FileSystem/FileSystem.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/EventLoop/EventLoop.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace EdgePulse::Agent
{
    namespace
    {
        constexpr std::chrono::seconds kInstallHookTimeout{300}; ///< 安装钩子的执行时限
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
                return std::unexpected("firmware.update 的 " + std::string{description} + " 缺失或不是字符串");
            }
            std::string value = iterator->get<std::string>();
            if (value.empty())
            {
                return std::unexpected("firmware.update 的 " + std::string{description} + " 为空");
            }
            return value;
        }

        /**
         * @brief 取对象里的必填正整数字段
         * @param object JSON 对象
         * @param key 字段名
         * @param description 写进中文原因的名称
         * @return 成功返回整数值；缺失、类型不符或非正数时返回中文原因
         */
        [[nodiscard]] std::expected<std::int64_t, std::string> requiredPositiveInteger(const AsynGyanis::Base::ConfigValue &object, const std::string_view key,
                                                                                        const std::string_view description)
        {
            const auto iterator = object.find(std::string{key});
            if (iterator == object.end() || !iterator->is_number_integer())
            {
                return std::unexpected("firmware.update 的 " + std::string{description} + " 缺失或不是整数");
            }
            const std::int64_t value = iterator->get<std::int64_t>();
            if (value <= 0)
            {
                return std::unexpected("firmware.update 的 " + std::string{description} + " 不是正数");
            }
            return value;
        }


        /**
         * @brief 在阻塞执行器上删除暂存文件
         * @param loop 命令所属事件循环
         * @param path 暂存文件路径；空路径直接返回
         */
        AsynGyanis::Core::Task<> removeStagingFile(AsynGyanis::Core::EventLoop &loop, const std::filesystem::path &path)
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
                                                                                      LOG_WARN_FMT("删除固件暂存文件 {} 失败：{}",
                                                                                                   AsynGyanis::Platform::FileSystem::utf8FromPath(path), filesystemError.message());
                                                                                  }
                                                                                  return 0;
                                                                              });
            } catch (const std::exception &failure)
            {
                LOG_WARN_FMT("提交固件暂存文件清理任务失败：{}", failure.what());
            }
            co_return;
        }
    } // namespace

    FirmwareExecutor::FirmwareExecutor(AgentClient &client, AgentState &state, AgentConfiguration configuration, const std::int64_t nodeId) :
        m_client(client), m_state(state), m_configuration(std::move(configuration)), m_nodeId(nodeId)
    {
    }

    bool FirmwareExecutor::supports(const std::string_view type) const
    {
        return type == "firmware.update";
    }

    std::expected<FirmwareExecutor::FirmwarePayload, FirmwareExecutor::PayloadFailure> FirmwareExecutor::parsePayload(const std::string_view payload) const
    {
        const auto parsed = AsynGyanis::Base::parseConfigValue(payload);
        if (!parsed.has_value() || !parsed->is_object())
        {
            return std::unexpected(PayloadFailure{std::nullopt, "firmware.update 的 payload 不是 JSON 对象"});
        }

        FirmwarePayload firmware;
        const auto      firmwareId = requiredPositiveInteger(*parsed, "firmwareId", "firmwareId");
        if (!firmwareId.has_value())
        {
            return std::unexpected(PayloadFailure{std::nullopt, firmwareId.error()});
        }
        firmware.firmwareId = *firmwareId;

        auto version = requiredString(*parsed, "version", "version");
        if (!version.has_value())
        {
            return std::unexpected(PayloadFailure{firmware.firmwareId, version.error()});
        }
        firmware.version = std::move(*version);

        auto sha256 = requiredString(*parsed, "sha256", "sha256");
        if (!sha256.has_value())
        {
            return std::unexpected(PayloadFailure{firmware.firmwareId, sha256.error()});
        }
        if (!isSha256HexText(*sha256))
        {
            return std::unexpected(PayloadFailure{firmware.firmwareId, "firmware.update 的 sha256 不是 64 个十六进制字符"});
        }
        firmware.sha256 = lowercaseHexText(*sha256);

        const auto sizeBytes = requiredPositiveInteger(*parsed, "sizeBytes", "sizeBytes");
        if (!sizeBytes.has_value())
        {
            return std::unexpected(PayloadFailure{firmware.firmwareId, sizeBytes.error()});
        }
        firmware.sizeBytes = static_cast<std::uint64_t>(*sizeBytes);

        auto downloadPath = requiredString(*parsed, "downloadPath", "downloadPath");
        if (!downloadPath.has_value())
        {
            return std::unexpected(PayloadFailure{firmware.firmwareId, downloadPath.error()});
        }
        firmware.downloadPath = std::move(*downloadPath);

        auto signature = requiredString(*parsed, "signature", "signature");
        if (!signature.has_value())
        {
            return std::unexpected(PayloadFailure{firmware.firmwareId, signature.error()});
        }
        firmware.signature = std::move(*signature);

        auto signingKeyId = requiredString(*parsed, "signingKeyId", "signingKeyId");
        if (!signingKeyId.has_value())
        {
            return std::unexpected(PayloadFailure{firmware.firmwareId, signingKeyId.error()});
        }
        firmware.signingKeyId = std::move(*signingKeyId);

        auto algorithm = requiredString(*parsed, "signatureAlgorithm", "signatureAlgorithm");
        if (!algorithm.has_value())
        {
            return std::unexpected(PayloadFailure{firmware.firmwareId, algorithm.error()});
        }
        if (*algorithm != "ed25519-sha256")
        {
            return std::unexpected(PayloadFailure{firmware.firmwareId, "firmware.update 的 signatureAlgorithm 只支持 ed25519-sha256，收到 " + *algorithm});
        }
        return firmware;
    }

    std::filesystem::path FirmwareExecutor::stagingPath(const FirmwarePayload &payload) const
    {
        if (m_configuration.firmwareStagingDirectory.empty())
        {
            return {};
        }
        const std::string name = "firmware-" + std::to_string(payload.firmwareId) + "-" + payload.sha256 + ".bin";
        return m_configuration.firmwareStagingDirectory / name;
    }

    AsynGyanis::Core::Task<> FirmwareExecutor::reportStatus(const std::int64_t firmwareId, const std::int64_t commandId, const std::string_view status, const int progress,
                                                            std::string message)
    {
        const auto reported = co_await m_client.reportFirmwareStatus(firmwareId, m_nodeId, status, progress, message, commandId);
        if (!reported.has_value())
        {
            LOG_WARN_FMT("固件 {} 的状态 {} 上报失败，不影响命令结论：{}", firmwareId, status, reported.error());
        }
        co_return;
    }

    AsynGyanis::Core::Task<CommandResult> FirmwareExecutor::fail(AsynGyanis::Core::EventLoop &loop, const FirmwarePayload &payload, const std::filesystem::path &staging,
                                                                 const std::int64_t commandId, std::string message)
    {
        co_await reportStatus(payload.firmwareId, commandId, "failed", 100, message);
        co_await removeStagingFile(loop, staging);
        co_return CommandResult{false, "{}", std::move(message)};
    }

    AsynGyanis::Core::Task<CommandResult> FirmwareExecutor::execute(AsynGyanis::Core::EventLoop &loop, const AgentCommand &command)
    {
        const auto parsed = parsePayload(command.payload);
        if (!parsed.has_value())
        {
            const PayloadFailure &failure = parsed.error();
            if (failure.firmwareId.has_value())
            {
                co_await reportStatus(*failure.firmwareId, command.id, "failed", 100, failure.message);
            } else
            {
                LOG_WARN_FMT("firmware.update 的 payload 没有可用的 firmwareId，无法上报 failed：{}", failure.message);
            }
            co_return CommandResult{false, "{}", failure.message};
        }
        const FirmwarePayload &payload = *parsed;

        if (m_configuration.firmwareInstallHook.empty())
        {
            co_await reportStatus(payload.firmwareId, command.id, "failed", 100, "未配置 agent.executors.firmware.install_hook，拒绝执行固件更新");
            co_return CommandResult{false, "{}", "未配置 agent.executors.firmware.install_hook，拒绝执行固件更新"};
        }
        const std::filesystem::path staging = stagingPath(payload);
        if (staging.empty())
        {
            co_await reportStatus(payload.firmwareId, command.id, "failed", 100, "未配置 agent.executors.firmware.staging_directory，无法下载固件");
            co_return CommandResult{false, "{}", "未配置 agent.executors.firmware.staging_directory，无法下载固件"};
        }

        // 信任键只加载一次：目录为空或任一公钥不可用时整批拒绝，不进入下载
        using Verifier = EdgePulse::Firmware::FirmwareSignatureVerifier;
        std::expected<std::shared_ptr<const Verifier>, std::string> loadedVerifier;
        try
        {
            loadedVerifier = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::expected<std::shared_ptr<const Verifier>, std::string>>(
                    loop,
                    [directory = m_configuration.firmwareTrustedKeysDirectory]() -> std::expected<std::shared_ptr<const Verifier>, std::string>
                    {
                        auto loaded = Verifier::loadFromDirectory(directory);
                        if (!loaded.has_value())
                        {
                            return std::unexpected("加载固件信任键目录失败：" + loaded.error());
                        }
                        if (loaded->empty())
                        {
                            return std::unexpected("固件信任键目录 " + AsynGyanis::Platform::FileSystem::utf8FromPath(directory) + " 没有可用的 Ed25519 公钥");
                        }
                        return std::shared_ptr<const Verifier>(new Verifier(std::move(*loaded)));
                    });
        } catch (const std::exception &failure)
        {
            loadedVerifier = std::unexpected("提交固件信任键加载任务失败：" + std::string{failure.what()});
        }
        if (!loadedVerifier.has_value())
        {
            co_return co_await fail(loop, payload, {}, command.id, loadedVerifier.error());
        }
        const std::shared_ptr<const Verifier> verifier = *loadedVerifier;

        co_await reportStatus(payload.firmwareId, command.id, "downloading", 0, "开始下载固件 " + payload.version);

        const std::shared_ptr<DownloadSink> sink = std::make_shared<DownloadSink>();
        sink->path                          = staging;
        sink->expectedBytes                 = payload.sizeBytes;
        sink->purpose                       = "固件";

        const auto receiver = [loopPointer = &loop, sink](const AsynGyanis::Net::HttpResponseInfo &head, const std::string_view batch, bool) -> AsynGyanis::Core::Task<bool>
        {
            // 非 200 一律当场收口：错误正文不是固件，既不落盘也不参与摘要
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
                // 批次视图指向客户端内部的接收缓冲，接收口返回前它一直有效；写入在阻塞执行器上完成
                co_return co_await AsynGyanis::Core::AsyncExecutor::shared().submit<bool>(*loopPointer, [sink, batch]() { return sink->append(batch); });
            } catch (const std::exception &failure)
            {
                sink->failed  = true;
                sink->failure = "提交固件写入任务失败：" + std::string{failure.what()};
                co_return false;
            }
        };

        const auto downloaded = co_await m_client.downloadFile(payload.downloadPath, receiver);

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
            LOG_WARN_FMT("提交固件暂存文件关闭任务失败：{}", failure.what());
        }

        if (!downloaded.has_value())
        {
            co_return co_await fail(loop, payload, staging, command.id, "固件下载失败：" + downloaded.error());
        }
        if (downloaded->httpStatus != 200)
        {
            co_return co_await fail(loop, payload, staging, command.id, "固件下载返回 HTTP " + std::to_string(downloaded->httpStatus) + "：" + downloaded->message);
        }

        std::expected<std::pair<std::uint64_t, std::string>, std::string> finalized = std::unexpected("固件下载未完成");
        try
        {
            finalized = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::expected<std::pair<std::uint64_t, std::string>, std::string>>(
                    loop, [sink]() { return sink->finish(); });
        } catch (const std::exception &failure)
        {
            finalized = std::unexpected("提交固件摘要收尾任务失败：" + std::string{failure.what()});
        }
        if (!finalized.has_value())
        {
            co_return co_await fail(loop, payload, staging, command.id, "固件下载未完成：" + finalized.error());
        }

        const auto [receivedBytes, receivedDigest] = *finalized;
        if (receivedBytes != payload.sizeBytes)
        {
            co_return co_await fail(loop, payload, staging, command.id,
                                    "固件大小不符：收到 " + std::to_string(receivedBytes) + " 字节，命令给出 " + std::to_string(payload.sizeBytes) + " 字节");
        }
        if (receivedDigest != payload.sha256)
        {
            co_return co_await fail(loop, payload, staging, command.id, "固件 SHA-256 与命令不符：收到 " + receivedDigest);
        }

        const auto verified = verifier->verify(payload.signingKeyId, receivedDigest, payload.signature);
        if (!verified.has_value())
        {
            co_return co_await fail(loop, payload, staging, command.id, "固件签名校验失败：" + verified.error());
        }

        co_await reportStatus(payload.firmwareId, command.id, "installing", 100, "固件 " + payload.version + " 校验通过，开始安装");

        ProcessRunResult hook;
        try
        {
            hook = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<ProcessRunResult>(
                    loop,
                    [hookPath = m_configuration.firmwareInstallHook, file = AsynGyanis::Platform::FileSystem::utf8FromPath(staging), version = payload.version,
                     sha256 = payload.sha256]() { return runProcess(hookPath, {file, version, sha256}, kInstallHookTimeout); });
        } catch (const std::exception &failure)
        {
            hook = ProcessRunResult{};
            hook.failure = "提交固件安装钩子任务失败：" + std::string{failure.what()};
        }
        if (!hook.succeeded())
        {
            co_return co_await fail(loop, payload, staging, command.id, "固件安装钩子失败：" + hook.failure);
        }

        m_state.set("installed_firmware_version", payload.version);
        co_await reportStatus(payload.firmwareId, command.id, "installed", 100, "固件 " + payload.version + " 已安装");
        co_await removeStagingFile(loop, staging);

        const AsynGyanis::Base::ConfigValue result =
                AsynGyanis::Base::ConfigValue::object({{"firmwareId", payload.firmwareId}, {"version", payload.version}, {"sha256", payload.sha256}, {"installed", true}});
        co_return CommandResult{true, result.dump(), "固件 " + payload.version + " 已安装"};
    }
} // namespace EdgePulse::Agent
