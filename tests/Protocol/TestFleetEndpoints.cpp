#include "Base/Config/ConfigValue.h"
#include "Base/Coding/Base64.h"
#include "Base/Database/Database.h"
#include "Base/Runtime/TimeFormat.h"
#include "Base/Security/PasswordHash.h"
#include "Core/Crypto/Digest.h"
#include "Core/Alarm/AlarmRepository.h"
#include "Core/Alarm/AlarmService.h"
#include "Core/Auth/AuthService.h"
#include "Core/Command/AgentChannelRegistry.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/AgentRegistry.h"
#include "Core/Device/NodeConfigRepository.h"
#include "Core/Device/NodeConfigService.h"
#include "Core/Device/NodeGroupRepository.h"
#include "Core/Device/NodeLifecycleService.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Device/NodeService.h"
#include "Core/Firmware/FirmwareRepository.h"
#include "Core/Firmware/FirmwareSignatureVerifier.h"
#include "Core/Firmware/FirmwareSigningPolicy.h"
#include "Core/Http/AgentWebSocket.h"
#include "Core/Http/Authorization.h"
#include "Core/Http/MiddlewareSetup.h"
#include "Core/Http/WebSocketEndpoints.h"
#include "Core/System/LogRepository.h"
#include "Core/System/TenantRepository.h"
#include "Core/Telemetry/TelemetryRepository.h"
#include "Plugins/AlarmPlugin/AlarmController.h"
#include "Plugins/AuthPlugin/AuthController.h"
#include "Plugins/CommandPlugin/CommandController.h"
#include "Plugins/DevicePlugin/AgentController.h"
#include "Plugins/DevicePlugin/DeviceController.h"
#include "Plugins/DevicePlugin/NodeConfigController.h"
#include "Plugins/DevicePlugin/NodeGroupController.h"
#include "Plugins/FirmwarePlugin/FirmwareController.h"
#include "Plugins/TelemetryPlugin/TelemetryController.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"
#include "TestSupport/HttpClientHarness.h"

#include <Net/Http/Client/HttpClient.h>
#include <Net/WebSocket/WebSocketClient.h>

#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace
{
    std::vector<std::pair<std::string, std::string>> bearer(const std::string &token)
    {
        return {{"Authorization", "Bearer " + token}};
    }

    [[nodiscard]] std::int64_t countFirmwareTasks(const std::filesystem::path &databasePath, const std::int64_t firmwareId)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
        const std::unique_ptr<AsynGyanis::Database::DatabaseResult>     result = connection->execute("SELECT COUNT(*) FROM biz_firmware_task WHERE firmware_id = " + std::to_string(firmwareId));
        EXPECT_NE(nullptr, result) << connection->lastError();
        if (result == nullptr || !result->next())
        {
            return -1;
        }
        const auto value = result->getValue(0);
        return std::holds_alternative<std::int64_t>(value) ? std::get<std::int64_t>(value) : -1;
    }

    void insertFirmwareTask(const std::filesystem::path &databasePath, const std::int64_t taskId, const std::int64_t tenantId, const std::int64_t firmwareId,
                            const std::int64_t nodeId)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
        const std::string now  = EdgePulse::Common::nowDateTimeUtc();
        const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection->execute(
                "INSERT INTO biz_firmware_task (id, tenant_id, firmware_id, node_id, status, progress, message, created_at, updated_at, previous_firmware_id) VALUES (" +
                std::to_string(taskId) + ", " + std::to_string(tenantId) + ", " + std::to_string(firmwareId) + ", " + std::to_string(nodeId) + ", 'pending', 0, '', '" + now +
                "', '" + now + "', 0)");
        EXPECT_NE(nullptr, result) << connection->lastError();
    }

    using PkeyPointer = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

    struct SigningKeyPair
    {
        PkeyPointer privateKey{nullptr, &EVP_PKEY_free}; ///< 签名私钥
        PkeyPointer publicKey{nullptr, &EVP_PKEY_free};  ///< 写入信任键目录的公钥
    };

    [[nodiscard]] SigningKeyPair generateSigningKey()
    {
        SigningKeyPair pair;
        EVP_PKEY_CTX  *context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        if (context == nullptr)
        {
            return pair;
        }
        EVP_PKEY_keygen_init(context);
        EVP_PKEY *key = nullptr;
        EVP_PKEY_keygen(context, &key);
        EVP_PKEY_CTX_free(context);
        pair.privateKey.reset(key);
        pair.publicKey.reset(key != nullptr ? EVP_PKEY_dup(key) : nullptr);
        return pair;
    }

    void writePublicKey(const std::filesystem::path &path, EVP_PKEY *key)
    {
        BIO *bio = BIO_new_file(path.string().c_str(), "w");
        if (bio == nullptr)
        {
            return;
        }
        PEM_write_bio_PUBKEY(bio, key);
        BIO_free(bio);
    }

    [[nodiscard]] std::string signFirmwareDigest(EVP_PKEY *privateKey, const std::string &digestHex)
    {
        if (privateKey == nullptr || digestHex.size() != 64)
        {
            return {};
        }
        std::array<unsigned char, 32> digest{};
        for (std::size_t index = 0; index < digest.size(); ++index)
        {
            digest[index] = static_cast<unsigned char>(std::stoul(digestHex.substr(index * 2, 2), nullptr, 16));
        }
        EVP_MD_CTX *context = EVP_MD_CTX_new();
        if (context == nullptr)
        {
            return {};
        }
        std::string signature;
        if (EVP_DigestSignInit(context, nullptr, nullptr, nullptr, privateKey) == 1)
        {
            std::size_t signatureLength = 0;
            if (EVP_DigestSign(context, nullptr, &signatureLength, digest.data(), digest.size()) == 1)
            {
                signature.assign(signatureLength, '\0');
                if (EVP_DigestSign(context, reinterpret_cast<unsigned char *>(signature.data()), &signatureLength, digest.data(), digest.size()) != 1)
                {
                    signature.clear();
                } else
                {
                    signature.resize(signatureLength);
                }
            }
        }
        EVP_MD_CTX_free(context);
        return AsynGyanis::Base::base64Encode(signature);
    }

    struct WebSocketProbeResult
    {
        bool        connected{false}; ///< 升级与 hello 都成功
        std::string commandPayload;   ///< 服务端推送的命令原文
        std::string error;            ///< 失败原因
    };

    AsynGyanis::Core::Task<> performWebSocketProbe(AsynGyanis::Core::EventLoop &loop, std::uint16_t port, std::string token, std::int64_t nodeId,
                                                   std::shared_ptr<std::promise<WebSocketProbeResult>> completion)
    {
        WebSocketProbeResult result;
        try
        {
            AsynGyanis::Net::WebSocketClient::Configuration configuration;
            configuration.hostName      = "127.0.0.1";
            configuration.port          = port;
            configuration.requestTarget = "/api/agent/ws";
            auto connection             = co_await AsynGyanis::Net::WebSocketClient::connect(loop, configuration);
            if (!connection)
            {
                result.error = "升级失败：" + connection.error().message;
                completion->set_value(std::move(result));
                co_return;
            }
            std::unique_ptr<AsynGyanis::Net::WebSocketClient> client = std::move(*connection);

            const AsynGyanis::Base::ConfigValue hello = AsynGyanis::Base::ConfigValue::object({{"type", "hello"}, {"nodeId", nodeId}, {"agentVersion", "protocol-test"}});
            if (!co_await client->sendText(hello.dump()))
            {
                result.error = "hello 发送失败";
                completion->set_value(std::move(result));
                co_return;
            }
            const auto reply = co_await client->receive();
            if (!reply || reply->opCode != AsynGyanis::Net::WebSocketOpCode::Text || reply->payload.find("\"ok\":true") == std::string::npos)
            {
                result.error = "未收到 hello 确认";
                completion->set_value(std::move(result));
                co_return;
            }
            result.connected = true;

            AsynGyanis::Net::HttpClientRequest request;
            request.method      = "POST";
            request.contentType = "application/json";
            request.headers     = bearer(token);
            request.body        = AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"type", "ws-echo"}, {"payload", "{}"}, {"timeoutSeconds", 30}}).dump();
            const auto created  = co_await AsynGyanis::Net::HttpClient::send(loop, "http://127.0.0.1:" + std::to_string(port) + "/api/commands", request, std::chrono::seconds{5});
            if (!created || created->statusCode != 200)
            {
                result.error = "命令创建失败";
                completion->set_value(std::move(result));
                co_return;
            }

            const auto frame = co_await client->receive();
            if (!frame || frame->opCode != AsynGyanis::Net::WebSocketOpCode::Text)
            {
                result.error = "未收到推送的命令";
                completion->set_value(std::move(result));
                co_return;
            }
            result.commandPayload = frame->payload;

            const auto value = AsynGyanis::Base::parseConfigValue(frame->payload);
            if (!value.has_value())
            {
                result.error = "命令推送不是 JSON";
                completion->set_value(std::move(result));
                co_return;
            }
            const auto command = value->find("command");
            if (command == value->end())
            {
                result.error = "命令推送缺少 command";
                completion->set_value(std::move(result));
                co_return;
            }
            const auto identifier = command->find("id");
            if (identifier == command->end() || !identifier->is_number_integer())
            {
                result.error = "命令推送缺少 command.id";
                completion->set_value(std::move(result));
                co_return;
            }
            const AsynGyanis::Base::ConfigValue acknowledgement = AsynGyanis::Base::ConfigValue::object(
                    {{"type", "command.result"}, {"commandId", identifier->get<std::int64_t>()}, {"status", "succeeded"}, {"result", R"({"ok":true})"}, {"message", "ws"}});
            static_cast<void>(co_await client->sendText(acknowledgement.dump()));
            static_cast<void>(co_await client->close(1000, "done"));
        } catch (const std::exception &failure)
        {
            result.error = failure.what();
        }
        completion->set_value(std::move(result));
        co_return;
    }

    struct AuthTimeoutProbeResult
    {
        bool          closed{false}; ///< 收到关闭帧
        std::uint16_t closeCode{0};  ///< 关闭帧状态码
        std::string   error;         ///< 失败原因
    };

    struct AgentCloseProbeResult
    {
        bool          connected{false}; ///< hello 握手成功
        bool          closed{false};    ///< 收到了服务端关闭帧
        std::uint16_t closeCode{0};     ///< 关闭帧状态码
        std::string   error;            ///< 失败原因
    };

    AsynGyanis::Core::Task<> probeAgentSocketUntilClosed(AsynGyanis::Core::EventLoop &loop, std::uint16_t port, std::int64_t nodeId,
                                                         std::shared_ptr<std::promise<AgentCloseProbeResult>> completion)
    {
        AgentCloseProbeResult result;
        try
        {
            AsynGyanis::Net::WebSocketClient::Configuration configuration;
            configuration.hostName      = "127.0.0.1";
            configuration.port          = port;
            configuration.requestTarget = "/api/agent/ws";
            auto connection             = co_await AsynGyanis::Net::WebSocketClient::connect(loop, configuration);
            if (!connection)
            {
                result.error = "升级失败：" + connection.error().message;
                completion->set_value(std::move(result));
                co_return;
            }
            std::unique_ptr<AsynGyanis::Net::WebSocketClient> client = std::move(*connection);

            const AsynGyanis::Base::ConfigValue hello = AsynGyanis::Base::ConfigValue::object({{"type", "hello"}, {"nodeId", nodeId}, {"agentVersion", "protocol-test"}});
            if (!co_await client->sendText(hello.dump()))
            {
                result.error = "hello 发送失败";
                completion->set_value(std::move(result));
                co_return;
            }
            const auto reply = co_await client->receive();
            if (!reply || reply->opCode != AsynGyanis::Net::WebSocketOpCode::Text || reply->payload.find("\"ok\":true") == std::string::npos)
            {
                result.error = "未收到 hello 确认";
                completion->set_value(std::move(result));
                co_return;
            }
            result.connected = true;

            // 服务端吊销证书后应主动以 1008 关闭；这里等到关闭帧或时限到点
            const auto frame = co_await client->receiveFor(std::chrono::seconds{10});
            if (!frame.has_value())
            {
                result.error = "未在时限内收到关闭帧";
            } else
            {
                result.closed = frame->opCode == AsynGyanis::Net::WebSocketOpCode::Close;
                if (result.closed && frame->payload.size() >= 2)
                {
                    result.closeCode = static_cast<std::uint16_t>((static_cast<std::uint8_t>(frame->payload[0]) << 8) | static_cast<std::uint8_t>(frame->payload[1]));
                }
            }
        } catch (const std::exception &failure)
        {
            result.error = failure.what();
        }
        completion->set_value(std::move(result));
        co_return;
    }

    AsynGyanis::Core::Task<> probeUnauthenticatedBrowserSocket(AsynGyanis::Core::EventLoop &loop, std::uint16_t port,
                                                               std::shared_ptr<std::promise<AuthTimeoutProbeResult>> completion)
    {
        AuthTimeoutProbeResult result;
        try
        {
            AsynGyanis::Net::WebSocketClient::Configuration configuration;
            configuration.hostName      = "127.0.0.1";
            configuration.port          = port;
            configuration.requestTarget = "/api/ws";
            auto connection             = co_await AsynGyanis::Net::WebSocketClient::connect(loop, configuration);
            if (!connection)
            {
                result.error = "升级失败：" + connection.error().message;
                completion->set_value(std::move(result));
                co_return;
            }
            std::unique_ptr<AsynGyanis::Net::WebSocketClient> client = std::move(*connection);
            const auto                                        frame  = co_await client->receiveFor(std::chrono::seconds{7});
            if (!frame.has_value())
            {
                result.error = "未在时限内收到关闭帧";
            } else
            {
                result.closed = frame->opCode == AsynGyanis::Net::WebSocketOpCode::Close;
                if (result.closed && frame->payload.size() >= 2)
                {
                    result.closeCode = static_cast<std::uint16_t>((static_cast<std::uint8_t>(frame->payload[0]) << 8) | static_cast<std::uint8_t>(frame->payload[1]));
                }
            }
        } catch (const std::exception &failure)
        {
            result.error = failure.what();
        }
        completion->set_value(std::move(result));
        co_return;
    }
} // namespace

class FleetEndpointsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_context          = std::make_unique<EdgePulse::TestSupport::AuthTestContext>();
        m_storage          = std::filesystem::temp_directory_path() / ("edgepulse-firmware-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        m_keyDirectory     = std::filesystem::path(m_storage.string() + "-keys");
        std::filesystem::create_directories(m_keyDirectory);
        m_signingKey = generateSigningKey();
        ASSERT_NE(nullptr, m_signingKey.privateKey.get());
        writePublicKey(m_keyDirectory / "protocol-key.pem", m_signingKey.publicKey.get());
        auto signingVerifier = EdgePulse::Firmware::FirmwareSignatureVerifier::loadFromDirectory(m_keyDirectory);
        ASSERT_TRUE(signingVerifier.has_value()) << signingVerifier.error();
        m_signingPolicy = std::make_shared<EdgePulse::Firmware::FirmwareSigningPolicy>(
                EdgePulse::Firmware::FirmwareSigningPolicy{false, std::make_shared<EdgePulse::Firmware::FirmwareSignatureVerifier>(std::move(*signingVerifier))});
        m_nodes            = std::make_shared<EdgePulse::Device::NodeRepository>(m_context->database);
        m_nodeService      = std::make_shared<EdgePulse::Device::NodeService>(m_nodes);
        m_groups           = std::make_shared<EdgePulse::Device::NodeGroupRepository>(m_context->database);
        m_telemetry        = std::make_shared<EdgePulse::Telemetry::TelemetryRepository>(m_context->database);
        m_alarmRepo        = std::make_shared<EdgePulse::Alarm::AlarmRepository>(m_context->database);
        m_alarmService     = std::make_shared<EdgePulse::Alarm::AlarmService>(m_alarmRepo);
        m_lifecycle        = std::make_unique<EdgePulse::Device::NodeLifecycleService>(
                *m_nodes, *m_alarmService, [](const EdgePulse::Device::NodeRecord &) -> AsynGyanis::Core::Task<> { co_return; }, 90);
        // 与 Application 一致：注册表把每次注册与心跳都交给生命周期，是否发布由它按 becameOnline 决定
        EdgePulse::Device::AgentRegistry::Options agentOptions;
        agentOptions.publisher = [this](const EdgePulse::Device::NodeRecord &node, const bool becameOnline) -> AsynGyanis::Core::Task<>
        { co_await m_lifecycle->onHeartbeat(m_server->loop(), node, becameOnline); };
        m_agentRegistry    = std::make_shared<EdgePulse::Device::AgentRegistry>(m_nodes, std::move(agentOptions));
        m_commands         = std::make_shared<EdgePulse::Command::CommandRepository>(m_context->database);
        m_channels         = std::make_shared<EdgePulse::Command::AgentChannelRegistry>();
        m_firmware         = std::make_shared<EdgePulse::Firmware::FirmwareRepository>(m_context->database);
        m_browserWebSocket = std::make_shared<EdgePulse::Web::BrowserWebSocket>();

        m_nodeConfigs       = std::make_shared<EdgePulse::Device::NodeConfigRepository>(m_context->database);
        m_nodeConfigService = std::make_shared<EdgePulse::Device::NodeConfigService>(m_nodeConfigs, m_nodes, m_commands, m_channels.get(), nullptr);

        m_server = std::make_unique<EdgePulse::TestSupport::ServerHarness>(
                [this](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                {
                    EdgePulse::Web::applySecurityHeaders(router);
                    EdgePulse::TestSupport::installTestAgentIdentity(router);
                    EdgePulse::Auth::registerRoutes(router, loop, *m_context->auth, *m_context->captcha, true);
                    const auto access = [this](AsynGyanis::Core::EventLoop  &accessLoop,
                                               AsynGyanis::Net::HttpRequest &request) -> AsynGyanis::Core::Task<std::optional<EdgePulse::Device::AccessContext>>
                    {
                        const auto token = EdgePulse::Web::bearerToken(request);
                        if (!token.has_value())
                        {
                            co_return std::nullopt;
                        }
                        const auto session = co_await m_context->auth->authenticate(accessLoop, *token);
                        if (!session.has_value())
                        {
                            co_return std::nullopt;
                        }
                        co_return EdgePulse::Device::AccessContext{session->userId,       session->tenantId, session->platformAdmin, session->username,
                                                                   session->departmentId, session->roles,    session->permissions};
                    };
                    EdgePulse::Device::NodeConfigDependencies configDependencies;
                    configDependencies.auth    = m_context->auth.get();
                    configDependencies.configs = m_nodeConfigs.get();
                    configDependencies.service = m_nodeConfigService.get();
                    configDependencies.nodes   = m_nodes.get();
                    EdgePulse::Device::registerNodeConfigRoutes(router, loop, configDependencies);
                    EdgePulse::Device::registerRoutes(router, loop, *m_nodeService, access, nullptr, m_context->tenants.get(), m_lifecycle.get(), m_channels.get(),
                                                      m_nodeConfigs.get());
                    EdgePulse::Device::registerRoutes(router, loop, *m_agentRegistry, *m_nodes, *m_context->tenants, 30, m_nodeConfigs.get());
                    EdgePulse::Device::registerNodeConfigAgentRoutes(router, loop, configDependencies);
                    EdgePulse::Device::NodeGroupDependencies groups;
                    groups.auth   = m_context->auth.get();
                    groups.groups = m_groups.get();
                    EdgePulse::Device::registerNodeGroupRoutes(router, loop, groups);

                    EdgePulse::Telemetry::TelemetryDependencies telemetry;
                    telemetry.auth      = m_context->auth.get();
                    telemetry.nodes     = m_nodes.get();
                    telemetry.telemetry = m_telemetry.get();
                    telemetry.alarms    = m_alarmService.get();
                    EdgePulse::Telemetry::registerTelemetryBrowserRoutes(router, loop, telemetry);
                    EdgePulse::Telemetry::registerTelemetryAgentRoutes(router, loop, telemetry);

                    EdgePulse::Alarm::AlarmDependencies alarms;
                    alarms.auth   = m_context->auth.get();
                    alarms.alarms = m_alarmService.get();
                    EdgePulse::Alarm::registerAlarmRoutes(router, loop, alarms);

                    EdgePulse::Command::CommandDependencies commands;
                    commands.auth     = m_context->auth.get();
                    commands.commands = m_commands.get();
                    commands.channels = m_channels.get();
                    commands.nodes    = m_nodes.get();
                    EdgePulse::Command::registerCommandRoutes(router, loop, commands);
                    EdgePulse::Command::registerCommandAgentRoutes(router, loop, commands);
                    EdgePulse::Web::registerAgentWebSocket(router, loop, *m_nodes, *m_commands, *m_channels, m_lifecycle.get());
                    // 浏览器端点只用于首条认证超时的用例：令牌校验器从不被调用，未认证连接必须在 5 秒后收到 1008
                    m_browserWebSocket->registerRoute(router, [](std::string_view) -> AsynGyanis::Core::Task<std::optional<EdgePulse::Web::BrowserSubscriptionScope>>
                                                      { co_return std::nullopt; }, {});

                    EdgePulse::Firmware::FirmwareDependencies firmware;
                    firmware.auth               = m_context->auth.get();
                    firmware.firmware           = m_firmware.get();
                    firmware.nodes              = m_nodes.get();
                    firmware.commands           = m_commands.get();
                    firmware.channels           = m_channels.get();
                    firmware.tenants            = m_context->tenants.get();
                    firmware.storageDirectory   = m_storage.string();
                    firmware.maximumUploadBytes = 8U * 1024U * 1024U;
                    firmware.signingPolicy      = m_signingPolicy;
                    EdgePulse::Firmware::registerFirmwareRoutes(router, loop, firmware);
                    EdgePulse::Firmware::registerFirmwareAgentRoutes(router, loop, firmware);
                });
    }

    void TearDown() override
    {
        m_server.reset();
        m_firmware.reset();
        m_nodeConfigService.reset();
        m_nodeConfigs.reset();
        m_commands.reset();
        m_lifecycle.reset();
        m_alarmService.reset();
        m_alarmRepo.reset();
        m_telemetry.reset();
        m_groups.reset();
        m_nodeService.reset();
        m_agentRegistry.reset();
        m_nodes.reset();
        m_context.reset();
        m_signingPolicy.reset();
        m_signingKey = SigningKeyPair{};
        std::error_code error;
        std::filesystem::remove_all(m_storage, error);
        std::filesystem::remove_all(m_keyDirectory, error);
    }

    std::string loginInTenant(const std::string &username, const std::string &password, const std::string &tenantCode)
    {
        const auto captcha = m_server->get("/api/auth/captcha");
        EXPECT_EQ(200, captcha.status);
        const auto json = captcha.json();
        EXPECT_TRUE(json.has_value());
        const auto response = m_server->post("/api/auth/login", AsynGyanis::Base::ConfigValue::object({{"username", username},
                                                                                                       {"password", password},
                                                                                                       {"captcha", (*json)["data"]["code"]},
                                                                                                       {"captchaUuid", (*json)["data"]["uuid"]},
                                                                                                       {"tenantCode", tenantCode}})
                                                                        .dump());
        EXPECT_EQ(200, response.status) << response.body;
        return (*response.json())["data"]["token"].get<std::string>();
    }

    std::string login()
    {
        const auto captcha = m_server->get("/api/auth/captcha");
        EXPECT_EQ(200, captcha.status);
        const auto json = captcha.json();
        EXPECT_TRUE(json.has_value());
        const auto response = m_server->post(
                "/api/auth/login", AsynGyanis::Base::ConfigValue::object(
                                           {{"username", "admin"}, {"password", "admin123"}, {"captcha", (*json)["data"]["code"]}, {"captchaUuid", (*json)["data"]["uuid"]}})
                                           .dump());
        EXPECT_EQ(200, response.status) << response.body;
        return (*response.json())["data"]["token"].get<std::string>();
    }

    std::int64_t createNode(const std::string &token, const std::string &name)
    {
        const auto response = m_server->post("/api/nodes", AsynGyanis::Base::ConfigValue::object({{"name", name}, {"groupId", 200}}).dump(), "application/json", bearer(token));
        EXPECT_EQ(200, response.status) << response.body;
        return (*response.json())["data"]["id"].get<std::int64_t>();
    }

    static std::vector<std::pair<std::string, std::string>> agentHeaders(const std::string &fingerprint)
    {
        return {{"X-Test-Agent-Fingerprint", fingerprint}};
    }

    void setNodeFingerprint(const std::int64_t nodeId, const std::string &fingerprint)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(m_context->databasePath().string());
        ASSERT_NE(nullptr, connection->execute("UPDATE biz_node SET cert_fingerprint = '" + fingerprint + "' WHERE id = " + std::to_string(nodeId)));
    }

    std::unique_ptr<EdgePulse::TestSupport::AuthTestContext>   m_context;
    EdgePulse::TestSupport::EventLoopRunner                    m_runner;
    std::shared_ptr<EdgePulse::Device::NodeRepository>         m_nodes;
    std::shared_ptr<EdgePulse::Device::NodeService>            m_nodeService;
    std::shared_ptr<EdgePulse::Device::AgentRegistry>          m_agentRegistry;
    std::shared_ptr<EdgePulse::Device::NodeGroupRepository>    m_groups;
    std::shared_ptr<EdgePulse::Telemetry::TelemetryRepository> m_telemetry;
    std::shared_ptr<EdgePulse::Alarm::AlarmRepository>         m_alarmRepo;
    std::shared_ptr<EdgePulse::Alarm::AlarmService>            m_alarmService;
    std::unique_ptr<EdgePulse::Device::NodeLifecycleService>   m_lifecycle;
    std::shared_ptr<EdgePulse::Command::CommandRepository>     m_commands;
    std::shared_ptr<EdgePulse::Command::AgentChannelRegistry>  m_channels;
    std::shared_ptr<EdgePulse::Device::NodeConfigRepository>   m_nodeConfigs;
    std::shared_ptr<EdgePulse::Device::NodeConfigService>      m_nodeConfigService;
    std::shared_ptr<EdgePulse::Firmware::FirmwareRepository>   m_firmware;
    std::shared_ptr<EdgePulse::Web::BrowserWebSocket>          m_browserWebSocket;
    std::unique_ptr<EdgePulse::TestSupport::ServerHarness>     m_server;
    std::vector<AsynGyanis::Core::Task<>>                      m_probeTasks; ///< WebSocket 探针帧，活到夹具析构
    std::filesystem::path                                      m_storage;
    std::filesystem::path                                      m_keyDirectory;
    SigningKeyPair                                             m_signingKey;
    std::shared_ptr<EdgePulse::Firmware::FirmwareSigningPolicy> m_signingPolicy;
};

