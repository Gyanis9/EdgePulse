#include "AgentClient.h"
#include "AgentConfiguration.h"
#include "AgentIdentity.h"
#include "AgentLoop.h"
#include "AgentState.h"
#include "Executors/FirmwareExecutor.h"
#include "FirmwareKeyFixture.h"
#include "TestSupport/HttpClientHarness.h"
#include "TestSupport/TemporaryDirectory.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Runtime/Environment.h"
#include "Core/Coroutine/Task.h"
#include "Core/Crypto/Digest.h"
#include "Core/EventLoop/EventLoop.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef EDGEPULSE_AGENT_TEST_HOOK_PATH
#error "用例需要 EDGEPULSE_AGENT_TEST_HOOK_PATH 指向测试钩子可执行文件"
#endif

namespace EdgePulse::Agent
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;
        using AsynGyanis::Core::EventLoop;
        using AsynGyanis::Core::Task;

        /**
         * @brief 在用例存续期间设置环境变量，析构时恢复原值
         */
        class ScopedEnvironmentVariable
        {
        public:
            ScopedEnvironmentVariable(std::string name, std::string value) : m_name(std::move(name)), m_previous(Common::environmentValue(m_name))
            {
                assign(value);
            }

            ~ScopedEnvironmentVariable()
            {
                if (m_previous.has_value())
                {
                    assign(*m_previous);
                } else
                {
                    remove();
                }
            }

            ScopedEnvironmentVariable(const ScopedEnvironmentVariable &)            = delete;
            ScopedEnvironmentVariable &operator=(const ScopedEnvironmentVariable &) = delete;

        private:
            void assign(const std::string &value) const
            {
#if defined(_WIN32)
                _putenv_s(m_name.c_str(), value.c_str());
#else
                ::setenv(m_name.c_str(), value.c_str(), 1);
#endif
            }

            void remove() const
            {
#if defined(_WIN32)
                _putenv_s(m_name.c_str(), "");
#else
                ::unsetenv(m_name.c_str());
#endif
            }

            std::string                m_name;
            std::optional<std::string> m_previous;
        };

        /**
         * @brief 回环服务器侧的固件正文与状态记录
         */
        struct FirmwareServerState
        {
            std::mutex               mutex;       ///< 保护 body 与 statusBodies
            std::string              body;        ///< 下载路由返回的正文
            std::vector<std::string> statusBodies; ///< 收到的状态上报正文
            std::atomic<int>         status{200}; ///< 下载路由返回的状态码
            std::atomic<int>         downloads{0}; ///< 已收到的下载请求次数
        };

        /**
         * @brief 一条状态上报的解析结果
         */
        struct FirmwareStatusReport
        {
            std::string  status;             ///< 状态文本
            int          progress{-1};       ///< 进度百分比
            std::int64_t nodeId{0};          ///< 节点标识
            std::string  message;            ///< 中文说明
        };

        [[nodiscard]] AgentIdentity testIdentity()
        {
            AgentIdentity identity;
            identity.fingerprint = std::string(64, 'a');
            identity.expiresAt   = "2030-01-01T00:00:00Z";
            return identity;
        }

        [[nodiscard]] std::string readTextFile(const std::filesystem::path &path)
        {
            std::ifstream stream(path, std::ios::binary);
            return std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        }

        [[nodiscard]] std::vector<std::string> splitLines(const std::string &text)
        {
            std::vector<std::string> lines;
            std::string              current;
            for (const char character: text)
            {
                if (character == '\n')
                {
                    if (!current.empty() && current.back() == '\r')
                    {
                        current.pop_back();
                    }
                    lines.push_back(current);
                    current.clear();
                    continue;
                }
                current.push_back(character);
            }
            if (!current.empty())
            {
                lines.push_back(current);
            }
            return lines;
        }

        /**
         * @brief 固件命令正文的构造器
         * @param digest 摘要文本，原样写入正文
         * @param sizeBytes 正文声明的字节数
         * @param signature Base64 签名
         * @param keyId 签名公钥名
         * @param algorithm 签名算法
         * @return ConfigValue 命令正文对象
         */
        [[nodiscard]] ConfigValue firmwarePayload(const std::string &digest, const std::int64_t sizeBytes, const std::string &signature, const std::string &keyId = "prod-2026",
                                                  const std::string &algorithm = "ed25519-sha256")
        {
            return ConfigValue::object({{"firmwareId", 7},
                                        {"name", "EdgePulse OS"},
                                        {"version", "2.0.0"},
                                        {"sha256", digest},
                                        {"sizeBytes", sizeBytes},
                                        {"signature", signature},
                                        {"signingKeyId", keyId},
                                        {"signatureAlgorithm", algorithm},
                                        {"downloadPath", "/api/agent/firmware/7/download?nodeId=77"}});
        }

        /**
         * @brief 装配回环服务器、状态库与固件执行器的用例夹具
         */
        class FirmwareHarness
        {
        public:
            explicit FirmwareHarness(std::string servedBody) : m_servedBody(std::move(servedBody))
            {
                m_serverState->body = m_servedBody;
                m_server = std::make_unique<TestSupport::ServerHarness>(
                        [state = m_serverState](AsynGyanis::Net::Router &router, EventLoop &)
                        {
                            router.get("/api/agent/firmware/7/download",
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
                                               response.setBody(R"({"code":500,"message":"固件下载失败","data":null,"traceId":"trace-firmware"})");
                                           }
                                           co_return;
                                       });
                            router.post("/api/agent/firmware/:id/status",
                                        [state](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> Task<>
                                        {
                                            {
                                                const std::lock_guard lock(state->mutex);
                                                state->statusBodies.push_back(std::string{request.body()});
                                            }
                                            response.setStatus(200);
                                            response.setBody(R"({"code":200,"message":"ok","data":{},"traceId":"trace-firmware"})");
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
                m_state->set("node_id", "77");

                m_configuration.baseUrl                = "http://127.0.0.1:" + std::to_string(m_server->port());
                m_configuration.allowPlaintextForTests = true;
                m_configuration.name                   = "edge-01";
                m_configuration.tenantCode             = "tenant-a";
                m_configuration.requestTimeoutSeconds  = 10;
                m_configuration.firmwareTrustedKeysDirectory = m_directory.path() / "keys";
                m_configuration.firmwareStagingDirectory     = m_directory.path() / "staging";
                m_configuration.firmwareInstallHook          = std::filesystem::path{EDGEPULSE_AGENT_TEST_HOOK_PATH};
            }

            FirmwareHarness(const FirmwareHarness &)            = delete;
            FirmwareHarness &operator=(const FirmwareHarness &) = delete;

            [[nodiscard]] const std::filesystem::path &directory() const noexcept
            {
                return m_directory.path();
            }

            [[nodiscard]] std::filesystem::path trustedKeysDirectory() const
            {
                return m_configuration.firmwareTrustedKeysDirectory;
            }

            [[nodiscard]] std::filesystem::path stagingDirectory() const
            {
                return m_configuration.firmwareStagingDirectory;
            }

            [[nodiscard]] std::filesystem::path hookMarkerPath() const
            {
                return m_directory.path() / "hook-marker.txt";
            }

            [[nodiscard]] std::uint16_t port() const noexcept
            {
                return m_server->port();
            }

            /**
             * @brief 设置下载路由下一次返回的状态码
             * @param status HTTP 状态码
             */
            void setDownloadStatus(const int status) const
            {
                m_serverState->status.store(status, std::memory_order_release);
            }

            /**
             * @brief 在独立事件循环上执行一次固件命令
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
                AgentLoop     loop;
                AgentClient   client(loop.loop(), m_configuration, testIdentity());
                FirmwareExecutor executor(client, *m_state, m_configuration, 77);
                LoopStopGuard stopGuard{loop};

                const auto outcome = loop.run(executor.execute(loop.loop(), AgentCommand{1, "firmware.update", payload, 300}), std::chrono::seconds{60}, "固件更新");
                if (!outcome.has_value())
                {
                    return CommandResult{false, "{}", "固件更新未在时限内完成"};
                }
                return *outcome;
            }

            [[nodiscard]] std::vector<FirmwareStatusReport> statusReports() const
            {
                std::vector<std::string> bodies;
                {
                    const std::lock_guard lock(m_serverState->mutex);
                    bodies = m_serverState->statusBodies;
                }
                std::vector<FirmwareStatusReport> reports;
                reports.reserve(bodies.size());
                for (const std::string &body: bodies)
                {
                    FirmwareStatusReport report;
                    const auto           parsed = AsynGyanis::Base::parseConfigValue(body);
                    if (parsed.has_value() && parsed->is_object())
                    {
                        report.status   = parsed->value("status", std::string{});
                        report.progress = parsed->value("progress", -1);
                        report.nodeId   = parsed->value("nodeId", static_cast<std::int64_t>(0));
                        report.message  = parsed->value("message", std::string{});
                    }
                    reports.push_back(std::move(report));
                }
                return reports;
            }

            [[nodiscard]] int downloads() const
            {
                return m_serverState->downloads.load(std::memory_order_acquire);
            }

            [[nodiscard]] std::optional<std::string> installedVersion() const
            {
                return m_state == nullptr ? std::nullopt : m_state->get("installed_firmware_version");
            }

            [[nodiscard]] bool stagingEmpty() const
            {
                std::error_code filesystemError;
                const bool      exists = std::filesystem::exists(m_configuration.firmwareStagingDirectory, filesystemError);
                if (filesystemError)
                {
                    return false;
                }
                if (!exists)
                {
                    return true;
                }
                const bool empty = std::filesystem::is_empty(m_configuration.firmwareStagingDirectory, filesystemError);
                return filesystemError ? false : empty;
            }

        private:
            TestSupport::TemporaryDirectory      m_directory;   ///< 用例独占的临时目录
            std::shared_ptr<FirmwareServerState> m_serverState = std::make_shared<FirmwareServerState>(); ///< 服务端记录
            std::unique_ptr<TestSupport::ServerHarness> m_server; ///< 回环服务器
            std::unique_ptr<AgentState>          m_state;       ///< 真实状态库
            AgentConfiguration                   m_configuration; ///< 执行器配置
            std::string                          m_servedBody;  ///< 下载正文副本
        };
    } // namespace

    TEST(AgentFirmware, DownloadsVerifiesInstallsAndReportsStatuses)
    {
        const std::string firmwareBytes = "EDGEPULSE-FIRMWARE-" + std::string(4096, 'x');
        FirmwareHarness   harness(firmwareBytes);
        const auto        key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "prod-2026");
        ASSERT_TRUE(key.has_value()) << key.error();

        const std::string digest = AsynGyanis::Core::Digest::sha256Hex(firmwareBytes);
        std::string       upperDigest = digest;
        for (char &character: upperDigest)
        {
            if (character >= 'a' && character <= 'f')
            {
                character = static_cast<char>(character - 'a' + 'A');
            }
        }
        const std::string payload = firmwarePayload(upperDigest, static_cast<std::int64_t>(firmwareBytes.size()), key->signDigest(digest)).dump();

        const ScopedEnvironmentVariable marker("EDGEPULSE_AGENT_TEST_HOOK_MARKER", harness.hookMarkerPath().string());
        const CommandResult             result = harness.execute(payload);
        ASSERT_TRUE(result.succeeded) << result.message;

        const auto parsedResult = AsynGyanis::Base::parseConfigValue(result.result);
        ASSERT_TRUE(parsedResult.has_value()) << result.result;
        EXPECT_EQ(7, parsedResult->value("firmwareId", 0));
        EXPECT_EQ("2.0.0", parsedResult->value("version", std::string{}));
        EXPECT_EQ(digest, parsedResult->value("sha256", std::string{})) << "正文里的摘要应为小写";
        EXPECT_TRUE(parsedResult->value("installed", false));

        ASSERT_EQ(1, harness.downloads());
        const std::vector<std::string> markerLines = splitLines(readTextFile(harness.hookMarkerPath()));
        ASSERT_EQ(3U, markerLines.size()) << "安装钩子必须收到文件、版本与摘要三个参数";
        EXPECT_NE(std::string::npos, markerLines[0].find(harness.stagingDirectory().string()));
        EXPECT_FALSE(std::filesystem::exists(markerLines[0])) << "安装成功后暂存文件必须删除";
        EXPECT_EQ("2.0.0", markerLines[1]);
        EXPECT_EQ(digest, markerLines[2]);

        const std::vector<FirmwareStatusReport> reports = harness.statusReports();
        ASSERT_EQ(3U, reports.size());
        EXPECT_EQ("downloading", reports[0].status);
        EXPECT_EQ(0, reports[0].progress);
        EXPECT_EQ("installing", reports[1].status);
        EXPECT_EQ(100, reports[1].progress);
        EXPECT_EQ("installed", reports[2].status);
        EXPECT_EQ(100, reports[2].progress);
        for (const FirmwareStatusReport &report: reports)
        {
            EXPECT_EQ(77, report.nodeId);
            EXPECT_FALSE(report.message.empty());
        }

        ASSERT_TRUE(harness.installedVersion().has_value());
        EXPECT_EQ("2.0.0", *harness.installedVersion());
        EXPECT_TRUE(harness.stagingEmpty());
    }

    TEST(AgentFirmware, TamperedBytesFailBeforeInstallHook)
    {
        const std::string firmwareBytes = "EDGEPULSE-FIRMWARE-" + std::string(4096, 'x');
        FirmwareHarness   harness(std::string(firmwareBytes.size(), 'y'));
        const auto        key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "prod-2026");
        ASSERT_TRUE(key.has_value()) << key.error();

        const std::string digest  = AsynGyanis::Core::Digest::sha256Hex(firmwareBytes);
        const std::string payload = firmwarePayload(digest, static_cast<std::int64_t>(firmwareBytes.size()), key->signDigest(digest)).dump();

        const ScopedEnvironmentVariable marker("EDGEPULSE_AGENT_TEST_HOOK_MARKER", harness.hookMarkerPath().string());
        const CommandResult             result = harness.execute(payload);
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("SHA-256")) << result.message;

        EXPECT_FALSE(std::filesystem::exists(harness.hookMarkerPath())) << "校验失败不得调用安装钩子";
        const std::vector<FirmwareStatusReport> reports = harness.statusReports();
        ASSERT_FALSE(reports.empty());
        EXPECT_EQ("failed", reports.back().status);
        EXPECT_FALSE(harness.installedVersion().has_value());
        EXPECT_TRUE(harness.stagingEmpty());
    }

    TEST(AgentFirmware, UnknownKeyIdFailsBeforeInstallHook)
    {
        const std::string firmwareBytes = "EDGEPULSE-FIRMWARE-" + std::string(2048, 'q');
        FirmwareHarness   harness(firmwareBytes);
        const auto        key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "prod-2026");
        ASSERT_TRUE(key.has_value()) << key.error();

        const std::string digest  = AsynGyanis::Core::Digest::sha256Hex(firmwareBytes);
        const std::string payload = firmwarePayload(digest, static_cast<std::int64_t>(firmwareBytes.size()), key->signDigest(digest), "missing-key").dump();

        const ScopedEnvironmentVariable marker("EDGEPULSE_AGENT_TEST_HOOK_MARKER", harness.hookMarkerPath().string());
        const CommandResult             result = harness.execute(payload);
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("签名")) << result.message;
        EXPECT_FALSE(std::filesystem::exists(harness.hookMarkerPath()));
        EXPECT_FALSE(harness.installedVersion().has_value());
        EXPECT_TRUE(harness.stagingEmpty());
    }

    TEST(AgentFirmware, UnsignedOrWrongAlgorithmFailsBeforeDownload)
    {
        const std::string firmwareBytes = "EDGEPULSE-FIRMWARE-" + std::string(1024, 'u');
        FirmwareHarness   harness(firmwareBytes);
        const auto        key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "prod-2026");
        ASSERT_TRUE(key.has_value()) << key.error();

        const std::string digest = AsynGyanis::Core::Digest::sha256Hex(firmwareBytes);
        const ScopedEnvironmentVariable marker("EDGEPULSE_AGENT_TEST_HOOK_MARKER", harness.hookMarkerPath().string());

        const std::string unsignedPayload = firmwarePayload(digest, static_cast<std::int64_t>(firmwareBytes.size()), "").dump();
        const CommandResult unsignedResult = harness.execute(unsignedPayload);
        EXPECT_FALSE(unsignedResult.succeeded);
        EXPECT_NE(std::string::npos, unsignedResult.message.find("signature")) << unsignedResult.message;
        const std::vector<FirmwareStatusReport> unsignedReports = harness.statusReports();
        ASSERT_EQ(1U, unsignedReports.size()) << "正文校验失败也必须上报 failed";
        EXPECT_EQ("failed", unsignedReports[0].status);
        EXPECT_EQ(100, unsignedReports[0].progress);
        EXPECT_EQ(77, unsignedReports[0].nodeId);
        EXPECT_NE(std::string::npos, unsignedReports[0].message.find("signature")) << unsignedReports[0].message;

        const std::string algorithmPayload =
                firmwarePayload(digest, static_cast<std::int64_t>(firmwareBytes.size()), key->signDigest(digest), "prod-2026", "rsa-sha256").dump();
        const CommandResult algorithmResult = harness.execute(algorithmPayload);
        EXPECT_FALSE(algorithmResult.succeeded);
        EXPECT_NE(std::string::npos, algorithmResult.message.find("ed25519-sha256")) << algorithmResult.message;
        const std::vector<FirmwareStatusReport> algorithmReports = harness.statusReports();
        ASSERT_EQ(2U, algorithmReports.size()) << "每次正文校验失败都各上报一条 failed";
        EXPECT_EQ("failed", algorithmReports[1].status);
        EXPECT_EQ(100, algorithmReports[1].progress);
        EXPECT_EQ(77, algorithmReports[1].nodeId);
        EXPECT_NE(std::string::npos, algorithmReports[1].message.find("ed25519-sha256")) << algorithmReports[1].message;

        EXPECT_EQ(0, harness.downloads()) << "正文不合法时不得发起下载";
        EXPECT_FALSE(std::filesystem::exists(harness.hookMarkerPath()));
        EXPECT_FALSE(harness.installedVersion().has_value());
    }

    TEST(AgentFirmware, SizeMismatchFailsBeforeInstallHook)
    {
        const std::string firmwareBytes = "EDGEPULSE-FIRMWARE-" + std::string(2048, 's');

        {
            FirmwareHarness harness(firmwareBytes);
            const auto      signingKey = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "prod-2026");
            ASSERT_TRUE(signingKey.has_value()) << signingKey.error();
            const std::string digest  = AsynGyanis::Core::Digest::sha256Hex(firmwareBytes);
            const std::string payload = firmwarePayload(digest, static_cast<std::int64_t>(firmwareBytes.size()) + 1, signingKey->signDigest(digest)).dump();

            const ScopedEnvironmentVariable marker("EDGEPULSE_AGENT_TEST_HOOK_MARKER", harness.hookMarkerPath().string());
            const CommandResult             result = harness.execute(payload);
            EXPECT_FALSE(result.succeeded);
            EXPECT_NE(std::string::npos, result.message.find("大小不符")) << result.message;
            EXPECT_FALSE(std::filesystem::exists(harness.hookMarkerPath()));
            EXPECT_TRUE(harness.stagingEmpty());
        }

        {
            // 正文超过命令声明的字节数时，接收口必须当场中止，剩下的字节不参与摘要
            FirmwareHarness harness(firmwareBytes + std::string(64, 'o'));
            const auto      signingKey = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "prod-2026");
            ASSERT_TRUE(signingKey.has_value()) << signingKey.error();
            const std::string digest  = AsynGyanis::Core::Digest::sha256Hex(firmwareBytes);
            const std::string payload = firmwarePayload(digest, static_cast<std::int64_t>(firmwareBytes.size()), signingKey->signDigest(digest)).dump();

            const ScopedEnvironmentVariable marker("EDGEPULSE_AGENT_TEST_HOOK_MARKER", harness.hookMarkerPath().string());
            const CommandResult             result = harness.execute(payload);
            EXPECT_FALSE(result.succeeded);
            EXPECT_NE(std::string::npos, result.message.find("超过")) << result.message;
            EXPECT_FALSE(std::filesystem::exists(harness.hookMarkerPath()));
            EXPECT_TRUE(harness.stagingEmpty());
        }
    }

    TEST(AgentFirmware, HookFailureKeepsInstalledVersionUnchanged)
    {
        const std::string firmwareBytes = "EDGEPULSE-FIRMWARE-" + std::string(1024, 'h');
        FirmwareHarness   harness(firmwareBytes);
        const auto        key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "prod-2026");
        ASSERT_TRUE(key.has_value()) << key.error();

        const std::string digest  = AsynGyanis::Core::Digest::sha256Hex(firmwareBytes);
        const std::string payload = firmwarePayload(digest, static_cast<std::int64_t>(firmwareBytes.size()), key->signDigest(digest)).dump();

        const ScopedEnvironmentVariable marker("EDGEPULSE_AGENT_TEST_HOOK_MARKER", harness.hookMarkerPath().string());
        const ScopedEnvironmentVariable exitCode("EDGEPULSE_AGENT_TEST_HOOK_EXIT", "7");
        const CommandResult             result = harness.execute(payload);
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("安装钩子")) << result.message;

        EXPECT_TRUE(std::filesystem::exists(harness.hookMarkerPath())) << "钩子应当已被调用";
        const std::vector<FirmwareStatusReport> reports = harness.statusReports();
        ASSERT_FALSE(reports.empty());
        EXPECT_EQ("failed", reports.back().status);
        EXPECT_FALSE(harness.installedVersion().has_value());
        EXPECT_TRUE(harness.stagingEmpty());
    }

    TEST(AgentFirmware, DownloadHttpFailureFailsBeforeInstallHook)
    {
        const std::string firmwareBytes = "EDGEPULSE-FIRMWARE-" + std::string(512, 'd');
        const std::string digest        = AsynGyanis::Core::Digest::sha256Hex(firmwareBytes);

        for (const int failingStatus: {404, 500})
        {
            FirmwareHarness harness(firmwareBytes);
            harness.setDownloadStatus(failingStatus);
            const auto key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "prod-2026");
            ASSERT_TRUE(key.has_value()) << key.error();
            const std::string payload = firmwarePayload(digest, static_cast<std::int64_t>(firmwareBytes.size()), key->signDigest(digest)).dump();

            const ScopedEnvironmentVariable marker("EDGEPULSE_AGENT_TEST_HOOK_MARKER", harness.hookMarkerPath().string());
            const CommandResult             result = harness.execute(payload);
            EXPECT_FALSE(result.succeeded);
            EXPECT_NE(std::string::npos, result.message.find("HTTP " + std::to_string(failingStatus))) << result.message;
            EXPECT_EQ(1, harness.downloads());

            EXPECT_FALSE(std::filesystem::exists(harness.hookMarkerPath()));
            const std::vector<FirmwareStatusReport> reports = harness.statusReports();
            ASSERT_FALSE(reports.empty());
            EXPECT_EQ("failed", reports.back().status);
            EXPECT_FALSE(harness.installedVersion().has_value());
            EXPECT_TRUE(harness.stagingEmpty());
        }
    }

    TEST(AgentFirmware, RejectsUnsupportedDownloadSchemeBeforeRequest)
    {
        const std::string firmwareBytes = "EDGEPULSE-FIRMWARE-" + std::string(256, 'e');
        FirmwareHarness   harness(firmwareBytes);
        const auto        key = Testing::FirmwareKeyFixture::generate(harness.trustedKeysDirectory(), "prod-2026");
        ASSERT_TRUE(key.has_value()) << key.error();

        const std::string digest = AsynGyanis::Core::Digest::sha256Hex(firmwareBytes);
        ConfigValue       body   = firmwarePayload(digest, static_cast<std::int64_t>(firmwareBytes.size()), key->signDigest(digest));
        body["downloadPath"]     = "ftp://127.0.0.1/firmware.bin";

        const ScopedEnvironmentVariable marker("EDGEPULSE_AGENT_TEST_HOOK_MARKER", harness.hookMarkerPath().string());
        const CommandResult             result = harness.execute(body.dump());
        EXPECT_FALSE(result.succeeded);
        EXPECT_NE(std::string::npos, result.message.find("协议")) << result.message;
        EXPECT_EQ(0, harness.downloads());
        EXPECT_FALSE(std::filesystem::exists(harness.hookMarkerPath()));
    }
} // namespace EdgePulse::Agent
