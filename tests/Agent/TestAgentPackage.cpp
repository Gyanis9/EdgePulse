#include "AgentClient.h"
#include "AgentConfiguration.h"
#include "AgentIdentity.h"
#include "AgentLoop.h"
#include "AgentState.h"
#include "AgentVersion.h"
#include "Executors/UpdateExecutor.h"
#include "FirmwareKeyFixture.h"
#include "ProcessRunner.h"
#include "TestSupport/HttpClientHarness.h"
#include "TestSupport/TemporaryDirectory.h"

#include "Base/Config/ConfigValue.h"
#include "Core/Crypto/Digest.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Firmware/FirmwareSignatureVerifier.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"
#include "Platform/FileSystem/FileSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef EDGEPULSE_SOURCE_DIR
#error "用例需要 EDGEPULSE_SOURCE_DIR 指向仓库根目录"
#endif

namespace EdgePulse::Agent
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;
        using AsynGyanis::Core::EventLoop;
        using AsynGyanis::Core::Task;

        /**
         * @brief 回环服务器侧的升级包正文与请求记录
         */
        struct PackageServerState
        {
            std::mutex       mutex;        ///< 保护正文
            std::string      body;         ///< 下载路由返回的正文
            std::atomic<int> status{200};  ///< 下载路由返回的状态码
            std::atomic<int> downloads{0}; ///< 已收到的下载请求次数
        };

        /**
         * @brief 一次安装器启动的记录
         */
        struct LaunchRecord
        {
            std::mutex               mutex;       ///< 保护字段
            int                      calls{0};    ///< 启动次数
            std::filesystem::path    executable;  ///< 启动器路径
            std::vector<std::string> arguments;   ///< 启动参数
            bool                     fail{false}; ///< 是否模拟启动失败
        };

        /**
         * @brief 一份真实归档及其 SHA-256
         */
        struct PackageArchive
        {
            std::string bytes;  ///< 归档文件的完整字节
            std::string digest; ///< 归档 SHA-256 小写十六进制
        };

        [[nodiscard]] AgentIdentity testIdentity()
        {
            AgentIdentity identity;
            identity.fingerprint = std::string(64, 'b');
            identity.expiresAt   = "2030-01-01T00:00:00Z";
            return identity;
        }

        [[nodiscard]] std::string readTextFile(const std::filesystem::path &path)
        {
            std::ifstream stream(path, std::ios::binary);
            return std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        }

        /**
         * @brief 在临时目录里构造一份带 manifest 与 checksums 的 tar.gz
         * @param root 用例独占的临时目录
         * @param version 归档版本
         * @param platform 归档平台
         * @return 成功返回归档字节与 SHA-256；失败返回中文原因
         * @note 用例验证的是归档摘要签名，归档内容按打包脚本的顶层布局生成，不依赖打包脚本本身
         */
        [[nodiscard]] std::expected<PackageArchive, std::string> buildPackageArchive(const std::filesystem::path &root, const std::string &version,
                                                                                     const std::string &platform = std::string{kAgentPlatform})
        {
            const std::filesystem::path stageName = std::filesystem::path{"package"} / (version + "-" + platform);
            const std::string           binary    = "EDGEPULSE-AGENT-BINARY-" + version;
            const std::string           example   = "base_url: https://127.0.0.1:9443\n";
            try
            {
                std::error_code error;
                std::filesystem::create_directories(root / stageName, error);
                if (error)
                {
                    return std::unexpected("无法创建归档暂存目录：" + error.message());
                }
                std::ofstream binaryStream(root / stageName / "edgepulse-agent", std::ios::binary | std::ios::trunc);
                binaryStream.write(binary.data(), static_cast<std::streamsize>(binary.size()));
                binaryStream.close();
                if (!binaryStream)
                {
                    return std::unexpected("无法写入归档内的 Agent 二进制");
                }
                std::ofstream exampleStream(root / stageName / "agent.yaml.example", std::ios::binary | std::ios::trunc);
                exampleStream.write(example.data(), static_cast<std::streamsize>(example.size()));
                exampleStream.close();
                if (!exampleStream)
                {
                    return std::unexpected("无法写入归档内的配置模板");
                }

                ConfigValue files = ConfigValue::array();
                files.push_back(ConfigValue::object(
                        {{"path", "agent.yaml.example"}, {"size", example.size()}, {"sha256", AsynGyanis::Core::Digest::sha256Hex(example)}}));
                files.push_back(ConfigValue::object({{"path", "edgepulse-agent"}, {"size", binary.size()}, {"sha256", AsynGyanis::Core::Digest::sha256Hex(binary)}}));
                const std::string manifest =
                        ConfigValue::object({{"version", version}, {"platform", platform}, {"files", files}}).dump();
                const std::string checksums = AsynGyanis::Core::Digest::sha256Hex(example) + "  agent.yaml.example\n" +
                                              AsynGyanis::Core::Digest::sha256Hex(binary) + "  edgepulse-agent\n";
                std::ofstream     manifestStream(root / stageName / "manifest.json", std::ios::binary | std::ios::trunc);
                manifestStream.write(manifest.data(), static_cast<std::streamsize>(manifest.size()));
                manifestStream.close();
                if (!manifestStream)
                {
                    return std::unexpected("无法写入归档内的 manifest.json");
                }
                std::ofstream checksumsStream(root / stageName / "checksums.sha256", std::ios::binary | std::ios::trunc);
                checksumsStream.write(checksums.data(), static_cast<std::streamsize>(checksums.size()));
                checksumsStream.close();
                if (!checksumsStream)
                {
                    return std::unexpected("无法写入归档内的 checksums.sha256");
                }
            } catch (const std::exception &failure)
            {
                return std::unexpected("构造归档暂存文件失败：" + std::string{failure.what()});
            }

            const std::filesystem::path archivePath = root / ("edgepulse-agent-" + version + "-" + platform + ".tar.gz");
            const auto                  packed      = runProcess("tar",
                                                       {"-czf",
                                                        AsynGyanis::Platform::FileSystem::utf8FromPath(archivePath),
                                                        "-C",
                                                        AsynGyanis::Platform::FileSystem::utf8FromPath(root / stageName),
                                                        "agent.yaml.example",
                                                        "checksums.sha256",
                                                        "edgepulse-agent",
                                                        "manifest.json"},
                                                       std::chrono::seconds{60});
            if (!packed.succeeded())
            {
                return std::unexpected("无法构造测试归档：" + packed.failure + packed.output);
            }

            PackageArchive archive;
            archive.bytes  = readTextFile(archivePath);
            archive.digest = AsynGyanis::Core::Digest::sha256Hex(archive.bytes);
            if (archive.bytes.empty())
            {
                return std::unexpected("测试归档为空");
            }
            return archive;
        }

        /**
         * @brief 构造 agent.update 的命令正文
         * @param version 目标版本
         * @param digest 归档 SHA-256
         * @param signature Base64 签名（覆盖归档摘要）
         * @param keyId 签名 key id
         * @param algorithm 签名算法
         * @param url 下载地址
         * @return ConfigValue 命令正文对象
         * @note 不写入 manifest 字段；命令里的 manifest 不再参与信任判断
         */
        [[nodiscard]] ConfigValue packagePayload(const std::string &version, const std::string &digest, const std::string &signature, const std::string &keyId = "pkg-2026",
                                                 const std::string &algorithm = "ed25519-sha256", const std::string &url = "/api/agent/update/download")
        {
            return ConfigValue::object({{"version", version},
                                        {"url", url},
                                        {"sha256", digest},
                                        {"signature", signature},
                                        {"signingKeyId", keyId},
                                        {"algorithm", algorithm}});
        }

        /**
         * @brief 装配回环服务器、状态库与更新执行器的用例夹具
         */
        class PackageHarness
        {
        public:
            explicit PackageHarness(std::string servedBody) : m_serverState(std::make_shared<PackageServerState>()), m_launch(std::make_shared<LaunchRecord>())
            {
                {
                    const std::lock_guard lock(m_serverState->mutex);
                    m_serverState->body = std::move(servedBody);
                }
                m_server = std::make_unique<TestSupport::ServerHarness>(
                        [state = m_serverState](AsynGyanis::Net::Router &router, EventLoop &)
                        {
                            router.get("/api/agent/update/download",
                                       [state](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> Task<>
                                       {
                                           state->downloads.fetch_add(1, std::memory_order_release);
                                           const int status = state->status.load(std::memory_order_acquire);
                                           response.setStatus(status);
                                           if (status == 200)
                                           {
                                               const std::lock_guard lock(state->mutex);
                                               response.setBody(state->body);
                                           } else
                                           {
                                               response.setBody(R"({"code":500,"message":"升级包下载失败","data":null,"traceId":"trace-package"})");
                                           }
                                           co_return;
                                       });
                        });

                auto opened = AgentState::open(m_directory.path() / "state" / "agent.db");
                if (!opened.has_value())
                {
                    ADD_FAILURE() << opened.error();
                    return;
                }
                m_state = std::make_unique<AgentState>(std::move(*opened));

                m_configuration.baseUrl                     = "http://127.0.0.1:" + std::to_string(m_server->port());
                m_configuration.allowPlaintextForTests      = true;
                m_configuration.name                        = "edge-01";
                m_configuration.requestTimeoutSeconds       = 10;
                m_configuration.stateDirectory              = m_directory.path() / "state";
                m_configuration.packageTrustedKeysDirectory = m_directory.path() / "package-keys";
                m_configuration.updateStagingDirectory      = m_directory.path() / "update-staging";
                m_configuration.updateHealthTimeoutSeconds  = 30;

                // 执行器启动前会检查安装器存在且可执行，用例统一放一个真实的临时脚本
                const std::filesystem::path installer = m_directory.path() / "install-agent";
                std::ofstream               installerStream(installer, std::ios::binary | std::ios::trunc);
                installerStream << "#!/bin/sh\nexit 0\n";
                installerStream.close();
                if (!installerStream)
                {
                    ADD_FAILURE() << "无法创建升级安装器测试脚本 " << installer;
                }
#if !defined(_WIN32)
                std::error_code error;
                std::filesystem::permissions(installer,
                                             std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
                                                     std::filesystem::perms::others_read | std::filesystem::perms::others_exec,
                                             error);
                if (error)
                {
                    ADD_FAILURE() << "无法设置升级安装器测试脚本的可执行位：" << error.message();
                }
#endif
                m_configuration.updateInstallerScript = installer;
            }

            PackageHarness(const PackageHarness &)            = delete;
            PackageHarness &operator=(const PackageHarness &) = delete;

            [[nodiscard]] std::filesystem::path trustedKeysDirectory() const
            {
                return m_configuration.packageTrustedKeysDirectory;
            }

            [[nodiscard]] std::filesystem::path stagingDirectory() const
            {
                return m_configuration.updateStagingDirectory;
            }

            [[nodiscard]] std::filesystem::path healthMarkerPath() const
            {
                return m_configuration.stateDirectory / "update-health";
            }

            [[nodiscard]] std::filesystem::path installerPath() const
            {
                return m_configuration.updateInstallerScript;
            }

            /**
             * @brief 删除安装器测试脚本，模拟安装器缺失
             */
            void removeInstaller() const
            {
                std::error_code filesystemError;
                std::filesystem::remove(m_configuration.updateInstallerScript, filesystemError);
                if (filesystemError)
                {
                    ADD_FAILURE() << "无法删除升级安装器测试脚本：" << filesystemError.message();
                }
            }

            void setDownloadStatus(const int status) const
            {
                m_serverState->status.store(status, std::memory_order_release);
            }

            void setLauncherFailure() const
            {
                const std::lock_guard lock(m_launch->mutex);
                m_launch->fail = true;
            }

            [[nodiscard]] int downloads() const
            {
                return m_serverState->downloads.load(std::memory_order_acquire);
            }

            [[nodiscard]] int launchCount() const
            {
                const std::lock_guard lock(m_launch->mutex);
                return m_launch->calls;
            }

            [[nodiscard]] std::vector<std::string> launchArguments() const
            {
                const std::lock_guard lock(m_launch->mutex);
                return m_launch->arguments;
            }

            [[nodiscard]] std::filesystem::path launchExecutable() const
            {
                const std::lock_guard lock(m_launch->mutex);
                return m_launch->executable;
            }

            [[nodiscard]] int stopRequests() const
            {
                return m_stopRequests.load(std::memory_order_acquire);
            }

            [[nodiscard]] std::expected<UpdateStateRecord, std::string> updateState() const
            {
                return m_state->updateState();
            }

            [[nodiscard]] bool stagingEmpty() const
            {
                std::error_code filesystemError;
                const bool      exists = std::filesystem::exists(m_configuration.updateStagingDirectory, filesystemError);
                if (filesystemError)
                {
                    return false;
                }
                if (!exists)
                {
                    return true;
                }
                const bool empty = std::filesystem::is_empty(m_configuration.updateStagingDirectory, filesystemError);
                return filesystemError ? false : empty;
            }

            [[nodiscard]] std::vector<std::filesystem::path> stagedFiles() const
            {
                std::vector<std::filesystem::path> files;
                std::error_code                    filesystemError;
                if (!std::filesystem::exists(m_configuration.updateStagingDirectory, filesystemError))
                {
                    return files;
                }
                for (const auto &entry: std::filesystem::directory_iterator(m_configuration.updateStagingDirectory, filesystemError))
                {
                    if (!filesystemError && entry.is_regular_file())
                    {
                        files.push_back(entry.path());
                    }
                }
                return files;
            }

            /**
             * @brief 在独立事件循环上执行一次更新命令
             * @param payload 命令正文 JSON 文本
             * @return 命令结果
             */
            [[nodiscard]] CommandResult execute(const std::string &payload)
            {
                if (m_state == nullptr)
                {
                    ADD_FAILURE() << "状态库未就绪";
                    return CommandResult{false, "{}", "状态库未就绪"};
                }
                AgentLoop   loop;
                AgentClient client(loop.loop(), m_configuration, testIdentity());

                const auto launch = [record = m_launch](const std::filesystem::path &executable, const std::vector<std::string> &arguments) -> std::expected<void, std::string>
                {
                    const std::lock_guard lock(record->mutex);
                    ++record->calls;
                    record->executable = executable;
                    record->arguments  = arguments;
                    if (record->fail)
                    {
                        return std::unexpected("模拟启动失败");
                    }
                    return {};
                };
                const auto stop = [this]() { m_stopRequests.fetch_add(1, std::memory_order_release); };

                UpdateExecutor executor(client, *m_state, m_configuration, launch, stop);
                LoopStopGuard  stopGuard{loop};
                const auto     outcome = loop.run(executor.execute(loop.loop(), AgentCommand{9, "agent.update", payload, 300}), std::chrono::seconds{60}, "升级包执行");
                if (!outcome.has_value())
                {
                    return CommandResult{false, "{}", "升级包执行未在时限内完成"};
                }
                return *outcome;
            }

        private:
            TestSupport::TemporaryDirectory             m_directory;   ///< 用例独占的临时目录
            std::shared_ptr<PackageServerState>         m_serverState; ///< 服务端记录
            std::unique_ptr<TestSupport::ServerHarness> m_server;      ///< 回环服务器
            std::unique_ptr<AgentState>                 m_state;       ///< 真实状态库
            AgentConfiguration                          m_configuration; ///< 执行器配置
            std::shared_ptr<LaunchRecord>               m_launch;      ///< 注入的启动器记录
            std::atomic<int>                            m_stopRequests{0}; ///< 停止回调次数
        };

        /**
         * @brief 断言失败用例没有留下任何升级痕迹
         * @param harness 用例夹具
         * @param expectedLaunches 预期的启动次数
         */
        void expectNoUpgradeLeftBehind(const PackageHarness &harness, const int expectedLaunches = 0)
        {
            EXPECT_EQ(expectedLaunches, harness.launchCount()) << "失败路径不得启动安装器";
            EXPECT_EQ(0, harness.stopRequests()) << "失败路径不得请求停止";
            const auto state = harness.updateState();
            ASSERT_TRUE(state.has_value()) << state.error();
            EXPECT_TRUE(state->version.empty()) << "失败路径不得写入目标版本";
            EXPECT_TRUE(state->pendingResult.empty()) << "失败路径不得留下待上报结果";
            EXPECT_TRUE(state->previousBinary.empty()) << "失败路径不得写入上一版二进制";
            EXPECT_TRUE(harness.stagingEmpty()) << "失败路径必须清理暂存目录";
        }

        /**
         * @brief 判断某个命令行工具能否运行
         * @param executable 工具名
         * @param arguments 探测参数
         * @param timeout 等待时限
         * @return true 表示启动且退出码为 0
         */
        [[nodiscard]] bool toolWorks(const std::string &executable, const std::vector<std::string> &arguments, const std::chrono::milliseconds timeout)
        {
            const ProcessRunResult result = runProcess(std::filesystem::path{executable}, arguments, timeout);
            return result.started && result.exitCode == 0;
        }

        /**
         * @brief 取本平台的 bash 路径
         * @return std::filesystem::path bash 路径
         */
        [[nodiscard]] std::filesystem::path bashPath()
        {
#if defined(_WIN32)
            return std::filesystem::path{"bash"};
#else
            return std::filesystem::path{"/bin/bash"};
#endif
        }

        /**
         * @brief 按 LF 切分文本并去掉 CR
         * @param text 待切分文本
         * @return std::vector<std::string> 非空行
         */
        [[nodiscard]] std::vector<std::string> splitLines(const std::string &text)
        {
            std::vector<std::string> lines;
            std::size_t              begin = 0;
            while (begin < text.size())
            {
                const std::size_t end  = text.find('\n', begin);
                std::string       line = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                if (!line.empty())
                {
                    lines.push_back(std::move(line));
                }
                if (end == std::string::npos)
                {
                    break;
                }
                begin = end + 1;
            }
            return lines;
        }

        /**
         * @brief 在输出目录里找指定扩展名的归档
         * @param output 输出目录
         * @param extension 文件扩展名，如 .gz 或 .zip
         * @return std::vector<std::filesystem::path> 归档列表
         */
        [[nodiscard]] std::vector<std::filesystem::path> findArchives(const std::filesystem::path &output, const std::string &extension)
        {
            std::vector<std::filesystem::path> archives;
            std::error_code                    error;
            for (const auto &entry: std::filesystem::directory_iterator(output, error))
            {
                if (!error && entry.is_regular_file() && entry.path().extension() == extension)
                {
                    archives.push_back(entry.path());
                }
            }
            return archives;
        }

        /**
         * @brief 取归档.zip 或 .tar.gz 的旁路文件路径
         * @param archive 归档路径
         * @param suffix 追加在完整归档名后的后缀，如 .sig 或 .json
         * @return std::filesystem::path 旁路文件路径
         */
        [[nodiscard]] std::filesystem::path sidecarPath(const std::filesystem::path &archive, const std::string &suffix)
        {
            return std::filesystem::path{AsynGyanis::Platform::FileSystem::utf8FromPath(archive) + suffix};
        }

        /**
         * @brief 列出 tar.gz 或 zip 的顶层条目
         * @param archive 归档路径
         * @return 成功返回条目列表；失败返回中文原因
         */
        [[nodiscard]] std::expected<std::vector<std::string>, std::string> archiveEntries(const std::filesystem::path &archive)
        {
            const auto listed = runProcess("tar", {"-tf", AsynGyanis::Platform::FileSystem::utf8FromPath(archive)}, std::chrono::seconds{60});
            if (!listed.succeeded())
            {
                return std::unexpected("无法列出归档条目：" + listed.failure + listed.output);
            }
            return splitLines(listed.output);
        }
    } // namespace

    TEST(AgentPackage, LaunchesVerifiedSignedPackageAndRequestsStop)
    {
        TestSupport::TemporaryDirectory packageDirectory;
        const auto                      archive = buildPackageArchive(packageDirectory.path(), "2.0.0");
        ASSERT_TRUE(archive.has_value()) << archive.error();

        PackageHarness harness(archive->bytes);
        const auto     key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string signature = key->signDigest(archive->digest);

        std::error_code error;
        std::filesystem::create_directories(harness.healthMarkerPath().parent_path(), error);
        std::ofstream(harness.healthMarkerPath()) << "stale";
        ASSERT_TRUE(std::filesystem::exists(harness.healthMarkerPath()));

        // 命令里的 manifest 只是历史元数据；与归档不一致也不得影响升级
        ConfigValue payload = packagePayload("2.0.0", archive->digest, signature);
        payload["manifest"] = R"({"version":"0.0.1","platform":"plan9-x86_64"})";
        const CommandResult result = harness.execute(payload.dump());
        ASSERT_TRUE(result.succeeded) << result.message;

        const auto parsed = AsynGyanis::Base::parseConfigValue(result.result);
        ASSERT_TRUE(parsed.has_value()) << result.result;
        EXPECT_EQ("2.0.0", parsed->value("version", std::string{}));
        EXPECT_EQ(archive->digest, parsed->value("sha256", std::string{}));
        EXPECT_EQ("launched", parsed->value("update", std::string{}));

        const auto state = harness.updateState();
        ASSERT_TRUE(state.has_value()) << state.error();
        EXPECT_EQ("2.0.0", state->version);
        EXPECT_FALSE(state->previousBinary.empty());
        EXPECT_TRUE(state->healthyAt.empty());
        const auto pending = AsynGyanis::Base::parseConfigValue(state->pendingResult);
        ASSERT_TRUE(pending.has_value()) << state->pendingResult;
        EXPECT_EQ(9, pending->value("commandId", 0));
        EXPECT_EQ("2.0.0", pending->value("version", std::string{}));
        EXPECT_EQ("pending", pending->value("status", std::string{}));

        EXPECT_FALSE(std::filesystem::exists(harness.healthMarkerPath())) << "升级前必须清掉旧健康标记";
        EXPECT_EQ(1, harness.downloads());
        EXPECT_EQ(1, harness.launchCount());
        EXPECT_EQ(1, harness.stopRequests());

        const std::vector<std::filesystem::path> staged = harness.stagedFiles();
        ASSERT_EQ(1U, staged.size()) << "安装器还要消费暂存包，成功路径不得删除";
        const std::string stagedName = staged.front().filename().string();
        EXPECT_NE(std::string::npos, stagedName.find("2.0.0"));
        EXPECT_EQ(archive->bytes, readTextFile(staged.front())) << "暂存包必须是校验过摘要的那份归档";

        const std::vector<std::string> arguments       = harness.launchArguments();
        const auto                     containsArgument = [&arguments](const std::string &expected)
        {
            return std::find(arguments.begin(), arguments.end(), expected) != arguments.end();
        };
#if defined(_WIN32)
        EXPECT_NE(std::string::npos, harness.launchExecutable().string().find("powershell"));
        EXPECT_TRUE(containsArgument("-Upgrade"));
        EXPECT_TRUE(containsArgument(AsynGyanis::Platform::FileSystem::utf8FromPath(staged.front())));
        EXPECT_TRUE(containsArgument(AsynGyanis::Platform::FileSystem::utf8FromPath(harness.installerPath())));
        EXPECT_TRUE(containsArgument("30"));
        const auto executable = currentExecutablePath();
        ASSERT_TRUE(executable.has_value()) << executable.error();
        EXPECT_TRUE(containsArgument(AsynGyanis::Platform::FileSystem::utf8FromPath(executable->parent_path())));
#else
        EXPECT_EQ("systemd-run", harness.launchExecutable().string());
        EXPECT_TRUE(containsArgument("--property=Type=exec")) << "启动结果必须能看见安装器 exec 失败";
        EXPECT_TRUE(containsArgument("--upgrade"));
        EXPECT_TRUE(containsArgument(AsynGyanis::Platform::FileSystem::utf8FromPath(staged.front())));
        EXPECT_TRUE(containsArgument(AsynGyanis::Platform::FileSystem::utf8FromPath(harness.installerPath())));
        EXPECT_TRUE(containsArgument("--health-timeout"));
        EXPECT_TRUE(containsArgument("30"));
        bool unitFound = false;
        for (const std::string &argument: arguments)
        {
            if (argument.rfind("--unit=edgepulse-agent-update-", 0) == 0)
            {
                unitFound = true;
            }
        }
        EXPECT_TRUE(unitFound);
#endif
    }

    TEST(AgentPackage, RejectsArchiveTamperedAfterSigning)
    {
        TestSupport::TemporaryDirectory packageDirectory;
        const auto                      archive = buildPackageArchive(packageDirectory.path(), "2.0.0");
        ASSERT_TRUE(archive.has_value()) << archive.error();

        PackageHarness harness(archive->bytes + "tampered-after-signing");
        const auto     key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string signature = key->signDigest(archive->digest);

        const CommandResult result = harness.execute(packagePayload("2.0.0", archive->digest, signature).dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("SHA-256")) << result.message;
        expectNoUpgradeLeftBehind(harness);
        EXPECT_EQ(1, harness.downloads()) << "签名覆盖的是命令摘要，篡改只能在下载后按哈希拒绝";
    }

    TEST(AgentPackage, RejectsSignatureOverDifferentDigest)
    {
        TestSupport::TemporaryDirectory packageDirectory;
        const auto                      archive = buildPackageArchive(packageDirectory.path(), "2.0.0");
        ASSERT_TRUE(archive.has_value()) << archive.error();

        PackageHarness harness(archive->bytes);
        const auto     key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        // 签名覆盖另一份摘要，与命令给出的归档摘要不匹配
        const std::string signature = key->signDigest(AsynGyanis::Core::Digest::sha256Hex("another-archive"));

        const CommandResult result = harness.execute(packagePayload("2.0.0", archive->digest, signature).dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("签名")) << result.message;
        expectNoUpgradeLeftBehind(harness);
        EXPECT_EQ(0, harness.downloads()) << "验签失败不得下载";
    }

    TEST(AgentPackage, RejectsUnknownKeyId)
    {
        TestSupport::TemporaryDirectory packageDirectory;
        const auto                      archive = buildPackageArchive(packageDirectory.path(), "2.0.0");
        ASSERT_TRUE(archive.has_value()) << archive.error();

        PackageHarness harness(archive->bytes);
        const auto     key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string signature = key->signDigest(archive->digest);

        const CommandResult result = harness.execute(packagePayload("2.0.0", archive->digest, signature, "missing-key").dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("签名")) << result.message;
        expectNoUpgradeLeftBehind(harness);
        EXPECT_EQ(0, harness.downloads());
    }

    TEST(AgentPackage, RejectsEmptyTrustedKeysDirectory)
    {
        TestSupport::TemporaryDirectory packageDirectory;
        const auto                      archive = buildPackageArchive(packageDirectory.path(), "2.0.0");
        ASSERT_TRUE(archive.has_value()) << archive.error();

        PackageHarness  harness(archive->bytes);
        std::error_code error;
        std::filesystem::create_directories(harness.trustedKeysDirectory(), error);
        ASSERT_FALSE(error) << error.message();

        const CommandResult result = harness.execute(packagePayload("2.0.0", archive->digest, std::string(88, 'A')).dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("公钥")) << result.message;
        expectNoUpgradeLeftBehind(harness);
        EXPECT_EQ(0, harness.downloads());
    }

    TEST(AgentPackage, RejectsWrongAlgorithm)
    {
        TestSupport::TemporaryDirectory packageDirectory;
        const auto                      archive = buildPackageArchive(packageDirectory.path(), "2.0.0");
        ASSERT_TRUE(archive.has_value()) << archive.error();

        PackageHarness harness(archive->bytes);
        const auto     key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string signature = key->signDigest(archive->digest);

        const CommandResult result = harness.execute(packagePayload("2.0.0", archive->digest, signature, "pkg-2026", "rsa-sha256").dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("ed25519-sha256")) << result.message;
        expectNoUpgradeLeftBehind(harness);
        EXPECT_EQ(0, harness.downloads());
    }

    TEST(AgentPackage, RejectsEmptySignatureOrSigningKeyId)
    {
        TestSupport::TemporaryDirectory packageDirectory;
        const auto                      archive = buildPackageArchive(packageDirectory.path(), "2.0.0");
        ASSERT_TRUE(archive.has_value()) << archive.error();

        PackageHarness harness(archive->bytes);
        const auto     key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string signature = key->signDigest(archive->digest);

        const CommandResult emptySignature = harness.execute(packagePayload("2.0.0", archive->digest, "").dump());
        EXPECT_FALSE(emptySignature.succeeded);
        EXPECT_NE(std::string::npos, emptySignature.message.find("signature")) << emptySignature.message;
        expectNoUpgradeLeftBehind(harness);

        const CommandResult emptyKeyId = harness.execute(packagePayload("2.0.0", archive->digest, signature, "").dump());
        EXPECT_FALSE(emptyKeyId.succeeded);
        EXPECT_NE(std::string::npos, emptyKeyId.message.find("signingKeyId")) << emptyKeyId.message;
        expectNoUpgradeLeftBehind(harness);
        EXPECT_EQ(0, harness.downloads());
    }

    TEST(AgentPackage, RejectsMissingOrEmptyVersion)
    {
        TestSupport::TemporaryDirectory packageDirectory;
        const auto                      archive = buildPackageArchive(packageDirectory.path(), "2.0.0");
        ASSERT_TRUE(archive.has_value()) << archive.error();

        {
            PackageHarness harness(archive->bytes);
            const auto     key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
            ASSERT_TRUE(key.has_value()) << key.error();
            const CommandResult result = harness.execute(packagePayload("", archive->digest, key->signDigest(archive->digest)).dump());
            EXPECT_FALSE(result.succeeded);
            EXPECT_NE(std::string::npos, result.message.find("version")) << result.message;
            expectNoUpgradeLeftBehind(harness);
            EXPECT_EQ(0, harness.downloads());
        }
        {
            PackageHarness harness(archive->bytes);
            const auto     key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
            ASSERT_TRUE(key.has_value()) << key.error();
            const ConfigValue payload = ConfigValue::object({{"url", "/api/agent/update/download"},
                                                            {"sha256", archive->digest},
                                                            {"signature", key->signDigest(archive->digest)},
                                                            {"signingKeyId", "pkg-2026"},
                                                            {"algorithm", "ed25519-sha256"}});
            const CommandResult result = harness.execute(payload.dump());
            EXPECT_FALSE(result.succeeded);
            EXPECT_NE(std::string::npos, result.message.find("version")) << result.message;
            expectNoUpgradeLeftBehind(harness);
            EXPECT_EQ(0, harness.downloads());
        }
    }

    TEST(AgentPackage, RejectsNonHexSha256)
    {
        TestSupport::TemporaryDirectory packageDirectory;
        const auto                      archive = buildPackageArchive(packageDirectory.path(), "2.0.0");
        ASSERT_TRUE(archive.has_value()) << archive.error();

        PackageHarness harness(archive->bytes);
        const auto     key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string signature = key->signDigest(archive->digest);

        const CommandResult result = harness.execute(packagePayload("2.0.0", "not-a-sha256", signature).dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("sha256")) << result.message;
        expectNoUpgradeLeftBehind(harness);
        EXPECT_EQ(0, harness.downloads());
    }

    TEST(AgentPackage, DownloadHttpFailureLeavesNoUpgradeState)
    {
        TestSupport::TemporaryDirectory packageDirectory;
        const auto                      archive = buildPackageArchive(packageDirectory.path(), "2.0.0");
        ASSERT_TRUE(archive.has_value()) << archive.error();

        for (const int failingStatus: {404, 500})
        {
            PackageHarness harness(archive->bytes);
            harness.setDownloadStatus(failingStatus);
            const auto key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
            ASSERT_TRUE(key.has_value()) << key.error();
            const std::string signature = key->signDigest(archive->digest);

            const CommandResult result = harness.execute(packagePayload("2.0.0", archive->digest, signature).dump());
            EXPECT_FALSE(result.succeeded);
            EXPECT_NE(std::string::npos, result.message.find("HTTP " + std::to_string(failingStatus))) << result.message;
            expectNoUpgradeLeftBehind(harness);
            EXPECT_EQ(1, harness.downloads());
        }
    }

    TEST(AgentPackage, LauncherFailureRevertsPendingState)
    {
        TestSupport::TemporaryDirectory packageDirectory;
        const auto                      archive = buildPackageArchive(packageDirectory.path(), "2.0.0");
        ASSERT_TRUE(archive.has_value()) << archive.error();

        PackageHarness harness(archive->bytes);
        harness.setLauncherFailure();
        const auto key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string signature = key->signDigest(archive->digest);

        std::error_code error;
        std::filesystem::create_directories(harness.healthMarkerPath().parent_path(), error);
        std::ofstream(harness.healthMarkerPath()) << "stale";
        ASSERT_TRUE(std::filesystem::exists(harness.healthMarkerPath()));

        const CommandResult result = harness.execute(packagePayload("2.0.0", archive->digest, signature).dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("安装器")) << result.message;
        expectNoUpgradeLeftBehind(harness, 1);
        EXPECT_TRUE(std::filesystem::exists(harness.healthMarkerPath())) << "启动安装器失败必须恢复旧健康标记";
        EXPECT_EQ("stale", readTextFile(harness.healthMarkerPath()));
    }

    TEST(AgentPackage, RejectsMissingInstallerBeforeWritingState)
    {
        TestSupport::TemporaryDirectory packageDirectory;
        const auto                      archive = buildPackageArchive(packageDirectory.path(), "2.0.0");
        ASSERT_TRUE(archive.has_value()) << archive.error();

        PackageHarness harness(archive->bytes);
        const auto     key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string signature = key->signDigest(archive->digest);

        std::error_code error;
        std::filesystem::create_directories(harness.healthMarkerPath().parent_path(), error);
        std::ofstream(harness.healthMarkerPath()) << "stale";
        ASSERT_TRUE(std::filesystem::exists(harness.healthMarkerPath()));
        harness.removeInstaller();

        const CommandResult result = harness.execute(packagePayload("2.0.0", archive->digest, signature).dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("安装器")) << result.message;
        expectNoUpgradeLeftBehind(harness);
        EXPECT_TRUE(std::filesystem::exists(harness.healthMarkerPath())) << "安装器缺失不得删除健康标记";
        EXPECT_EQ("stale", readTextFile(harness.healthMarkerPath()));
    }

    TEST(AgentPackage, StateStoresUpdateFieldsAndPendingResult)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = AgentState::open(directory.path() / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();

        const auto initial = state->updateState();
        ASSERT_TRUE(initial.has_value()) << initial.error();
        EXPECT_TRUE(initial->version.empty());
        EXPECT_TRUE(initial->pendingResult.empty());

        UpdateStateRecord record;
        record.version        = "3.1.4";
        record.previousBinary = "/usr/local/bin/edgepulse-agent";
        record.pendingResult  = R"({"commandId":12,"version":"3.1.4","status":"pending"})";
        const auto written    = state->setUpdateState(record);
        ASSERT_TRUE(written.has_value()) << written.error();

        const auto stored = state->updateState();
        ASSERT_TRUE(stored.has_value()) << stored.error();
        EXPECT_EQ(record.version, stored->version);
        EXPECT_EQ(record.previousBinary, stored->previousBinary);
        EXPECT_TRUE(stored->healthyAt.empty());
        EXPECT_EQ(record.pendingResult, stored->pendingResult);

        const auto pending = state->pendingUpdateResult();
        ASSERT_TRUE(pending.has_value());
        EXPECT_EQ(record.pendingResult, *pending);

        state->setPendingUpdateResult(R"({"commandId":12,"version":"3.1.4","status":"succeeded"})");
        const auto updated = state->updateState();
        ASSERT_TRUE(updated.has_value());
        EXPECT_EQ(record.version, updated->version) << "只改待上报结果不应动其它字段";
    }

    TEST(AgentPackage, SignScriptSignsArchiveDigestAndManifest)
    {
        if (!toolWorks("openssl", {"version"}, std::chrono::seconds{10}))
        {
            GTEST_SKIP() << "缺少 openssl，跳过签名脚本冒烟";
        }
        if (!toolWorks(AsynGyanis::Platform::FileSystem::utf8FromPath(bashPath()), {"-c", "exit 0"}, std::chrono::seconds{10}))
        {
            GTEST_SKIP() << "缺少 bash，跳过签名脚本冒烟";
        }

        TestSupport::TemporaryDirectory directory;
        const std::filesystem::path     keyPath   = directory.path() / "signing-key.pem";
        const std::filesystem::path     keyDir    = directory.path() / "keys";
        const std::filesystem::path     publicKey = keyDir / "pkg-2026.pem";
        std::error_code                 error;
        std::filesystem::create_directories(keyDir, error);
        ASSERT_FALSE(error) << error.message();

        auto generated = runProcess("openssl", {"genpkey", "-algorithm", "ED25519", "-out", AsynGyanis::Platform::FileSystem::utf8FromPath(keyPath)},
                                    std::chrono::seconds{30});
        ASSERT_TRUE(generated.succeeded()) << generated.failure;
        auto exported = runProcess("openssl",
                                   {"pkey", "-in", AsynGyanis::Platform::FileSystem::utf8FromPath(keyPath), "-pubout", "-out",
                                    AsynGyanis::Platform::FileSystem::utf8FromPath(publicKey)},
                                   std::chrono::seconds{30});
        ASSERT_TRUE(exported.succeeded()) << exported.failure;

        const auto script = std::filesystem::path{EDGEPULSE_SOURCE_DIR} / "scripts" / "sign-agent-package.sh";
        ASSERT_TRUE(std::filesystem::exists(script)) << script;

        // 通用输入：签归档文件的 SHA-256 摘要，写出 <文件>.sig
        const std::string archiveBytes = "EDGEPULSE-AGENT-ARCHIVE-FOR-SIGNER";
        const auto        archivePath  = directory.write("edgepulse-agent-2.0.0.tar.gz", archiveBytes);
        const auto        signedArchive = runProcess(AsynGyanis::Platform::FileSystem::utf8FromPath(bashPath()),
                                                     {AsynGyanis::Platform::FileSystem::utf8FromPath(script), AsynGyanis::Platform::FileSystem::utf8FromPath(keyPath),
                                                      AsynGyanis::Platform::FileSystem::utf8FromPath(archivePath), "pkg-2026"},
                                                     std::chrono::seconds{30});
        ASSERT_TRUE(signedArchive.succeeded()) << signedArchive.failure << signedArchive.output;

        const std::filesystem::path archiveSignature = sidecarPath(archivePath, ".sig");
        ASSERT_TRUE(std::filesystem::exists(archiveSignature)) << archiveSignature;
        const auto verifier = Firmware::FirmwareSignatureVerifier::loadFromDirectory(keyDir);
        ASSERT_TRUE(verifier.has_value()) << verifier.error();
        EXPECT_TRUE(verifier->verify("pkg-2026", AsynGyanis::Core::Digest::sha256Hex(archiveBytes), readTextFile(archiveSignature)).has_value());
        EXPECT_FALSE(verifier->verify("pkg-2026", AsynGyanis::Core::Digest::sha256Hex(archiveBytes + "x"), readTextFile(archiveSignature)).has_value())
                << "篡改归档后签名必须失败";

        // 已存在的签名默认不覆盖
        const auto overwrite = runProcess(AsynGyanis::Platform::FileSystem::utf8FromPath(bashPath()),
                                          {AsynGyanis::Platform::FileSystem::utf8FromPath(script), AsynGyanis::Platform::FileSystem::utf8FromPath(keyPath),
                                           AsynGyanis::Platform::FileSystem::utf8FromPath(archivePath), "pkg-2026"},
                                          std::chrono::seconds{30});
        EXPECT_FALSE(overwrite.succeeded()) << "已存在的签名文件不得被静默覆盖";

        // manifest 行为保留：校验内嵌 key id 与算法，写出 manifest.sig
        const ConfigValue manifestValue = ConfigValue::object(
                {{"version", "2.0.0"}, {"platform", std::string{kAgentPlatform}}, {"signingKeyId", "pkg-2026"}, {"signatureAlgorithm", "ed25519-sha256"}});
        const std::string manifest     = manifestValue.dump();
        const auto        manifestPath = directory.write("manifest.json", manifest);
        const auto        signedManifest = runProcess(AsynGyanis::Platform::FileSystem::utf8FromPath(bashPath()),
                                                      {AsynGyanis::Platform::FileSystem::utf8FromPath(script), AsynGyanis::Platform::FileSystem::utf8FromPath(keyPath),
                                                       AsynGyanis::Platform::FileSystem::utf8FromPath(manifestPath), "pkg-2026"},
                                                      std::chrono::seconds{30});
        ASSERT_TRUE(signedManifest.succeeded()) << signedManifest.failure << signedManifest.output;
        const std::filesystem::path manifestSignature = manifestPath.parent_path() / "manifest.sig";
        ASSERT_TRUE(std::filesystem::exists(manifestSignature));
        EXPECT_TRUE(verifier->verify("pkg-2026", AsynGyanis::Core::Digest::sha256Hex(manifest), readTextFile(manifestSignature)).has_value());

        const auto mismatched = runProcess(AsynGyanis::Platform::FileSystem::utf8FromPath(bashPath()),
                                           {AsynGyanis::Platform::FileSystem::utf8FromPath(script), AsynGyanis::Platform::FileSystem::utf8FromPath(keyPath),
                                            AsynGyanis::Platform::FileSystem::utf8FromPath(manifestPath), "another-key"},
                                           std::chrono::seconds{30});
        EXPECT_FALSE(mismatched.succeeded()) << "key id 与 manifest 不一致时必须拒绝签名";
    }

    TEST(AgentPackage, PackageScriptSignsArchiveAndIncludesChecksums)
    {
        if (!toolWorks("openssl", {"version"}, std::chrono::seconds{10}))
        {
            GTEST_SKIP() << "缺少 openssl，跳过打包脚本冒烟";
        }
        if (!toolWorks(AsynGyanis::Platform::FileSystem::utf8FromPath(bashPath()), {"-c", "command -v tar && command -v sha256sum && command -v install"},
                       std::chrono::seconds{10}))
        {
            GTEST_SKIP() << "缺少 tar、sha256sum 或 install，跳过打包脚本冒烟";
        }

        TestSupport::TemporaryDirectory directory;
        const std::filesystem::path     keyPath   = directory.path() / "signing-key.pem";
        const std::filesystem::path     keyDir    = directory.path() / "keys";
        const std::filesystem::path     publicKey = keyDir / "pkg-2026.pem";
        std::error_code                 error;
        std::filesystem::create_directories(keyDir, error);
        ASSERT_FALSE(error) << error.message();

        auto generated = runProcess("openssl", {"genpkey", "-algorithm", "ED25519", "-out", AsynGyanis::Platform::FileSystem::utf8FromPath(keyPath)},
                                    std::chrono::seconds{30});
        ASSERT_TRUE(generated.succeeded()) << generated.failure;
        auto exported = runProcess("openssl",
                                   {"pkey", "-in", AsynGyanis::Platform::FileSystem::utf8FromPath(keyPath), "-pubout", "-out",
                                    AsynGyanis::Platform::FileSystem::utf8FromPath(publicKey)},
                                   std::chrono::seconds{30});
        ASSERT_TRUE(exported.succeeded()) << exported.failure;

        const auto script = std::filesystem::path{EDGEPULSE_SOURCE_DIR} / "scripts" / "package-agent.sh";
        const auto binary = std::filesystem::path{EDGEPULSE_AGENT_TEST_HOOK_PATH};
        const auto output = directory.path() / "output";
        ASSERT_TRUE(std::filesystem::exists(script)) << script;
        ASSERT_TRUE(std::filesystem::exists(binary)) << binary;

        const auto packaged = runProcess(AsynGyanis::Platform::FileSystem::utf8FromPath(bashPath()),
                                         {AsynGyanis::Platform::FileSystem::utf8FromPath(script), "--version", "9.9.9", "--output",
                                          AsynGyanis::Platform::FileSystem::utf8FromPath(output), "--binary", AsynGyanis::Platform::FileSystem::utf8FromPath(binary),
                                          "--signing-key", AsynGyanis::Platform::FileSystem::utf8FromPath(keyPath), "--signing-key-id", "pkg-2026"},
                                         std::chrono::seconds{120});
        ASSERT_TRUE(packaged.succeeded()) << packaged.failure << packaged.output;

        const std::vector<std::filesystem::path> archives = findArchives(output, ".gz");
        ASSERT_EQ(1U, archives.size()) << "打包脚本应产出一个 tar.gz";
        const std::filesystem::path archive = archives.front();
        EXPECT_NE(std::string::npos, archive.filename().string().find("9.9.9"));

        const std::string archiveBytes = readTextFile(archive);
        const std::string archiveDigest = AsynGyanis::Core::Digest::sha256Hex(archiveBytes);
        const auto        verifier      = Firmware::FirmwareSignatureVerifier::loadFromDirectory(keyDir);
        ASSERT_TRUE(verifier.has_value()) << verifier.error();

        const std::filesystem::path archiveSignature = sidecarPath(archive, ".sig");
        const std::filesystem::path metadataPath     = sidecarPath(archive, ".json");
        ASSERT_TRUE(std::filesystem::exists(archiveSignature)) << "签名包必须写出 <归档>.sig";
        ASSERT_TRUE(std::filesystem::exists(metadataPath)) << "签名包必须写出 <归档>.json";
        const std::string signature = readTextFile(archiveSignature);
        EXPECT_TRUE(verifier->verify("pkg-2026", archiveDigest, signature).has_value()) << "签名必须覆盖归档本身的 SHA-256";
        EXPECT_FALSE(verifier->verify("pkg-2026", AsynGyanis::Core::Digest::sha256Hex(archiveBytes + "tampered"), signature).has_value());

        const auto metadata = AsynGyanis::Base::parseConfigValue(readTextFile(metadataPath));
        ASSERT_TRUE(metadata.has_value()) << readTextFile(metadataPath);
        const std::string metadataPlatform = metadata->value("platform", std::string{});
        EXPECT_EQ("9.9.9", metadata->value("version", std::string{}));
        EXPECT_EQ(0U, metadataPlatform.rfind("linux-", 0)) << "bash 打包脚本面向 Linux 归档：" << metadataPlatform;
        EXPECT_EQ(archiveDigest, metadata->value("sha256", std::string{}));
        EXPECT_EQ(signature, metadata->value("signature", std::string{}));
        EXPECT_EQ("pkg-2026", metadata->value("signingKeyId", std::string{}));
        EXPECT_EQ("ed25519-sha256", metadata->value("algorithm", std::string{}));

        const auto entries = archiveEntries(archive);
        ASSERT_TRUE(entries.has_value()) << entries.error();
        const bool hasManifestSignature = std::find(entries->begin(), entries->end(), "manifest.sig") != entries->end();
        const bool hasChecksums         = std::find(entries->begin(), entries->end(), "checksums.sha256") != entries->end();
        EXPECT_TRUE(hasManifestSignature) << "manifest.sig 必须在归档内";
        EXPECT_TRUE(hasChecksums) << "checksums.sha256 必须在归档内";
        for (const std::string &entry: *entries)
        {
            EXPECT_EQ(std::string::npos, entry.find(".tar.gz.sig")) << "归档签名旁路文件不得进入归档";
            EXPECT_EQ(std::string::npos, entry.find(".tar.gz.json")) << "归档元数据旁路文件不得进入归档";
        }

        const std::filesystem::path extract = directory.path() / "extract";
        std::filesystem::create_directories(extract, error);
        ASSERT_FALSE(error) << error.message();
        const auto extracted = runProcess("tar", {"-xzf", AsynGyanis::Platform::FileSystem::utf8FromPath(archive), "-C", AsynGyanis::Platform::FileSystem::utf8FromPath(extract)},
                                          std::chrono::seconds{60});
        ASSERT_TRUE(extracted.succeeded()) << extracted.failure;

        const std::filesystem::path manifestPath  = extract / "manifest.json";
        const std::filesystem::path signaturePath = extract / "manifest.sig";
        const std::filesystem::path checksumsPath = extract / "checksums.sha256";
        ASSERT_TRUE(std::filesystem::exists(manifestPath));
        ASSERT_TRUE(std::filesystem::exists(signaturePath)) << "包内必须有 manifest.sig";
        ASSERT_TRUE(std::filesystem::exists(checksumsPath)) << "包内必须有 checksums.sha256";

        const std::string manifest = readTextFile(manifestPath);
        const auto        parsed   = AsynGyanis::Base::parseConfigValue(manifest);
        ASSERT_TRUE(parsed.has_value()) << manifest;
        EXPECT_EQ("9.9.9", parsed->value("version", std::string{}));
        EXPECT_EQ("pkg-2026", parsed->value("signingKeyId", std::string{}));
        EXPECT_EQ("ed25519-sha256", parsed->value("signatureAlgorithm", std::string{}));
        bool agentListed = false;
        for (const auto &file: parsed->at("files"))
        {
            if (file.value("path", std::string{}) == "edgepulse-agent")
            {
                agentListed = true;
            }
        }
        EXPECT_TRUE(agentListed) << "manifest 必须列出 edgepulse-agent";
        EXPECT_TRUE(verifier->verify("pkg-2026", AsynGyanis::Core::Digest::sha256Hex(manifest), readTextFile(signaturePath)).has_value())
                << "包内 manifest.sig 是可选的纵深防御，必须能验证";

        std::size_t checkedFiles = 0;
        for (const std::string &line: splitLines(readTextFile(checksumsPath)))
        {
            const std::size_t separator = line.find("  ");
            ASSERT_NE(std::string::npos, separator) << line;
            const std::string expected = line.substr(0, separator);
            const std::string name     = line.substr(separator + 2);
            const std::filesystem::path file = extract / name;
            ASSERT_TRUE(std::filesystem::exists(file)) << file;
            EXPECT_EQ(expected, AsynGyanis::Core::Digest::sha256Hex(readTextFile(file))) << name;
            ++checkedFiles;
        }
        EXPECT_GT(checkedFiles, 0U);
        EXPECT_EQ(std::string::npos, readTextFile(checksumsPath).find("checksums.sha256")) << "校验文件不得把自身列进去";

        // 没有私钥时不产出任何签名旁路，只用于本地或手工安装
        const auto unsignedOutput = directory.path() / "unsigned";
        const auto unsignedRun    = runProcess(AsynGyanis::Platform::FileSystem::utf8FromPath(bashPath()),
                                            {AsynGyanis::Platform::FileSystem::utf8FromPath(script), "--version", "9.9.9", "--output",
                                             AsynGyanis::Platform::FileSystem::utf8FromPath(unsignedOutput), "--binary",
                                             AsynGyanis::Platform::FileSystem::utf8FromPath(binary)},
                                            std::chrono::seconds{120});
        ASSERT_TRUE(unsignedRun.succeeded()) << unsignedRun.failure << unsignedRun.output;
        const std::vector<std::filesystem::path> unsignedArchives = findArchives(unsignedOutput, ".gz");
        ASSERT_EQ(1U, unsignedArchives.size());
        EXPECT_FALSE(std::filesystem::exists(sidecarPath(unsignedArchives.front(), ".sig")));
        EXPECT_FALSE(std::filesystem::exists(sidecarPath(unsignedArchives.front(), ".json")));
    }