// 钉住：遥测上报触发告警，告警列表、确认与规则接口都能读回
TEST_F(FleetEndpointsTest, TelemetryTriggersAndAlarmLifecycle)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "telemetry-node");
    const std::string  fingerprint(64, 'a');
    setNodeFingerprint(nodeId, fingerprint);
    const std::string  body =
            AsynGyanis::Base::ConfigValue::object(
                    {{"nodeId", nodeId},
                     {"samples", AsynGyanis::Base::ConfigValue::array({AsynGyanis::Base::ConfigValue::object({{"metric", "cpu.usage"}, {"value", 96}, {"unit", "%"}})})}})
                    .dump();
    const auto ingest = m_server->post("/api/agent/telemetry", body, "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, ingest.status) << ingest.body;
    EXPECT_EQ(1, (*ingest.json())["data"]["alarms"].size());

    const auto alarms = m_server->get("/api/alarms?pageNum=1&pageSize=10", bearer(token));
    ASSERT_EQ(200, alarms.status) << alarms.body;
    const auto alarmJson = alarms.json();
    ASSERT_TRUE(alarmJson.has_value());
    ASSERT_EQ(1, (*alarmJson)["data"]["rows"].size());
    EXPECT_EQ(1, (*alarmJson)["data"]["openCount"].get<std::int64_t>());
    const std::int64_t alarmId = (*alarmJson)["data"]["rows"][0]["id"].get<std::int64_t>();
    EXPECT_EQ(200, m_server->send("PUT", "/api/alarms/" + std::to_string(alarmId) + "/ack", "{}", "application/json", bearer(token)).status);
    EXPECT_EQ(200, m_server->send("PUT", "/api/alarms/" + std::to_string(alarmId) + "/resolve", "{}", "application/json", bearer(token)).status);

    const auto rules = m_server->get("/api/alarm-rules?pageNum=1&pageSize=10", bearer(token));
    ASSERT_EQ(200, rules.status);
    EXPECT_GE((*rules.json())["data"]["total"].get<std::int64_t>(), 3);
}

// 钉住：命令从浏览器创建、Agent 轮询并回传结果
TEST_F(FleetEndpointsTest, CommandRoundTrip)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "command-node");
    const std::string  fingerprint(64, 'b');
    setNodeFingerprint(nodeId, fingerprint);
    const auto         created =
            m_server->post("/api/commands", AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"type", "reboot"}, {"payload", "{}"}, {"timeoutSeconds", 30}}).dump(),
                           "application/json", bearer(token));
    ASSERT_EQ(200, created.status) << created.body;
    const std::int64_t commandId = (*created.json())["data"]["id"].get<std::int64_t>();

    const auto next = m_server->get("/api/agent/commands/next?nodeId=" + std::to_string(nodeId), agentHeaders(fingerprint));
    ASSERT_EQ(200, next.status) << next.body;
    const auto nextJson = next.json();
    ASSERT_TRUE(nextJson.has_value());
    ASSERT_EQ(commandId, (*nextJson)["data"]["id"].get<std::int64_t>());
    EXPECT_EQ("running", (*nextJson)["data"]["status"].get<std::string>());

    const auto result =
            m_server->post("/api/agent/commands/" + std::to_string(commandId) + "/result",
                           AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"status", "succeeded"}, {"result", R"({"ok":true})"}, {"message", "done"}}).dump(),
                           "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, result.status) << result.body;
    EXPECT_EQ("succeeded", (*result.json())["data"]["status"].get<std::string>());
}

