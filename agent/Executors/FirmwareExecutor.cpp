#include "Executors/FirmwareExecutor.h"

#include "AgentClient.h"
#include "AgentState.h"
#include "ProcessRunner.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Core/Firmware/FirmwareSignatureVerifier.h"
#include "Platform/FileSystem/FileSystem.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/EventLoop/EventLoop.h"

#include <openssl/evp.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
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
        constexpr std::size_t          kSha256HexLength = 64;    ///< SHA-256 十六进制文本的长度

        /**
         * @brief 判断文本是否恰好 64 个十六进制字符
         * @param text 待判断文本
         * @return true 表示长度与字符集都符合 SHA-256 十六进制写法
         */
        [[nodiscard]] bool isSha256Hex(const std::string_view text)
        {
            if (text.size() != kSha256HexLength)
            {
                return false;
            }
            for (const char character: text)
            {
                const bool isDigit = character >= '0' && character <= '9';
                const bool isLower = character >= 'a' && character <= 'f';
                const bool isUpper = character >= 'A' && character <= 'F';
                if (!isDigit && !isLower && !isUpper)
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 把十六进制文本折成小写
         * @param text 十六进制文本
         * @return std::string 小写副本
         */
        [[nodiscard]] std::string lowerHex(const std::string_view text)
        {
            std::string lowered;
            lowered.reserve(text.size());
            for (const char character: text)
            {
                lowered.push_back(character >= 'A' && character <= 'F' ? static_cast<char>(character - 'A' + 'a') : character);
            }
            return lowered;
        }

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
         * @brief 计算 SHA-256 的十六进制文本
         * @param digest 32 字节摘要
         * @return std::string 小写十六进制文本
         */
        [[nodiscard]] std::string digestHexText(const std::array<unsigned char, 32> &digest)
        {
            static constexpr char kHexDigits[] = "0123456789abcdef";
            std::string           text;
            text.reserve(64);
            for (const unsigned char byte: digest)
            {
                text.push_back(kHexDigits[byte >> 4]);
                text.push_back(kHexDigits[byte & 0x0F]);
            }
            return text;
        }

        /**
         * @brief 固件下载的落盘与摘要状态
         * @details 每次写入都经阻塞执行器调用，因此文件流与 EVP 上下文只会被一个工作线程依次碰到；
         *          接收口只负责把批次交进来，不在事件循环上做文件读写或摘要计算。
         */
        struct DownloadSink
        {
            std::filesystem::path path;                        ///< 暂存文件路径
            std::ofstream         file;                        ///< 暂存文件，首次写入时打开
            EVP_MD_CTX           *digest{nullptr};             ///< SHA-256 上下文
            std::uint64_t         expectedBytes{0};            ///< 命令给出的固件字节数
            std::uint64_t         receivedBytes{0};            ///< 已写入的字节数
            bool                  opened{false};               ///< 暂存文件是否已打开
            bool                  failed{false};               ///< 是否已判定失败
            std::string           failure;                     ///< 中文失败原因

            DownloadSink() = default;

            ~DownloadSink()
            {
                if (digest != nullptr)
                {
                    EVP_MD_CTX_free(digest);
                }
            }

            DownloadSink(const DownloadSink &)            = delete;
            DownloadSink &operator=(const DownloadSink &) = delete;

            /**
             * @brief 创建暂存目录、打开文件并初始化摘要
             * @return true 表示已就绪
             */
            bool open()
            {
                std::error_code filesystemError;
                std::filesystem::create_directories(path.parent_path(), filesystemError);
                if (filesystemError)
                {
                    failure = "无法创建固件暂存目录 " + AsynGyanis::Platform::FileSystem::utf8FromPath(path.parent_path()) + "：" + filesystemError.message();
                    return false;
                }
                file.open(path, std::ios::binary | std::ios::trunc);
                if (!file.is_open())
                {
                    failure = "无法创建固件暂存文件 " + AsynGyanis::Platform::FileSystem::utf8FromPath(path);
                    return false;
                }
                digest = EVP_MD_CTX_new();
                if (digest == nullptr || EVP_DigestInit_ex(digest, EVP_sha256(), nullptr) != 1)
                {
                    failure = "无法初始化固件 SHA-256 计算";
                    return false;
                }
                opened = true;
                return true;
            }

            /**
             * @brief 关闭暂存文件
             * @return true 表示文件已关闭或本来就没打开
             * @note Windows 上删除仍被打开的文件会失败，任何结局在删除前都要先关闭
             */
            bool close()
            {
                if (!file.is_open())
                {
                    return true;
                }
                file.close();
                if (!file)
                {
                    failed = true;
                    if (failure.empty())
                    {
                        failure = "关闭固件暂存文件失败";
                    }
                    return false;
                }
                return true;
            }

            /**
             * @brief 写入一批正文并更新摘要
             * @param batch 本批字节
             * @return true 表示已完整写入；失败时 failed 与 failure 已置位
             */
            bool append(const std::string_view batch)
            {
                if (failed)
                {
                    return false;
                }
                if (!opened && !open())
                {
                    failed = true;
                    return false;
                }
                if (batch.empty())
                {
                    return true;
                }
                if (receivedBytes + batch.size() > expectedBytes)
                {
                    failed  = true;
                    failure = "固件正文超过命令给出的 " + std::to_string(expectedBytes) + " 字节，已中止下载";
                    return false;
                }
                file.write(batch.data(), static_cast<std::streamsize>(batch.size()));
                if (!file)
                {
                    failed  = true;
                    failure = "写入固件暂存文件失败，可用磁盘空间可能不足";
                    return false;
                }
                if (EVP_DigestUpdate(digest, batch.data(), batch.size()) != 1)
                {
                    failed  = true;
                    failure = "更新固件 SHA-256 失败";
                    return false;
                }
                receivedBytes += batch.size();
                return true;
            }

            /**
             * @brief 收尾文件与摘要
             * @return 成功返回已收字节数与十六进制摘要；失败返回中文原因
             */
            std::expected<std::pair<std::uint64_t, std::string>, std::string> finish()
            {
                const bool closed = close();
                if (failed)
                {
                    return std::unexpected(failure);
                }
                if (!closed)
                {
                    return std::unexpected(failure);
                }
                if (!opened)
                {
                    // 零字节正文不会触发写入，摘要仍要按空文件算完
                    if (!open() || !close())
                    {
                        return std::unexpected(failure);
                    }
                }
                std::array<unsigned char, EVP_MAX_MD_SIZE> digestValue{};
                unsigned int                               digestLength = 0;
                if (EVP_DigestFinal_ex(digest, digestValue.data(), &digestLength) != 1 || digestLength != 32)
                {
                    return std::unexpected("计算固件 SHA-256 失败");
                }
                std::array<unsigned char, 32> sha256{};
                for (std::size_t index = 0; index < sha256.size(); ++index)
                {
                    sha256[index] = digestValue[index];
                }
                return std::pair<std::uint64_t, std::string>{receivedBytes, digestHexText(sha256)};
            }
        };

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
        if (!isSha256Hex(*sha256))
        {
            return std::unexpected(PayloadFailure{firmware.firmwareId, "firmware.update 的 sha256 不是 64 个十六进制字符"});
        }
        firmware.sha256 = lowerHex(*sha256);

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

    AsynGyanis::Core::Task<> FirmwareExecutor::reportStatus(const std::int64_t firmwareId, const std::string_view status, const int progress, std::string message)
    {
        const auto reported = co_await m_client.reportFirmwareStatus(firmwareId, m_nodeId, status, progress, message);
        if (!reported.has_value())
        {
            LOG_WARN_FMT("固件 {} 的状态 {} 上报失败，不影响命令结论：{}", firmwareId, status, reported.error());
        }
        co_return;
    }

    AsynGyanis::Core::Task<CommandResult> FirmwareExecutor::fail(AsynGyanis::Core::EventLoop &loop, const FirmwarePayload &payload, const std::filesystem::path &staging,
                                                                 std::string message)
    {
        co_await reportStatus(payload.firmwareId, "failed", 100, message);
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
                co_await reportStatus(*failure.firmwareId, "failed", 100, failure.message);
            } else
            {
                LOG_WARN_FMT("firmware.update 的 payload 没有可用的 firmwareId，无法上报 failed：{}", failure.message);
            }
            co_return CommandResult{false, "{}", failure.message};
        }
        const FirmwarePayload &payload = *parsed;

        if (m_configuration.firmwareInstallHook.empty())
        {
            co_await reportStatus(payload.firmwareId, "failed", 100, "未配置 agent.executors.firmware.install_hook，拒绝执行固件更新");
            co_return CommandResult{false, "{}", "未配置 agent.executors.firmware.install_hook，拒绝执行固件更新"};
        }
        const std::filesystem::path staging = stagingPath(payload);
        if (staging.empty())
        {
            co_await reportStatus(payload.firmwareId, "failed", 100, "未配置 agent.executors.firmware.staging_directory，无法下载固件");
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
            co_return co_await fail(loop, payload, {}, loadedVerifier.error());
        }
        const std::shared_ptr<const Verifier> verifier = *loadedVerifier;

        co_await reportStatus(payload.firmwareId, "downloading", 0, "开始下载固件 " + payload.version);

        const std::shared_ptr<DownloadSink> sink = std::make_shared<DownloadSink>();
        sink->path                          = staging;
        sink->expectedBytes                 = payload.sizeBytes;

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

        const auto downloaded = co_await m_client.downloadFirmware(payload.downloadPath, receiver);

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
            co_return co_await fail(loop, payload, staging, "固件下载失败：" + downloaded.error());
        }
        if (downloaded->httpStatus != 200)
        {
            co_return co_await fail(loop, payload, staging, "固件下载返回 HTTP " + std::to_string(downloaded->httpStatus) + "：" + downloaded->message);
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
            co_return co_await fail(loop, payload, staging, "固件下载未完成：" + finalized.error());
        }

        const auto [receivedBytes, receivedDigest] = *finalized;
        if (receivedBytes != payload.sizeBytes)
        {
            co_return co_await fail(loop, payload, staging,
                                    "固件大小不符：收到 " + std::to_string(receivedBytes) + " 字节，命令给出 " + std::to_string(payload.sizeBytes) + " 字节");
        }
        if (receivedDigest != payload.sha256)
        {
            co_return co_await fail(loop, payload, staging, "固件 SHA-256 与命令不符：收到 " + receivedDigest);
        }

        const auto verified = verifier->verify(payload.signingKeyId, receivedDigest, payload.signature);
        if (!verified.has_value())
        {
            co_return co_await fail(loop, payload, staging, "固件签名校验失败：" + verified.error());
        }

        co_await reportStatus(payload.firmwareId, "installing", 100, "固件 " + payload.version + " 校验通过，开始安装");

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
            co_return co_await fail(loop, payload, staging, "固件安装钩子失败：" + hook.failure);
        }

        m_state.set("installed_firmware_version", payload.version);
        co_await reportStatus(payload.firmwareId, "installed", 100, "固件 " + payload.version + " 已安装");
        co_await removeStagingFile(loop, staging);

        const AsynGyanis::Base::ConfigValue result =
                AsynGyanis::Base::ConfigValue::object({{"firmwareId", payload.firmwareId}, {"version", payload.version}, {"sha256", payload.sha256}, {"installed", true}});
        co_return CommandResult{true, result.dump(), "固件 " + payload.version + " 已安装"};
    }
} // namespace EdgePulse::Agent
