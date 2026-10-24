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
            std::mutex               mutex;      ///< 保护字段
            int                      calls{0};   ///< 启动次数
            std::filesystem::path    executable; ///< 启动器路径
            std::vector<std::string> arguments;  ///< 启动参数
            bool                     fail{false}; ///< 是否模拟启动失败
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
         * @brief 构造一份签名前的 manifest 文本
         * @param version 包版本
         * @param platform 包平台
         * @param keyId 签名 key id
         * @param algorithm 签名算法
         * @param agentBinary 清单里的 Agent 文件名；空串表示删掉这一项
         * @return std::string 紧凑 JSON 文本
         */
        [[nodiscard]] std::string manifestText(const std::string &version, const std::string &platform, const std::string &keyId = "pkg-2026",
                                               const std::string &algorithm = "ed25519-sha256", const std::string &agentBinary = "edgepulse-agent")
        {
            ConfigValue files = ConfigValue::array();
            if (!agentBinary.empty())
            {
                files.push_back(ConfigValue::object({{"path", agentBinary}, {"size", 4096}, {"sha256", std::string(64, 'a')}}));
            }
            files.push_back(ConfigValue::object({{"path", "agent.yaml.example"}, {"size", 128}, {"sha256", std::string(64, 'b')}}));
            const ConfigValue manifest = ConfigValue::object(
                    {{"version", version}, {"platform", platform}, {"signingKeyId", keyId}, {"signatureAlgorithm", algorithm}, {"files", files}});
            return manifest.dump();
        }

        /**
         * @brief 构造 agent.update 的命令正文
         * @param version 目标版本
         * @param digest 归档 SHA-256
         * @param signature Base64 签名
         * @param manifest 被签名的 manifest 原文
         * @param keyId 签名 key id
         * @param algorithm 签名算法
         * @param url 下载地址
         * @return ConfigValue 命令正文对象
         */
        [[nodiscard]] ConfigValue packagePayload(const std::string &version, const std::string &digest, const std::string &signature, const std::string &manifest,
                                                 const std::string &keyId = "pkg-2026", const std::string &algorithm = "ed25519-sha256",
                                                 const std::string &url = "/api/agent/update/download")
        {
            return ConfigValue::object({{"version", version},
                                        {"url", url},
                                        {"sha256", digest},
                                        {"signature", signature},
                                        {"signingKeyId", keyId},
                                        {"algorithm", algorithm},
                                        {"manifest", manifest}});
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

                m_configuration.baseUrl                      = "http://127.0.0.1:" + std::to_string(m_server->port());
                m_configuration.allowPlaintextForTests       = true;
                m_configuration.name                         = "edge-01";
                m_configuration.requestTimeoutSeconds        = 10;
                m_configuration.stateDirectory               = m_directory.path() / "state";
                m_configuration.packageTrustedKeysDirectory  = m_directory.path() / "package-keys";
                m_configuration.updateStagingDirectory       = m_directory.path() / "update-staging";
                m_configuration.updateInstallerScript        = m_directory.path() / "install-agent";
                m_configuration.updateHealthTimeoutSeconds   = 30;
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
            TestSupport::TemporaryDirectory             m_directory;                       ///< 用例独占的临时目录
            std::shared_ptr<PackageServerState>         m_serverState;                     ///< 服务端记录
            std::unique_ptr<TestSupport::ServerHarness> m_server;                          ///< 回环服务器
            std::unique_ptr<AgentState>                 m_state;                           ///< 真实状态库
            AgentConfiguration                          m_configuration;                   ///< 执行器配置
            std::shared_ptr<LaunchRecord>               m_launch;                          ///< 注入的启动器记录
            std::atomic<int>                            m_stopRequests{0};                 ///< 停止回调次数
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
    } // namespace

    TEST(AgentPackage, LaunchesVerifiedSignedPackageAndRequestsStop)
    {
        const std::string archive = "EDGEPULSE-AGENT-ARCHIVE-" + std::string(4096, 'p');
        PackageHarness   harness(archive);
        const auto       key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();

        const std::string manifest  = manifestText("2.0.0", std::string{kAgentPlatform});
        const std::string digest    = AsynGyanis::Core::Digest::sha256Hex(archive);
        const std::string signature = key->signDigest(AsynGyanis::Core::Digest::sha256Hex(manifest));

        std::error_code error;
        std::filesystem::create_directories(harness.healthMarkerPath().parent_path(), error);
        std::ofstream(harness.healthMarkerPath()) << "stale";
        ASSERT_TRUE(std::filesystem::exists(harness.healthMarkerPath()));

        const CommandResult result = harness.execute(packagePayload("2.0.0", digest, signature, manifest).dump());
        ASSERT_TRUE(result.succeeded) << result.message;

        const auto parsed = AsynGyanis::Base::parseConfigValue(result.result);
        ASSERT_TRUE(parsed.has_value()) << result.result;
        EXPECT_EQ("2.0.0", parsed->value("version", std::string{}));
        EXPECT_EQ(digest, parsed->value("sha256", std::string{}));
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

    TEST(AgentPackage, RejectsTamperedArchive)
    {
        const std::string archive = "EDGEPULSE-AGENT-ARCHIVE-" + std::string(2048, 't');
        PackageHarness    harness(std::string(archive.size(), 'x'));
        const auto        key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string manifest  = manifestText("2.0.0", std::string{kAgentPlatform});
        const std::string digest    = AsynGyanis::Core::Digest::sha256Hex(archive);
        const std::string signature = key->signDigest(AsynGyanis::Core::Digest::sha256Hex(manifest));

        const CommandResult result = harness.execute(packagePayload("2.0.0", digest, signature, manifest).dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("SHA-256")) << result.message;
        expectNoUpgradeLeftBehind(harness);
    }

    TEST(AgentPackage, RejectsInvalidManifestSignature)
    {
        const std::string archive = "EDGEPULSE-AGENT-ARCHIVE-" + std::string(1024, 's');
        PackageHarness    harness(archive);
        const auto        key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string manifest = manifestText("2.0.0", std::string{kAgentPlatform});
        const std::string digest   = AsynGyanis::Core::Digest::sha256Hex(archive);
        // 签名覆盖另一份 manifest，原文与签名不再匹配
        const std::string signature = key->signDigest(AsynGyanis::Core::Digest::sha256Hex(manifest + " "));

        const CommandResult result = harness.execute(packagePayload("2.0.0", digest, signature, manifest).dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("签名")) << result.message;
        expectNoUpgradeLeftBehind(harness);
        EXPECT_EQ(0, harness.downloads()) << "验签失败不得下载";
    }

    TEST(AgentPackage, RejectsUnknownKeyId)
    {
        const std::string archive = "EDGEPULSE-AGENT-ARCHIVE-" + std::string(1024, 'k');
        PackageHarness    harness(archive);
        const auto        key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string manifest  = manifestText("2.0.0", std::string{kAgentPlatform}, "missing-key");
        const std::string digest    = AsynGyanis::Core::Digest::sha256Hex(archive);
        const std::string signature = key->signDigest(AsynGyanis::Core::Digest::sha256Hex(manifest));

        const CommandResult result = harness.execute(packagePayload("2.0.0", digest, signature, manifest, "missing-key").dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("签名")) << result.message;
        expectNoUpgradeLeftBehind(harness);
    }

    TEST(AgentPackage, RejectsManifestVersionMismatch)
    {
        const std::string archive = "EDGEPULSE-AGENT-ARCHIVE-" + std::string(1024, 'v');
        PackageHarness    harness(archive);
        const auto        key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string manifest  = manifestText("1.9.0", std::string{kAgentPlatform});
        const std::string digest    = AsynGyanis::Core::Digest::sha256Hex(archive);
        const std::string signature = key->signDigest(AsynGyanis::Core::Digest::sha256Hex(manifest));

        const CommandResult result = harness.execute(packagePayload("2.0.0", digest, signature, manifest).dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("版本")) << result.message;
        expectNoUpgradeLeftBehind(harness);
    }

    TEST(AgentPackage, RejectsManifestPlatformMismatch)
    {
        const std::string archive = "EDGEPULSE-AGENT-ARCHIVE-" + std::string(1024, 'l');
        PackageHarness    harness(archive);
        const auto        key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string manifest  = manifestText("2.0.0", "plan9-x86_64");
        const std::string digest    = AsynGyanis::Core::Digest::sha256Hex(archive);
        const std::string signature = key->signDigest(AsynGyanis::Core::Digest::sha256Hex(manifest));

        const CommandResult result = harness.execute(packagePayload("2.0.0", digest, signature, manifest).dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("平台")) << result.message;
        expectNoUpgradeLeftBehind(harness);
    }

    TEST(AgentPackage, RejectsManifestWithoutAgentBinary)
    {
        const std::string archive = "EDGEPULSE-AGENT-ARCHIVE-" + std::string(1024, 'b');
        PackageHarness    harness(archive);
        const auto        key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string manifest  = manifestText("2.0.0", std::string{kAgentPlatform}, "pkg-2026", "ed25519-sha256", "");
        const std::string digest    = AsynGyanis::Core::Digest::sha256Hex(archive);
        const std::string signature = key->signDigest(AsynGyanis::Core::Digest::sha256Hex(manifest));

        const CommandResult result = harness.execute(packagePayload("2.0.0", digest, signature, manifest).dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("edgepulse-agent")) << result.message;
        expectNoUpgradeLeftBehind(harness);
    }

    TEST(AgentPackage, RejectsWrongAlgorithm)
    {
        const std::string archive = "EDGEPULSE-AGENT-ARCHIVE-" + std::string(1024, 'a');
        PackageHarness    harness(archive);
        const auto        key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string manifest  = manifestText("2.0.0", std::string{kAgentPlatform}, "pkg-2026", "rsa-sha256");
        const std::string digest    = AsynGyanis::Core::Digest::sha256Hex(archive);
        const std::string signature = key->signDigest(AsynGyanis::Core::Digest::sha256Hex(manifest));

        const CommandResult result = harness.execute(packagePayload("2.0.0", digest, signature, manifest, "pkg-2026", "rsa-sha256").dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("ed25519-sha256")) << result.message;
        expectNoUpgradeLeftBehind(harness);
    }

    TEST(AgentPackage, RejectsNonHexSha256)
    {
        const std::string archive = "EDGEPULSE-AGENT-ARCHIVE-" + std::string(1024, 'h');
        PackageHarness    harness(archive);
        const auto        key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string manifest  = manifestText("2.0.0", std::string{kAgentPlatform});
        const std::string signature = key->signDigest(AsynGyanis::Core::Digest::sha256Hex(manifest));

        const CommandResult result = harness.execute(packagePayload("2.0.0", "not-a-sha256", signature, manifest).dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("sha256")) << result.message;
        expectNoUpgradeLeftBehind(harness);
        EXPECT_EQ(0, harness.downloads());
    }

    TEST(AgentPackage, DownloadHttpFailureLeavesNoUpgradeState)
    {
        const std::string archive = "EDGEPULSE-AGENT-ARCHIVE-" + std::string(512, 'd');
        const std::string digest  = AsynGyanis::Core::Digest::sha256Hex(archive);

        for (const int failingStatus: {404, 500})
        {
            PackageHarness harness(archive);
            harness.setDownloadStatus(failingStatus);
            const auto key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
            ASSERT_TRUE(key.has_value()) << key.error();
            const std::string manifest  = manifestText("2.0.0", std::string{kAgentPlatform});
            const std::string signature = key->signDigest(AsynGyanis::Core::Digest::sha256Hex(manifest));

            const CommandResult result = harness.execute(packagePayload("2.0.0", digest, signature, manifest).dump());
            EXPECT_FALSE(result.succeeded);
            EXPECT_NE(std::string::npos, result.message.find("HTTP " + std::to_string(failingStatus))) << result.message;
            expectNoUpgradeLeftBehind(harness);
            EXPECT_EQ(1, harness.downloads());
        }
    }

    TEST(AgentPackage, LauncherFailureRevertsPendingState)
    {
        const std::string archive = "EDGEPULSE-AGENT-ARCHIVE-" + std::string(1024, 'f');
        PackageHarness    harness(archive);
        harness.setLauncherFailure();
        const auto key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "pkg-2026");
        ASSERT_TRUE(key.has_value()) << key.error();
        const std::string manifest  = manifestText("2.0.0", std::string{kAgentPlatform});
        const std::string digest    = AsynGyanis::Core::Digest::sha256Hex(archive);
        const std::string signature = key->signDigest(AsynGyanis::Core::Digest::sha256Hex(manifest));

        const CommandResult result = harness.execute(packagePayload("2.0.0", digest, signature, manifest).dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("安装器")) << result.message;
        expectNoUpgradeLeftBehind(harness, 1);
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

    TEST(AgentPackage, SignScriptProducesVerifiableSignature)
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

        const std::string manifest = manifestText("2.0.0", std::string{kAgentPlatform});
        const std::filesystem::path manifestPath = directory.write("manifest.json", manifest);
        const auto script = std::filesystem::path{EDGEPULSE_SOURCE_DIR} / "scripts" / "sign-agent-package.sh";
        ASSERT_TRUE(std::filesystem::exists(script)) << script;

        const auto signedResult = runProcess(AsynGyanis::Platform::FileSystem::utf8FromPath(bashPath()),
                                             {AsynGyanis::Platform::FileSystem::utf8FromPath(script), AsynGyanis::Platform::FileSystem::utf8FromPath(keyPath),
                                              AsynGyanis::Platform::FileSystem::utf8FromPath(manifestPath), "pkg-2026"},
                                             std::chrono::seconds{30});
        ASSERT_TRUE(signedResult.succeeded()) << signedResult.failure << signedResult.output;

        const std::filesystem::path signaturePath = manifestPath.parent_path() / "manifest.sig";
        ASSERT_TRUE(std::filesystem::exists(signaturePath));
        const std::string signature = readTextFile(signaturePath);
        ASSERT_FALSE(signature.empty());

        const auto verifier = Firmware::FirmwareSignatureVerifier::loadFromDirectory(keyDir);
        ASSERT_TRUE(verifier.has_value()) << verifier.error();
        const auto verified = verifier->verify("pkg-2026", AsynGyanis::Core::Digest::sha256Hex(manifest), signature);
        EXPECT_TRUE(verified.has_value()) << verified.error();

        const std::string tampered = manifest + " ";
        const auto        rejected = verifier->verify("pkg-2026", AsynGyanis::Core::Digest::sha256Hex(tampered), signature);
        EXPECT_FALSE(rejected.has_value()) << "篡改后的 manifest 必须验签失败";

        // key id 与 manifest 字段不一致时脚本必须拒绝
        const auto mismatched = runProcess(AsynGyanis::Platform::FileSystem::utf8FromPath(bashPath()),
                                           {AsynGyanis::Platform::FileSystem::utf8FromPath(script), AsynGyanis::Platform::FileSystem::utf8FromPath(keyPath),
                                            AsynGyanis::Platform::FileSystem::utf8FromPath(manifestPath), "another-key"},
                                           std::chrono::seconds{30});
        EXPECT_FALSE(mismatched.succeeded()) << "key id 与 manifest 不一致时必须拒绝签名";
    }

    TEST(AgentPackage, PackageScriptSignsAndIncludesChecksums)
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

        const auto script   = std::filesystem::path{EDGEPULSE_SOURCE_DIR} / "scripts" / "package-agent.sh";
        const auto binary   = std::filesystem::path{EDGEPULSE_AGENT_TEST_HOOK_PATH};
        const auto output   = directory.path() / "output";
        ASSERT_TRUE(std::filesystem::exists(script)) << script;
        ASSERT_TRUE(std::filesystem::exists(binary)) << binary;

        const auto packaged = runProcess(AsynGyanis::Platform::FileSystem::utf8FromPath(bashPath()),
                                         {AsynGyanis::Platform::FileSystem::utf8FromPath(script), "--version", "9.9.9", "--output",
                                          AsynGyanis::Platform::FileSystem::utf8FromPath(output), "--binary", AsynGyanis::Platform::FileSystem::utf8FromPath(binary),
                                          "--signing-key", AsynGyanis::Platform::FileSystem::utf8FromPath(keyPath), "--signing-key-id", "pkg-2026"},
                                         std::chrono::seconds{120});
        ASSERT_TRUE(packaged.succeeded()) << packaged.failure << packaged.output;

        std::vector<std::filesystem::path> archives;
        for (const auto &entry: std::filesystem::directory_iterator(output))
        {
            if (entry.path().extension() == ".gz")
            {
                archives.push_back(entry.path());
            }
        }
        ASSERT_EQ(1U, archives.size()) << "打包脚本应产出一个 tar.gz";
        const std::filesystem::path archive = archives.front();
        EXPECT_NE(std::string::npos, archive.filename().string().find("9.9.9"));

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
        ASSERT_TRUE(std::filesystem::exists(signaturePath)) << "签名包必须带 manifest.sig";
        ASSERT_TRUE(std::filesystem::exists(checksumsPath)) << "签名包必须带 checksums.sha256";

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

        const auto verifier = Firmware::FirmwareSignatureVerifier::loadFromDirectory(keyDir);
        ASSERT_TRUE(verifier.has_value()) << verifier.error();
        const auto verified = verifier->verify("pkg-2026", AsynGyanis::Core::Digest::sha256Hex(manifest), readTextFile(signaturePath));
        EXPECT_TRUE(verified.has_value()) << verified.error();

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
    }
} // namespace EdgePulse::Agent