// 钉住：同一个合法 CA 下，没有登记指纹或指纹不符的证书不能冒充节点
TEST_F(FleetEndpointsTest, AgentRequestsRequireCertificateBinding)
{
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                                node = runner.run(m_nodes->create(runner.loop(), "agent-identity", "{}"));
    const std::string                         fingerprint(64, 'a');
    setNodeFingerprint(node.id, fingerprint);

    const std::string heartbeatBody = R"({"nodeId":)" + std::to_string(node.id) + R"(,"agentVersion":"1.0.0"})";
    EXPECT_EQ(403, m_server->post("/api/agent/heartbeat", heartbeatBody).status);
    EXPECT_EQ(403, m_server->post("/api/agent/heartbeat", heartbeatBody, "application/json", agentHeaders(std::string(64, 'b'))).status);
    const auto heartbeat = m_server->post("/api/agent/heartbeat", heartbeatBody, "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, heartbeat.status) << heartbeat.body;
    const auto heartbeatJson = heartbeat.json();
    ASSERT_TRUE(heartbeatJson.has_value());
    // 心跳响应要带回服务端下发的节奏，Agent 不需要自己猜间隔
    EXPECT_EQ(30, (*heartbeatJson)["data"]["heartbeatIntervalSeconds"].get<std::int64_t>());
    EXPECT_EQ(0, (*heartbeatJson)["data"]["desiredConfigVersion"].get<std::int64_t>());

    // 未登记指纹的节点不接受任何证书，即使证书格式合法
    const auto        unbound     = runner.run(m_nodes->create(runner.loop(), "agent-unbound", "{}"));
    const std::string unboundBody = R"({"nodeId":)" + std::to_string(unbound.id) + R"(,"agentVersion":"1.0.0"})";
    EXPECT_EQ(403, m_server->post("/api/agent/heartbeat", unboundBody, "application/json", agentHeaders(std::string(64, 'f'))).status);
}

// 钉住：心跳把最新期望配置版本带回 Agent，配置下发后无需等待下一次命令轮询
TEST_F(FleetEndpointsTest, HeartbeatCarriesDesiredConfigVersion)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "heartbeat-config-node");
    const std::string  fingerprint(64, '9');
    setNodeFingerprint(nodeId, fingerprint);

    const auto applied = m_runner.run(m_nodeConfigService->applyDesired(m_runner.loop(), {1, false}, nodeId, R"({"interval":30})", 0, "初始配置", 1));
    ASSERT_TRUE(applied.has_value()) << applied.error().message;

    const std::string heartbeatBody = R"({"nodeId":)" + std::to_string(nodeId) + R"(,"agentVersion":"1.0.0"})";
    const auto        heartbeat     = m_server->post("/api/agent/heartbeat", heartbeatBody, "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, heartbeat.status) << heartbeat.body;
    EXPECT_EQ(1, (*heartbeat.json())["data"]["desiredConfigVersion"].get<std::int64_t>());

    const auto detail = m_server->get("/api/nodes/" + std::to_string(nodeId), bearer(token));
    ASSERT_EQ(200, detail.status) << detail.body;
    EXPECT_EQ(1, (*detail.json())["data"]["desiredConfigVersion"].get<std::int64_t>());
    EXPECT_FALSE((*detail.json())["data"]["configInSync"].get<bool>());
}

// 钉住：节点已在线时的心跳不再触发在线转换，但仍要销案扫描在心跳落库后插入的离线告警
TEST_F(FleetEndpointsTest, HeartbeatWithoutTransitionResolvesSweepInsertedAlarm)
{
    EdgePulse::TestSupport::EventLoopRunner runner;
    const std::string                         token  = login();
    const std::int64_t                        nodeId = createNode(token, "heartbeat-alarm-node");
    const std::string                         fingerprint(64, '6');
    setNodeFingerprint(nodeId, fingerprint);

    const std::string heartbeatBody = R"({"nodeId":)" + std::to_string(nodeId) + R"(,"agentVersion":"1.0.0"})";
    const auto        online        = m_server->post("/api/agent/heartbeat", heartbeatBody, "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, online.status) << online.body;
    ASSERT_EQ("online", (*online.json())["data"]["status"].get<std::string>());

    // 模拟离线扫描在心跳落库之后补插的系统告警
    const auto node = runner.run(m_nodes->findById(runner.loop(), nodeId));
    ASSERT_TRUE(node.has_value());
    ASSERT_EQ("online", node->status);
    ASSERT_TRUE(runner.run(m_alarmService->reportNodeOffline(runner.loop(), *node, 90)).has_value());
    ASSERT_EQ(1, runner.run(m_alarmRepo->openCount(runner.loop())));

    // 节点已经在线，这次心跳没有离线转在线的转换
    const auto repeated = m_server->post("/api/agent/heartbeat", heartbeatBody, "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, repeated.status) << repeated.body;

    EXPECT_EQ(0, runner.run(m_alarmRepo->openCount(runner.loop())));
    EXPECT_FALSE(runner.run(m_alarmRepo->findOpen(runner.loop(), 0, nodeId)).has_value());
}

// 钉住：删除节点时同步销案系统离线告警，删除后不留永远未恢复的告警
TEST_F(FleetEndpointsTest, DeletingNodeResolvesSystemOfflineAlarm)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "alarm-delete-node");
    const auto         node   = m_runner.run(m_nodes->findById(m_runner.loop(), nodeId));
    ASSERT_TRUE(node.has_value());
    ASSERT_TRUE(m_runner.run(m_alarmService->reportNodeOffline(m_runner.loop(), *node, 90)).has_value());
    ASSERT_EQ(1, m_runner.run(m_alarmRepo->openCount(m_runner.loop())));

    const auto removed = m_server->send("DELETE", "/api/nodes/" + std::to_string(nodeId), "", "", bearer(token));
    ASSERT_EQ(200, removed.status) << removed.body;

    EXPECT_EQ(0, m_runner.run(m_alarmRepo->openCount(m_runner.loop())));
    EXPECT_FALSE(m_runner.run(m_alarmRepo->findOpen(m_runner.loop(), 0, nodeId)).has_value());
}

// 钉住：Agent 走 WebSocket 时 hello 注册成功，命令由长连接推送、结果回写为 succeeded
TEST_F(FleetEndpointsTest, CommandOverAgentWebSocket)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "ws-command-node");

    auto                              promise = std::make_shared<std::promise<WebSocketProbeResult>>();
    std::future<WebSocketProbeResult> future  = promise->get_future();
    AsynGyanis::Core::Task<>          probe   = performWebSocketProbe(m_server->loop(), m_server->port(), token, nodeId, promise);
    m_server->loop().scheduler().scheduleRemote(probe.handle());
    m_probeTasks.push_back(std::move(probe));

    ASSERT_EQ(std::future_status::ready, future.wait_for(std::chrono::seconds{10})) << "WebSocket 探针未在时限内完成";
    const WebSocketProbeResult result = future.get();
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_TRUE(result.connected);
    EXPECT_NE(std::string::npos, result.commandPayload.find("\"type\":\"command\""));

    std::string status;
    for (int attempt = 0; attempt < 100 && status != "succeeded"; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        const auto list = m_server->get("/api/commands?pageNum=1&pageSize=10&nodeId=" + std::to_string(nodeId), bearer(token));
        ASSERT_EQ(200, list.status) << list.body;
        const auto json = list.json();
        ASSERT_TRUE(json.has_value());
        if ((*json)["data"]["rows"].empty())
        {
            continue;
        }
        status = (*json)["data"]["rows"][0]["status"].get<std::string>();
    }
    EXPECT_EQ("succeeded", status);
}

// 钉住：吊销证书要摘除在线通道并以 1008 关闭已建立的 Agent WebSocket，不能只拦下一次 hello
TEST_F(FleetEndpointsTest, CertificateRevocationClosesAgentWebSocket)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "ws-revoke-node");

    auto                               promise = std::make_shared<std::promise<AgentCloseProbeResult>>();
    std::future<AgentCloseProbeResult> future  = promise->get_future();
    AsynGyanis::Core::Task<>           probe   = probeAgentSocketUntilClosed(m_server->loop(), m_server->port(), nodeId, promise);
    m_server->loop().scheduler().scheduleRemote(probe.handle());
    m_probeTasks.push_back(std::move(probe));

    // 先等 hello 把通道登记进表，再发吊销；否则关闭可能落在注册之前
    bool connected = false;
    for (int attempt = 0; attempt < 500 && !connected; ++attempt)
    {
        connected = m_channels->isConnected(nodeId);
        if (!connected)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }
    ASSERT_TRUE(connected) << "Agent WebSocket 未在时限内完成 hello 注册";

    const auto revoked = m_server->post("/api/nodes/" + std::to_string(nodeId) + "/certificate/revoke", "", "application/json", bearer(token));
    ASSERT_EQ(200, revoked.status) << revoked.body;
    EXPECT_FALSE(m_channels->isConnected(nodeId));

    ASSERT_EQ(std::future_status::ready, future.wait_for(std::chrono::seconds{10})) << "Agent WebSocket 未在吊销后收口";
    const AgentCloseProbeResult result = future.get();
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_TRUE(result.connected);
    EXPECT_TRUE(result.closed);
    EXPECT_EQ(1008, result.closeCode);
}

// 钉住：删除节点和吊销一样要摘除在线通道并以 1008 关闭已建立的 Agent WebSocket
TEST_F(FleetEndpointsTest, DeletingNodeClosesAgentWebSocket)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "ws-delete-node");

    auto                               promise = std::make_shared<std::promise<AgentCloseProbeResult>>();
    std::future<AgentCloseProbeResult> future  = promise->get_future();
    AsynGyanis::Core::Task<>           probe   = probeAgentSocketUntilClosed(m_server->loop(), m_server->port(), nodeId, promise);
    m_server->loop().scheduler().scheduleRemote(probe.handle());
    m_probeTasks.push_back(std::move(probe));

    bool connected = false;
    for (int attempt = 0; attempt < 500 && !connected; ++attempt)
    {
        connected = m_channels->isConnected(nodeId);
        if (!connected)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }
    ASSERT_TRUE(connected) << "Agent WebSocket 未在时限内完成 hello 注册";

    const auto removed = m_server->send("DELETE", "/api/nodes/" + std::to_string(nodeId), "", "", bearer(token));
    ASSERT_EQ(200, removed.status) << removed.body;
    EXPECT_FALSE(m_channels->isConnected(nodeId));

    ASSERT_EQ(std::future_status::ready, future.wait_for(std::chrono::seconds{10})) << "Agent WebSocket 未在删除后收口";
    const AgentCloseProbeResult result = future.get();
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_TRUE(result.connected);
    EXPECT_TRUE(result.closed);
    EXPECT_EQ(1008, result.closeCode);
}

// 钉住：浏览器连接 5 秒内不认证即被 1008 收口，且不依赖额外的计时线程
TEST_F(FleetEndpointsTest, BrowserWebSocketClosesUnauthenticatedSession)
{
    auto                                promise = std::make_shared<std::promise<AuthTimeoutProbeResult>>();
    std::future<AuthTimeoutProbeResult> future  = promise->get_future();
    AsynGyanis::Core::Task<>            probe   = probeUnauthenticatedBrowserSocket(m_server->loop(), m_server->port(), promise);
    m_server->loop().scheduler().scheduleRemote(probe.handle());
    m_probeTasks.push_back(std::move(probe));

    ASSERT_EQ(std::future_status::ready, future.wait_for(std::chrono::seconds{10})) << "未认证连接未在时限内收口";
    const AuthTimeoutProbeResult result = future.get();
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_TRUE(result.closed);
    EXPECT_EQ(1008, result.closeCode);
}