#if defined(_WIN32)
    TEST(AgentPackage, PowerShellPackageScriptSignsArchive)
    {
        if (!toolWorks("openssl", {"version"}, std::chrono::seconds{10}))
        {
            GTEST_SKIP() << "缺少 openssl，跳过 PowerShell 打包脚本冒烟";
        }
        if (!toolWorks("powershell.exe", {"-NoProfile", "-Command", "exit 0"}, std::chrono::seconds{20}))
        {
            GTEST_SKIP() << "缺少 PowerShell，跳过打包脚本冒烟";
        }

        TestSupport::TemporaryDirectory directory;
        const std::filesystem::path     keyPath   = directory.path() / "signing-key.pem";
        const std::filesystem::path     keyDir    = directory.path() / "keys";
        const std::filesystem::path     publicKey = keyDir / "pkg-2026.pem";
        std::error_code                 error;
        std::filesystem::create_directories(keyDir, error);
        ASSERT_FALSE(error) << error.message();

        auto generated = runProcess("openssl", {"genpkey", "-algorithm", "ED25519", "-out", AsynGyanis::Platform::FileSystem::utf8FromPath(keyPath)},
                                    std::chrono::seconds{30});
        ASSERT_TRUE(generated.succeeded()) << generated.failure;
        auto exported = runProcess("openssl",
                                   {"pkey", "-in", AsynGyanis::Platform::FileSystem::utf8FromPath(keyPath), "-pubout", "-out",
                                    AsynGyanis::Platform::FileSystem::utf8FromPath(publicKey)},
                                   std::chrono::seconds{30});
        ASSERT_TRUE(exported.succeeded()) << exported.failure;

        const auto script = std::filesystem::path{EDGEPULSE_SOURCE_DIR} / "scripts" / "package-agent.ps1";
        const auto binary = std::filesystem::path{EDGEPULSE_AGENT_TEST_HOOK_PATH};
        const auto output = directory.path() / "output";
        ASSERT_TRUE(std::filesystem::exists(script)) << script;

        const auto packaged = runProcess("powershell.exe",
                                         {"-NoProfile",
                                          "-ExecutionPolicy",
                                          "Bypass",
                                          "-File",
                                          AsynGyanis::Platform::FileSystem::utf8FromPath(script),
                                          "-Version",
                                          "9.9.8",
                                          "-Output",
                                          AsynGyanis::Platform::FileSystem::utf8FromPath(output),
                                          "-Binary",
                                          AsynGyanis::Platform::FileSystem::utf8FromPath(binary),
                                          "-SigningKey",
                                          AsynGyanis::Platform::FileSystem::utf8FromPath(keyPath),
                                          "-SigningKeyId",
                                          "pkg-2026"},
                                         std::chrono::seconds{180});
        ASSERT_TRUE(packaged.succeeded()) << packaged.failure << packaged.output;

        const std::vector<std::filesystem::path> archives = findArchives(output, ".zip");
        ASSERT_EQ(1U, archives.size()) << "PowerShell 打包脚本应产出一个 zip";
        const std::filesystem::path archive     = archives.front();
        const std::string           archiveBytes  = readTextFile(archive);
        const std::string           archiveDigest = AsynGyanis::Core::Digest::sha256Hex(archiveBytes);
        const auto                  verifier      = Firmware::FirmwareSignatureVerifier::loadFromDirectory(keyDir);
        ASSERT_TRUE(verifier.has_value()) << verifier.error();

        const std::filesystem::path archiveSignature = sidecarPath(archive, ".sig");
        const std::filesystem::path metadataPath     = sidecarPath(archive, ".json");
        ASSERT_TRUE(std::filesystem::exists(archiveSignature));
        ASSERT_TRUE(std::filesystem::exists(metadataPath));
        const std::string signature = readTextFile(archiveSignature);
        EXPECT_TRUE(verifier->verify("pkg-2026", archiveDigest, signature).has_value());
        EXPECT_FALSE(verifier->verify("pkg-2026", AsynGyanis::Core::Digest::sha256Hex(archiveBytes + "tampered"), signature).has_value());

        const auto metadata = AsynGyanis::Base::parseConfigValue(readTextFile(metadataPath));
        ASSERT_TRUE(metadata.has_value()) << readTextFile(metadataPath);
        const std::string metadataPlatform = metadata->value("platform", std::string{});
        EXPECT_EQ("9.9.8", metadata->value("version", std::string{}));
        EXPECT_EQ(0U, metadataPlatform.rfind("windows-", 0)) << "PowerShell 打包脚本面向 Windows 归档：" << metadataPlatform;
        EXPECT_EQ(archiveDigest, metadata->value("sha256", std::string{}));
        EXPECT_EQ(signature, metadata->value("signature", std::string{}));
        EXPECT_EQ("pkg-2026", metadata->value("signingKeyId", std::string{}));
        EXPECT_EQ("ed25519-sha256", metadata->value("algorithm", std::string{}));

        const auto entries = archiveEntries(archive);
        ASSERT_TRUE(entries.has_value()) << entries.error();
        const bool hasManifestSignature = std::find(entries->begin(), entries->end(), "manifest.sig") != entries->end();
        const bool hasChecksums         = std::find(entries->begin(), entries->end(), "checksums.sha256") != entries->end();
        EXPECT_TRUE(hasManifestSignature) << "zip 内必须有 manifest.sig";
        EXPECT_TRUE(hasChecksums) << "zip 内必须有 checksums.sha256";
    }
#endif
} // namespace EdgePulse::Agent