// 钉住：固件流式上传、Range 下载、ETag 304 与下发任务
TEST_F(FleetEndpointsTest, FirmwareUploadRangeAndDeploy)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "firmware-node");
    const std::string  fingerprint(64, 'c');
    setNodeFingerprint(nodeId, fingerprint);
    std::string        payload(1024 * 1024, 'E');
    const auto         upload = m_server->send("POST", "/api/firmware?name=EdgePulse+OS&version=1.0.0&fileName=os.bin", payload, "application/octet-stream", bearer(token));
    ASSERT_EQ(200, upload.status) << upload.body;
    const auto uploadJson = upload.json();
    ASSERT_TRUE(uploadJson.has_value());
    const std::int64_t firmwareId = (*uploadJson)["data"]["id"].get<std::int64_t>();
    const std::string  sha256     = (*uploadJson)["data"]["sha256"].get<std::string>();
    EXPECT_EQ(1024 * 1024, (*uploadJson)["data"]["sizeBytes"].get<std::int64_t>());

    std::vector<std::pair<std::string, std::string>> rangeHeaders = agentHeaders(fingerprint);
    rangeHeaders.emplace_back("Range", "bytes=10-19");
    const auto range = m_server->get("/api/agent/firmware/" + std::to_string(firmwareId) + "/download?nodeId=" + std::to_string(nodeId), rangeHeaders);
    ASSERT_EQ(206, range.status) << range.body;
    EXPECT_EQ(std::string(10, 'E'), range.body);
    EXPECT_EQ("bytes 10-19/1048576", range.header("content-range"));
    std::vector<std::pair<std::string, std::string>> cachedHeaders = agentHeaders(fingerprint);
    cachedHeaders.emplace_back("If-None-Match", "\"" + sha256 + "\"");
    const auto cached = m_server->get("/api/agent/firmware/" + std::to_string(firmwareId) + "/download?nodeId=" + std::to_string(nodeId), cachedHeaders);
    EXPECT_EQ(304, cached.status);

    const auto deployed =
            m_server->post("/api/firmware/" + std::to_string(firmwareId) + "/deploy",
                           AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeId})}}).dump(), "application/json", bearer(token));
    ASSERT_EQ(200, deployed.status) << deployed.body;
    ASSERT_EQ(1, (*deployed.json())["data"].size());
    const std::int64_t taskId    = (*deployed.json())["data"][0]["taskId"].get<std::int64_t>();
    const std::int64_t commandId = (*deployed.json())["data"][0]["commandId"].get<std::int64_t>();
    const auto         commands  = m_server->get("/api/commands?pageNum=1&pageSize=10&nodeId=" + std::to_string(nodeId), bearer(token));
    ASSERT_EQ(200, commands.status) << commands.body;
    const auto commandsJson = commands.json();
    ASSERT_TRUE(commandsJson.has_value());
    const auto &rows    = (*commandsJson)["data"]["rows"];
    const auto  created = std::find_if(rows.begin(), rows.end(), [commandId](const AsynGyanis::Base::ConfigValue &row) { return row["id"].get<std::int64_t>() == commandId; });
    ASSERT_NE(rows.end(), created) << commands.body;
    // 下发的下载地址必须带上目标节点，否则真实 Agent 按它请求会得到 400
    const auto downloadPayload = AsynGyanis::Base::parseConfigValue((*created)["payload"].get<std::string>());
    ASSERT_TRUE(downloadPayload.has_value());
    EXPECT_EQ("/api/agent/firmware/" + std::to_string(firmwareId) + "/download?nodeId=" + std::to_string(nodeId), (*downloadPayload)["downloadPath"].get<std::string>());
    const auto         status = m_server->post("/api/agent/firmware/" + std::to_string(firmwareId) + "/status",
                                               AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"status", "installed"}, {"progress", 100}, {"message", "ok"}}).dump(),
                                               "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, status.status) << status.body;
    EXPECT_EQ(taskId, (*status.json())["data"]["id"].get<std::int64_t>());

    const auto tasks = m_server->get("/api/firmware/" + std::to_string(firmwareId) + "/tasks", bearer(token));
    ASSERT_EQ(200, tasks.status);
    EXPECT_EQ("installed", (*tasks.json())["data"][0]["status"].get<std::string>());
}

// 钉住：签名上传落库、下载下发签名头，策略端点读回信任键，篡改或缺 key id 都拒绝
TEST_F(FleetEndpointsTest, FirmwareSignedUploadDownloadAndPolicy)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "signed-firmware-node");
    const std::string  fingerprint(64, '7');
    setNodeFingerprint(nodeId, fingerprint);

    const std::string bytes(4096, 'S');
    const std::string digest    = AsynGyanis::Core::Digest::sha256Hex(bytes);
    const std::string signature = signFirmwareDigest(m_signingKey.privateKey.get(), digest);
    ASSERT_FALSE(signature.empty());

    std::vector<std::pair<std::string, std::string>> uploadHeaders = bearer(token);
    uploadHeaders.emplace_back("X-Firmware-Signature", signature);
    uploadHeaders.emplace_back("X-Firmware-Key-Id", "protocol-key");
    const auto upload = m_server->send("POST", "/api/firmware?name=Signed+OS&version=2.0.0&fileName=s.bin", bytes, "application/octet-stream", uploadHeaders);
    ASSERT_EQ(200, upload.status) << upload.body;
    const auto uploadJson = upload.json();
    ASSERT_TRUE(uploadJson.has_value());
    const std::int64_t firmwareId = (*uploadJson)["data"]["id"].get<std::int64_t>();
    EXPECT_TRUE((*uploadJson)["data"]["signed"].get<bool>());
    EXPECT_EQ("protocol-key", (*uploadJson)["data"]["signatureKeyId"].get<std::string>());
    EXPECT_EQ("ed25519-sha256", (*uploadJson)["data"]["signatureAlgorithm"].get<std::string>());

    const auto download = m_server->get("/api/agent/firmware/" + std::to_string(firmwareId) + "/download?nodeId=" + std::to_string(nodeId), agentHeaders(fingerprint));
    ASSERT_EQ(200, download.status) << download.body;
    ASSERT_TRUE(download.header("x-firmware-signature").has_value());
    EXPECT_EQ(signature, *download.header("x-firmware-signature"));
    ASSERT_TRUE(download.header("x-firmware-key-id").has_value());
    EXPECT_EQ("protocol-key", *download.header("x-firmware-key-id"));
    ASSERT_TRUE(download.header("x-firmware-signature-algorithm").has_value());
    EXPECT_EQ("ed25519-sha256", *download.header("x-firmware-signature-algorithm"));

    const auto policy = m_server->get("/api/firmware/policy", bearer(token));
    ASSERT_EQ(200, policy.status) << policy.body;
    const auto policyJson = policy.json();
    ASSERT_TRUE(policyJson.has_value());
    EXPECT_FALSE((*policyJson)["data"]["requireSignature"].get<bool>());
    ASSERT_EQ(1, (*policyJson)["data"]["trustedKeyIds"].size());
    EXPECT_EQ("protocol-key", (*policyJson)["data"]["trustedKeyIds"][0].get<std::string>());

    // 下发命令的 payload 必须带上签名三元组，Agent 才能按它复核下载到的文件
    const auto deployed =
            m_server->post("/api/firmware/" + std::to_string(firmwareId) + "/deploy",
                           AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeId})}}).dump(), "application/json", bearer(token));
    ASSERT_EQ(200, deployed.status) << deployed.body;
    ASSERT_EQ(1, (*deployed.json())["data"].size());
    const std::int64_t commandId = (*deployed.json())["data"][0]["commandId"].get<std::int64_t>();
    const auto         commands  = m_server->get("/api/commands?pageNum=1&pageSize=10&nodeId=" + std::to_string(nodeId), bearer(token));
    ASSERT_EQ(200, commands.status) << commands.body;
    const auto commandsJson = commands.json();
    ASSERT_TRUE(commandsJson.has_value());
    const auto &rows    = (*commandsJson)["data"]["rows"];
    const auto  created = std::find_if(rows.begin(), rows.end(), [commandId](const AsynGyanis::Base::ConfigValue &row) { return row["id"].get<std::int64_t>() == commandId; });
    ASSERT_NE(rows.end(), created) << commands.body;
    const auto commandPayload = AsynGyanis::Base::parseConfigValue((*created)["payload"].get<std::string>());
    ASSERT_TRUE(commandPayload.has_value());
    EXPECT_EQ(signature, (*commandPayload)["signature"].get<std::string>());
    EXPECT_EQ("protocol-key", (*commandPayload)["signingKeyId"].get<std::string>());
    EXPECT_EQ("ed25519-sha256", (*commandPayload)["signatureAlgorithm"].get<std::string>());

    // 缺少 key id：在收正文前拒绝
    std::vector<std::pair<std::string, std::string>> missingKeyId = bearer(token);
    missingKeyId.emplace_back("X-Firmware-Signature", signature);
    EXPECT_EQ(400, m_server->send("POST", "/api/firmware?name=NoKey&version=2.0.1", bytes, "application/octet-stream", missingKeyId).status);

    // 只给 key id 不给签名：同样在收正文前拒绝，避免留下「未签名却记录 key id」的半套元数据
    std::vector<std::pair<std::string, std::string>> missingSignature = bearer(token);
    missingSignature.emplace_back("X-Firmware-Key-Id", "protocol-key");
    EXPECT_EQ(400, m_server->send("POST", "/api/firmware?name=NoSignature&version=2.0.4", bytes, "application/octet-stream", missingSignature).status);

    // 未知 key id 与摘要不符都不能落库
    std::vector<std::pair<std::string, std::string>> unknownKey = bearer(token);
    unknownKey.emplace_back("X-Firmware-Signature", signature);
    unknownKey.emplace_back("X-Firmware-Key-Id", "missing-key");
    EXPECT_EQ(400, m_server->send("POST", "/api/firmware?name=UnknownKey&version=2.0.2", bytes, "application/octet-stream", unknownKey).status);

    std::vector<std::pair<std::string, std::string>> tampered = bearer(token);
    tampered.emplace_back("X-Firmware-Signature", signFirmwareDigest(m_signingKey.privateKey.get(), AsynGyanis::Core::Digest::sha256Hex("other-bytes")));
    tampered.emplace_back("X-Firmware-Key-Id", "protocol-key");
    EXPECT_EQ(400, m_server->send("POST", "/api/firmware?name=Tampered&version=2.0.3", bytes, "application/octet-stream", tampered).status);

    // 失败上传要把已重命名的成品删掉，只留下那一份成功固件
    std::size_t storedFiles = 0;
    for (const auto &entry: std::filesystem::directory_iterator(m_storage))
    {
        if (entry.is_regular_file())
        {
            ++storedFiles;
        }
    }
    EXPECT_EQ(1U, storedFiles);
}

// 钉住：强制签名时未签名固件不能上传或下发，但历史未签名固件仍可下载
TEST_F(FleetEndpointsTest, FirmwareRequiredPolicyRejectsUnsignedButAllowsLegacyDownload)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "required-firmware-node");
    const std::string  fingerprint(64, '8');
    setNodeFingerprint(nodeId, fingerprint);

    auto requiredPolicy = std::make_shared<EdgePulse::Firmware::FirmwareSigningPolicy>(
            EdgePulse::Firmware::FirmwareSigningPolicy{true, m_signingPolicy->verifier});
    EdgePulse::TestSupport::ServerHarness requiredServer(
            [this, requiredPolicy](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
            {
                EdgePulse::Web::applySecurityHeaders(router);
                EdgePulse::TestSupport::installTestAgentIdentity(router);
                EdgePulse::Auth::registerRoutes(router, loop, *m_context->auth, *m_context->captcha, true);
                EdgePulse::Firmware::FirmwareDependencies firmware;
                firmware.auth               = m_context->auth.get();
                firmware.firmware           = m_firmware.get();
                firmware.nodes              = m_nodes.get();
                firmware.commands           = m_commands.get();
                firmware.channels           = m_channels.get();
                firmware.tenants            = m_context->tenants.get();
                firmware.storageDirectory   = m_storage.string();
                firmware.maximumUploadBytes = 8U * 1024U * 1024U;
                firmware.signingPolicy      = requiredPolicy;
                EdgePulse::Firmware::registerFirmwareRoutes(router, loop, firmware);
                EdgePulse::Firmware::registerFirmwareAgentRoutes(router, loop, firmware);
            });

    const std::string bytes(256, 'R');
    const auto        unsignedUpload =
            requiredServer.send("POST", "/api/firmware?name=Unsigned+OS&version=3.0.0&fileName=u.bin", bytes, "application/octet-stream", bearer(token));
    EXPECT_EQ(400, unsignedUpload.status) << unsignedUpload.body;

    const std::string digest    = AsynGyanis::Core::Digest::sha256Hex(bytes);
    const std::string signature = signFirmwareDigest(m_signingKey.privateKey.get(), digest);
    std::vector<std::pair<std::string, std::string>> signedHeaders = bearer(token);
    signedHeaders.emplace_back("X-Firmware-Signature", signature);
    signedHeaders.emplace_back("X-Firmware-Key-Id", "protocol-key");
    const auto signedUpload =
            requiredServer.send("POST", "/api/firmware?name=Signed+OS&version=3.0.1&fileName=s.bin", bytes, "application/octet-stream", signedHeaders);
    ASSERT_EQ(200, signedUpload.status) << signedUpload.body;
    const std::int64_t signedId = (*signedUpload.json())["data"]["id"].get<std::int64_t>();
    const auto         deploySigned =
            requiredServer.post("/api/firmware/" + std::to_string(signedId) + "/deploy",
                                AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeId})}}).dump(), "application/json", bearer(token));
    ASSERT_EQ(200, deploySigned.status) << deploySigned.body;

    EdgePulse::Firmware::FirmwareRecord legacy;
    legacy.name        = "Legacy OS";
    legacy.version     = "0.9.0";
    legacy.fileName    = "legacy.bin";
    legacy.contentType = "application/octet-stream";
    legacy.sha256      = AsynGyanis::Core::Digest::sha256Hex("legacy-bytes");
    legacy.storagePath = (m_storage / "legacy-unsigned.bin").string();
    {
        std::ofstream stream(legacy.storagePath, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        stream << "legacy-bytes";
    }
    const std::int64_t legacyId = m_runner.run(m_firmware->create(m_runner.loop(), legacy));
    ASSERT_GT(legacyId, 0);

    const auto deployLegacy =
            requiredServer.post("/api/firmware/" + std::to_string(legacyId) + "/deploy",
                                AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeId})}}).dump(), "application/json", bearer(token));
    EXPECT_EQ(400, deployLegacy.status) << deployLegacy.body;

    // 历史未签名固件照常可下载，只是响应不宣称签名
    const auto legacyDownload =
            requiredServer.get("/api/agent/firmware/" + std::to_string(legacyId) + "/download?nodeId=" + std::to_string(nodeId), agentHeaders(fingerprint));
    ASSERT_EQ(200, legacyDownload.status) << legacyDownload.body;
    EXPECT_FALSE(legacyDownload.header("x-firmware-signature").has_value());
    EXPECT_EQ("legacy-bytes", legacyDownload.body);
}

// 钉住：没有装配签名策略时，带签名头的请求不能静默跳过验签，必须在收正文前拒绝
TEST_F(FleetEndpointsTest, FirmwareUploadWithoutSigningPolicyRejectsSignedRequest)
{
    const std::string token = login();
    EdgePulse::TestSupport::ServerHarness unpolicedServer(
            [this](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
            {
                EdgePulse::Web::applySecurityHeaders(router);
                EdgePulse::Auth::registerRoutes(router, loop, *m_context->auth, *m_context->captcha, true);
                EdgePulse::Firmware::FirmwareDependencies firmware;
                firmware.auth               = m_context->auth.get();
                firmware.firmware           = m_firmware.get();
                firmware.nodes              = m_nodes.get();
                firmware.commands           = m_commands.get();
                firmware.channels           = m_channels.get();
                firmware.storageDirectory   = m_storage.string();
                firmware.maximumUploadBytes = 8U * 1024U * 1024U;
                // signingPolicy 保持为空：模拟装配遗漏，策略缺位不等于允许无签名固件
                EdgePulse::Firmware::registerFirmwareRoutes(router, loop, firmware);
            });

    const std::string bytes(128, 'P');
    const auto        unsignedUpload =
            unpolicedServer.send("POST", "/api/firmware?name=Plain+OS&version=4.0.0&fileName=p.bin", bytes, "application/octet-stream", bearer(token));
    ASSERT_EQ(200, unsignedUpload.status) << unsignedUpload.body;

    std::vector<std::pair<std::string, std::string>> signedHeaders = bearer(token);
    signedHeaders.emplace_back("X-Firmware-Signature", AsynGyanis::Base::base64Encode(std::string(64, '\0')));
    signedHeaders.emplace_back("X-Firmware-Key-Id", "protocol-key");
    const auto signedUpload =
            unpolicedServer.send("POST", "/api/firmware?name=Signed+OS&version=4.0.1&fileName=s.bin", bytes, "application/octet-stream", signedHeaders);
    EXPECT_EQ(400, signedUpload.status) << signedUpload.body;
    EXPECT_NE(std::string::npos, signedUpload.body.find("信任键"));

    // 被拒请求在收正文前就返回，只留下成功那份固件
    std::size_t storedFiles = 0;
    for (const auto &entry: std::filesystem::directory_iterator(m_storage))
    {
        if (entry.is_regular_file())
        {
            ++storedFiles;
        }
    }
    EXPECT_EQ(1U, storedFiles);
}

// 钉住：节点分组 CRUD 与占用保护
TEST_F(FleetEndpointsTest, NodeGroupCrud)
{
    const std::string token = login();
    const auto created      = m_server->post("/api/node-groups", AsynGyanis::Base::ConfigValue::object({{"name", "华东"}, {"sort", 5}}).dump(), "application/json", bearer(token));
    ASSERT_EQ(200, created.status) << created.body;
    const std::int64_t groupId = (*created.json())["data"]["id"].get<std::int64_t>();
    createNode(token, "grouped-node");
    const auto tree = m_server->get("/api/node-groups", bearer(token));
    ASSERT_EQ(200, tree.status);
    EXPECT_GE((*tree.json())["data"].size(), 2U);
    EXPECT_EQ(200, m_server->send("DELETE", "/api/node-groups/" + std::to_string(groupId), "", "", bearer(token)).status);
}

// 钉住：Agent 侧没有会话，命令与固件任务必须按节点/固件自身的租户查找，不能落在默认租户
TEST_F(FleetEndpointsTest, AgentRoutesResolveTenantFromNodeAndFirmware)
{
    EdgePulse::System::TenantProvisionRequest request;
    request.code              = "fleet_tenant";
    request.name              = "设备租户";
    request.adminUsername     = "fleet-admin";
    request.adminPasswordHash = EdgePulse::Common::hashPassword("fleet-pass-1");
    const auto provisioned    = m_runner.run(m_context->tenants->provision(m_runner.loop(), std::move(request), 1));
    ASSERT_TRUE(provisioned.has_value()) << provisioned.error();
    const std::int64_t tenantId = provisioned->tenantId;

    const auto tenantNodes    = m_nodes->scoped({tenantId, false});
    const auto node           = m_runner.run(tenantNodes->create(m_runner.loop(), "tenant-b-node", "{}"));
    const std::string fingerprint(64, 'd');
    setNodeFingerprint(node.id, fingerprint);
    const auto tenantCommands = m_commands->scoped({tenantId, false});
    const auto command        = m_runner.run(tenantCommands->create(m_runner.loop(), node.id, "reboot", "{}", 1, 60));
    ASSERT_TRUE(command.has_value());

    const auto next = m_server->get("/api/agent/commands/next?nodeId=" + std::to_string(node.id), agentHeaders(fingerprint));
    ASSERT_EQ(200, next.status) << next.body;
    ASSERT_TRUE((*next.json())["data"].is_object()) << next.body;
    EXPECT_EQ(command->id, (*next.json())["data"]["id"].get<std::int64_t>());

    const auto result = m_server->post("/api/agent/commands/" + std::to_string(command->id) + "/result",
                                       AsynGyanis::Base::ConfigValue::object({{"nodeId", node.id}, {"status", "succeeded"}, {"result", "{}"}}).dump(),
                                       "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, result.status) << result.body;
    EXPECT_EQ("succeeded", (*result.json())["data"]["status"].get<std::string>());

    EdgePulse::Firmware::FirmwareRecord firmware;
    firmware.name                     = "Tenant OS";
    firmware.version                  = "2.0.0";
    firmware.fileName                 = "tenant-os.bin";
    firmware.contentType              = "application/octet-stream";
    firmware.sha256                   = std::string(64, 'b');
    firmware.storagePath              = (m_storage / "tenant-os.bin").string();
    const auto         tenantFirmware = m_firmware->scoped({tenantId, false});
    const std::int64_t firmwareId     = m_runner.run(tenantFirmware->create(m_runner.loop(), firmware));
    ASSERT_GT(firmwareId, 0);
    const std::int64_t taskId = m_runner.run(tenantFirmware->createTask(m_runner.loop(), firmwareId, node.id));
    ASSERT_GT(taskId, 0);
    const auto status = m_server->post("/api/agent/firmware/" + std::to_string(firmwareId) + "/status",
                                       AsynGyanis::Base::ConfigValue::object({{"nodeId", node.id}, {"status", "installed"}, {"progress", 100}}).dump(),
                                       "application/json", agentHeaders(fingerprint));
    ASSERT_EQ(200, status.status) << status.body;
    EXPECT_EQ(taskId, (*status.json())["data"]["id"].get<std::int64_t>());

    // 默认租户的节点不能借自己的合法证书取走别家租户的固件
    const auto        defaultNode = m_runner.run(m_nodes->create(m_runner.loop(), "tenant-a-node", "{}"));
    const std::string defaultFingerprint(64, '5');
    setNodeFingerprint(defaultNode.id, defaultFingerprint);
    const auto crossTenant = m_server->get("/api/agent/firmware/" + std::to_string(firmwareId) + "/download?nodeId=" + std::to_string(defaultNode.id),
                                           agentHeaders(defaultFingerprint));
    EXPECT_EQ(403, crossTenant.status) << crossTenant.body;
}

// 钉住：批量命令共享批次号；计划时间未到时 Agent 取不到，到点后可取
TEST_F(FleetEndpointsTest, BatchAndScheduledCommands)
{
    const std::string  token  = login();
    const std::int64_t nodeA  = createNode(token, "batch-node-a");
    const std::int64_t nodeB  = createNode(token, "batch-node-b");
    const std::string  fingerprintA(64, 'e');
    const std::string  fingerprintB(64, '1');
    setNodeFingerprint(nodeA, fingerprintA);
    setNodeFingerprint(nodeB, fingerprintB);
    const auto         now    = std::chrono::system_clock::now();
    const std::string  future = EdgePulse::Common::formatDateTimeUtc(now + std::chrono::hours{1});
    const std::string  past   = EdgePulse::Common::formatDateTimeUtc(now - std::chrono::minutes{1});

    const auto scheduled =
            m_server->post("/api/commands/batch",
                           AsynGyanis::Base::ConfigValue::object(
                                   {{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeA, nodeB})}, {"type", "diagnostics.collect"}, {"payload", "{}"}, {"scheduledAt", future}})
                                   .dump(),
                           "application/json", bearer(token));
    ASSERT_EQ(200, scheduled.status) << scheduled.body;
    const auto scheduledJson = scheduled.json();
    ASSERT_TRUE(scheduledJson.has_value());
    ASSERT_EQ(2, (*scheduledJson)["data"]["commands"].size());
    const std::int64_t batchId = (*scheduledJson)["data"]["batchId"].get<std::int64_t>();
    EXPECT_EQ(batchId, (*scheduledJson)["data"]["commands"][0]["batchId"].get<std::int64_t>());
    EXPECT_EQ(future, (*scheduledJson)["data"]["commands"][0]["scheduledAt"].get<std::string>());
    const auto early = m_server->get("/api/agent/commands/next?nodeId=" + std::to_string(nodeA), agentHeaders(fingerprintA));
    ASSERT_EQ(200, early.status) << early.body;
    EXPECT_TRUE((*early.json())["data"].is_null());
    const auto canceled = m_server->send("PUT", "/api/commands/batch/" + std::to_string(batchId) + "/cancel", "{}", "application/json", bearer(token));
    ASSERT_EQ(200, canceled.status) << canceled.body;
    ASSERT_EQ(2, (*canceled.json())["data"].size());
    EXPECT_EQ("canceled", (*canceled.json())["data"][0]["status"].get<std::string>());
    const auto afterCancel = m_server->get("/api/agent/commands/next?nodeId=" + std::to_string(nodeA), agentHeaders(fingerprintA));
    ASSERT_EQ(200, afterCancel.status) << afterCancel.body;
    EXPECT_TRUE((*afterCancel.json())["data"].is_null());

    const auto due = m_server->post(
            "/api/commands/batch",
            AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeA})}, {"type", "reboot"}, {"payload", "{}"}, {"scheduledAt", past}})
                    .dump(),
            "application/json", bearer(token));
    ASSERT_EQ(200, due.status) << due.body;
    const auto next = m_server->get("/api/agent/commands/next?nodeId=" + std::to_string(nodeA), agentHeaders(fingerprintA));
    ASSERT_EQ(200, next.status) << next.body;
    ASSERT_TRUE((*next.json())["data"].is_object()) << next.body;
    EXPECT_EQ("reboot", (*next.json())["data"]["type"].get<std::string>());
}

// 钉住：固件与目标节点必须同租户；跨租户下发整单 400，且不留下任何任务行
TEST_F(FleetEndpointsTest, FirmwareDeployRejectsCrossTenantNodes)
{
    const std::string  token       = login();
    const std::int64_t defaultNode = createNode(token, "cross-tenant-deploy-node");

    EdgePulse::System::TenantProvisionRequest request;
    request.code              = "firmware_tenant";
    request.name              = "固件租户";
    request.adminUsername     = "firmware-admin";
    request.adminPasswordHash = EdgePulse::Common::hashPassword("firmware-pass-1");
    const auto provisioned    = m_runner.run(m_context->tenants->provision(m_runner.loop(), std::move(request), 1));
    ASSERT_TRUE(provisioned.has_value()) << provisioned.error();
    const std::int64_t tenantId = provisioned->tenantId;

    EdgePulse::Firmware::FirmwareRecord record;
    record.name        = "Tenant OS";
    record.version     = "1.0.0";
    record.fileName    = "tenant-os.bin";
    record.contentType = "application/octet-stream";
    record.sha256      = std::string(64, '4');
    record.storagePath = (m_storage / "tenant-os.bin").string();
    const std::int64_t firmwareId = m_runner.run(m_firmware->scoped({tenantId, false})->create(m_runner.loop(), record));
    ASSERT_GT(firmwareId, 0);

    // 平台管理员能看见别家固件，但把节点列表指向默认租户节点必须整单拒绝
    const auto deployed =
            m_server->post("/api/firmware/" + std::to_string(firmwareId) + "/deploy",
                           AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({defaultNode})}}).dump(), "application/json", bearer(token));
    EXPECT_EQ(400, deployed.status) << deployed.body;
    EXPECT_NE(std::string::npos, deployed.body.find(std::to_string(defaultNode)));
    EXPECT_EQ(0, countFirmwareTasks(m_context->databasePath(), firmwareId));
}

// 钉住：单次下发最多 100 个节点，超限在解析节点前就整单拒绝
TEST_F(FleetEndpointsTest, FirmwareDeployRejectsMoreThanHundredNodes)
{
    const std::string token = login();

    EdgePulse::Firmware::FirmwareRecord record;
    record.name        = "Batch OS";
    record.version     = "1.0.0";
    record.fileName    = "batch-os.bin";
    record.contentType = "application/octet-stream";
    record.sha256      = std::string(64, '6');
    record.storagePath = (m_storage / "batch-os.bin").string();
    const std::int64_t firmwareId = m_runner.run(m_firmware->create(m_runner.loop(), record));
    ASSERT_GT(firmwareId, 0);

    AsynGyanis::Base::ConfigValue nodeIds = AsynGyanis::Base::ConfigValue::array();
    for (std::int64_t nodeId = 1; nodeId <= 101; ++nodeId)
    {
        nodeIds.push_back(nodeId);
    }
    const auto deployed = m_server->post("/api/firmware/" + std::to_string(firmwareId) + "/deploy",
                                         AsynGyanis::Base::ConfigValue::object({{"nodeIds", std::move(nodeIds)}}).dump(), "application/json", bearer(token));
    EXPECT_EQ(400, deployed.status) << deployed.body;
    EXPECT_NE(std::string::npos, deployed.body.find("1 到 100"));
    EXPECT_EQ(0, countFirmwareTasks(m_context->databasePath(), firmwareId));
}

// 钉住：任务列表按任务租户连接节点，历史孤儿行不能带出别家节点的名称
TEST_F(FleetEndpointsTest, FirmwareTaskListDoesNotExposeForeignNodeName)
{
    const std::string  token   = login();
    const std::int64_t ownNode = createNode(token, "task-list-own-node");

    EdgePulse::System::TenantProvisionRequest request;
    request.code              = "task_list_tenant";
    request.name              = "任务列表租户";
    request.adminUsername     = "task-list-admin";
    request.adminPasswordHash = EdgePulse::Common::hashPassword("task-list-pass-1");
    const auto provisioned    = m_runner.run(m_context->tenants->provision(m_runner.loop(), std::move(request), 1));
    ASSERT_TRUE(provisioned.has_value()) << provisioned.error();
    const auto foreignNode = m_runner.run(m_nodes->scoped({provisioned->tenantId, false})->create(m_runner.loop(), "task-list-foreign-node", "{}"));
    ASSERT_EQ(provisioned->tenantId, foreignNode.tenantId);

    EdgePulse::Firmware::FirmwareRecord record;
    record.name        = "Task OS";
    record.version     = "1.0.0";
    record.fileName    = "task-os.bin";
    record.contentType = "application/octet-stream";
    record.sha256      = std::string(64, '7');
    record.storagePath = (m_storage / "task-os.bin").string();
    const std::int64_t firmwareId = m_runner.run(m_firmware->create(m_runner.loop(), record));
    ASSERT_GT(firmwareId, 0);

    // 任务行都落在默认租户，其中一条指向别家节点：连接条件必须带上租户，否则会泄漏节点名
    insertFirmwareTask(m_context->databasePath(), 900000000000000101, 1, firmwareId, ownNode);
    insertFirmwareTask(m_context->databasePath(), 900000000000000102, 1, firmwareId, foreignNode.id);

    const auto tasks = m_server->get("/api/firmware/" + std::to_string(firmwareId) + "/tasks", bearer(token));
    ASSERT_EQ(200, tasks.status) << tasks.body;
    const auto json = tasks.json();
    ASSERT_TRUE(json.has_value());
    ASSERT_EQ(2, (*json)["data"].size()) << tasks.body;
    for (const auto &task: (*json)["data"])
    {
        const std::int64_t nodeId = task["nodeId"].get<std::int64_t>();
        if (nodeId == ownNode)
        {
            EXPECT_EQ("task-list-own-node", task["nodeName"].get<std::string>());
        } else
        {
            EXPECT_EQ(foreignNode.id, nodeId);
            EXPECT_EQ("", task["nodeName"].get<std::string>());
        }
    }
}

// 钉住：强制签名时回滚到未签名固件在解析目标与创建任务之前就被拒绝
TEST_F(FleetEndpointsTest, FirmwareRequiredPolicyRejectsUnsignedRollbackBeforeTaskCreation)
{
    const std::string  token  = login();
    const std::int64_t nodeId = createNode(token, "unsigned-rollback-node");

    auto requiredPolicy = std::make_shared<EdgePulse::Firmware::FirmwareSigningPolicy>(
            EdgePulse::Firmware::FirmwareSigningPolicy{true, m_signingPolicy->verifier});
    EdgePulse::TestSupport::ServerHarness requiredServer(
            [this, requiredPolicy](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
            {
                EdgePulse::Web::applySecurityHeaders(router);
                EdgePulse::Auth::registerRoutes(router, loop, *m_context->auth, *m_context->captcha, true);
                EdgePulse::Firmware::FirmwareDependencies firmware;
                firmware.auth               = m_context->auth.get();
                firmware.firmware           = m_firmware.get();
                firmware.nodes              = m_nodes.get();
                firmware.commands           = m_commands.get();
                firmware.channels           = m_channels.get();
                firmware.tenants            = m_context->tenants.get();
                firmware.storageDirectory   = m_storage.string();
                firmware.maximumUploadBytes = 8U * 1024U * 1024U;
                firmware.signingPolicy      = requiredPolicy;
                EdgePulse::Firmware::registerFirmwareRoutes(router, loop, firmware);
            });

    EdgePulse::Firmware::FirmwareRecord legacy;
    legacy.name        = "Legacy OS";
    legacy.version     = "0.9.0";
    legacy.fileName    = "legacy.bin";
    legacy.contentType = "application/octet-stream";
    legacy.sha256      = std::string(64, '8');
    legacy.storagePath = (m_storage / "legacy.bin").string();
    const std::int64_t legacyId = m_runner.run(m_firmware->create(m_runner.loop(), legacy));
    ASSERT_GT(legacyId, 0);

    EdgePulse::Firmware::FirmwareRecord signedRecord = legacy;
    signedRecord.version            = "1.0.0";
    signedRecord.fileName           = "signed.bin";
    signedRecord.signature          = std::string(86, 'A');
    signedRecord.signingKeyId       = "protocol-key";
    signedRecord.signatureAlgorithm = "ed25519-sha256";
    const std::int64_t signedId     = m_runner.run(m_firmware->create(m_runner.loop(), signedRecord));
    ASSERT_GT(signedId, 0);
    // 当前版本任务把上一版记为未签名固件，回滚源解析后目标即 legacy
    ASSERT_GT(m_runner.run(m_firmware->createTask(m_runner.loop(), signedId, nodeId, legacyId)), 0);

    const auto rollback =
            requiredServer.post("/api/firmware/" + std::to_string(signedId) + "/rollback",
                                AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeId})}}).dump(), "application/json", bearer(token));
    EXPECT_EQ(400, rollback.status) << rollback.body;
    EXPECT_NE(std::string::npos, rollback.body.find("未签名"));
    // 拒绝必须发生在建任务之前：未签名目标不能多出回滚任务行
    EXPECT_EQ(0, countFirmwareTasks(m_context->databasePath(), legacyId));
}

// 钉住：灰度下发按批次返回剩余节点，回滚把节点切回上一版本
TEST_F(FleetEndpointsTest, FirmwareCanaryAndRollback)
{
    const std::string  token = login();
    const std::int64_t nodeA = createNode(token, "canary-node-a");
    const std::int64_t nodeB = createNode(token, "canary-node-b");
    const std::string  fingerprintA(64, '2');
    const std::string  fingerprintB(64, '3');
    setNodeFingerprint(nodeA, fingerprintA);
    setNodeFingerprint(nodeB, fingerprintB);
    const std::string bytes(1024, 'F');

    const auto uploadFirst = m_server->send("POST", "/api/firmware?name=Canary+OS&version=1.0.0&fileName=a.bin", bytes, "application/octet-stream", bearer(token));
    ASSERT_EQ(200, uploadFirst.status) << uploadFirst.body;
    const std::int64_t firstId = (*uploadFirst.json())["data"]["id"].get<std::int64_t>();
    const auto         deployFirst =
            m_server->post("/api/firmware/" + std::to_string(firstId) + "/deploy",
                           AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeA, nodeB})}}).dump(), "application/json", bearer(token));
    ASSERT_EQ(200, deployFirst.status) << deployFirst.body;
    for (const auto &[nodeId, fingerprint]: {std::pair{nodeA, fingerprintA}, std::pair{nodeB, fingerprintB}})
    {
        ASSERT_EQ(200, m_server->post("/api/agent/firmware/" + std::to_string(firstId) + "/status",
                                      AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"status", "installed"}, {"progress", 100}}).dump(),
                                      "application/json", agentHeaders(fingerprint))
                               .status);
    }

    const auto uploadSecond = m_server->send("POST", "/api/firmware?name=Canary+OS&version=2.0.0&fileName=b.bin", bytes, "application/octet-stream", bearer(token));
    ASSERT_EQ(200, uploadSecond.status) << uploadSecond.body;
    const std::int64_t secondId = (*uploadSecond.json())["data"]["id"].get<std::int64_t>();
    const auto         canary = m_server->post("/api/firmware/" + std::to_string(secondId) + "/deploy",
                                               AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeA, nodeB})}, {"batchSize", 1}}).dump(),
                                               "application/json", bearer(token));
    ASSERT_EQ(200, canary.status) << canary.body;
    const auto canaryJson = canary.json();
    ASSERT_TRUE(canaryJson.has_value());
    ASSERT_EQ(1, (*canaryJson)["data"]["deployed"].size());
    ASSERT_EQ(1, (*canaryJson)["data"]["deferredNodeIds"].size());
    EXPECT_EQ(nodeB, (*canaryJson)["data"]["deferredNodeIds"][0].get<std::int64_t>());
    ASSERT_EQ(200, m_server->post("/api/agent/firmware/" + std::to_string(secondId) + "/status",
                                  AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeA}, {"status", "installed"}, {"progress", 100}}).dump(),
                                  "application/json", agentHeaders(fingerprintA))
                           .status);

    const auto rollback =
            m_server->post("/api/firmware/" + std::to_string(secondId) + "/rollback",
                           AsynGyanis::Base::ConfigValue::object({{"nodeIds", AsynGyanis::Base::ConfigValue::array({nodeA})}}).dump(), "application/json", bearer(token));
    ASSERT_EQ(200, rollback.status) << rollback.body;
    const auto rollbackJson = rollback.json();
    ASSERT_TRUE(rollbackJson.has_value());
    ASSERT_EQ(1, (*rollbackJson)["data"].size());
    EXPECT_EQ(firstId, (*rollbackJson)["data"][0]["toFirmwareId"].get<std::int64_t>());
}

// 钉住：平台管理员可把节点与固件建到指定租户，租户管理员只看到自己的
TEST_F(FleetEndpointsTest, PlatformAdminProvisionsTenantNodesAndFirmware)
{
    EdgePulse::System::TenantProvisionRequest request;
    request.code              = "fleet_b";
    request.name              = "设备租户B";
    request.adminUsername     = "fleet-b-admin";
    request.adminPasswordHash = EdgePulse::Common::hashPassword("fleet-b-pass");
    const auto provisioned    = m_runner.run(m_context->tenants->provision(m_runner.loop(), std::move(request), 1));
    ASSERT_TRUE(provisioned.has_value()) << provisioned.error();
    const std::int64_t tenantId = provisioned->tenantId;

    const std::string platformToken = login();
    const auto createdNode = m_server->post("/api/nodes", AsynGyanis::Base::ConfigValue::object({{"name", "tenant-b-node"}, {"groupId", 200}, {"tenantId", tenantId}}).dump(),
                                            "application/json", bearer(platformToken));
    ASSERT_EQ(200, createdNode.status) << createdNode.body;
    EXPECT_EQ(tenantId, (*createdNode.json())["data"]["tenantId"].get<std::int64_t>());
    createNode(platformToken, "default-tenant-node");

    const auto badTenant = m_server->post("/api/nodes", AsynGyanis::Base::ConfigValue::object({{"name", "bad-tenant-node"}, {"tenantId", 99999999}}).dump(), "application/json",
                                          bearer(platformToken));
    EXPECT_EQ(400, badTenant.status);

    const std::string tenantToken = loginInTenant("fleet-b-admin", "fleet-b-pass", "fleet_b");
    const auto        tenantNodes = m_server->get("/api/nodes?pageNum=1&pageSize=10&keyword=tenant-b-node", bearer(tenantToken));
    ASSERT_EQ(200, tenantNodes.status) << tenantNodes.body;
    EXPECT_EQ(1, (*tenantNodes.json())["data"]["total"].get<std::int64_t>());
    const auto hidden = m_server->get("/api/nodes?pageNum=1&pageSize=10&keyword=default-tenant-node", bearer(tenantToken));
    ASSERT_EQ(200, hidden.status) << hidden.body;
    EXPECT_EQ(0, (*hidden.json())["data"]["total"].get<std::int64_t>());

    const std::string bytes(512, 'T');
    const auto upload = m_server->send("POST", "/api/firmware?name=Tenant+OS&version=3.0.0&fileName=t.bin&tenantId=" + std::to_string(tenantId), bytes, "application/octet-stream",
                                       bearer(platformToken));
    ASSERT_EQ(200, upload.status) << upload.body;
    EXPECT_EQ(tenantId, (*upload.json())["data"]["tenantId"].get<std::int64_t>());
    const auto tenantFirmware = m_server->get("/api/firmware?pageNum=1&pageSize=10&keyword=Tenant", bearer(tenantToken));
    ASSERT_EQ(200, tenantFirmware.status) << tenantFirmware.body;
    EXPECT_EQ(1, (*tenantFirmware.json())["data"]["total"].get<std::int64_t>());
}
