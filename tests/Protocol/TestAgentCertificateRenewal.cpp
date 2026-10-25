#include "Base/Config/ConfigValue.h"
#include "Base/Database/Database.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Alarm/AlarmRepository.h"
#include "Core/Alarm/AlarmService.h"
#include "Core/Command/AgentChannelRegistry.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/AgentCertificateAuthority.h"
#include "Core/Device/AgentCertificateRepository.h"
#include "Core/Device/AgentRegistry.h"
#include "Core/Device/CertificateBinding.h"
#include "Core/Device/EnrollmentRepository.h"
#include "Core/Device/EnrollmentService.h"
#include "Core/Device/NodeRepository.h"
#include "Core/System/TenantRepository.h"
#include "Core/Telemetry/TelemetryRepository.h"
#include "Plugins/CommandPlugin/CommandController.h"
#include "Plugins/DevicePlugin/AgentController.h"
#include "Plugins/DevicePlugin/NodeJson.h"
#include "Plugins/TelemetryPlugin/TelemetryController.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"
#include "TestSupport/HttpClientHarness.h"
#include "TestSupport/TemporaryDirectory.h"

#include <Core/Tls/TlsPeerIdentity.h>

#include <gtest/gtest.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace
{
    using BioHandle    = std::unique_ptr<BIO, decltype(&BIO_free)>;
    using KeyHandle    = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using CertHandle   = std::unique_ptr<X509, decltype(&X509_free)>;
    using ReqHandle    = std::unique_ptr<X509_REQ, decltype(&X509_REQ_free)>;
    using ExtHandle    = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>;
    using NameHandle   = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>;
    using BignumHandle = std::unique_ptr<BIGNUM, decltype(&BN_free)>;

    constexpr char        kHexDigits[]      = "0123456789abcdef";
    constexpr std::size_t kFingerprintBytes = 32;

    /**
     * @brief 生成测试材料失败时抛出带步骤名的异常
     * @param succeeded OpenSSL 调用的成功判据
     * @param step 失败步骤
     * @throws std::runtime_error succeeded 为 false 时抛出
     */
    void requireStep(const bool succeeded, const char *step)
    {
        if (!succeeded)
        {
            throw std::runtime_error(std::string{"生成测试材料失败："} + step);
        }
    }

    /**
     * @brief 取内存 BIO 中的文本
     * @param bio 内存 BIO
     * @return std::string 文本内容
     */
    [[nodiscard]] std::string bioText(BIO &bio)
    {
        char      *buffer = nullptr;
        const long length = BIO_get_mem_data(&bio, &buffer);
        if (buffer == nullptr || length <= 0)
        {
            throw std::runtime_error("生成测试材料失败：内存缓冲为空");
        }
        return std::string{buffer, static_cast<std::size_t>(length)};
    }

    /**
     * @brief 生成一把 P-256 私钥
     * @return KeyHandle 私钥句柄
     */
    [[nodiscard]] KeyHandle generateKey()
    {
        KeyHandle key(EVP_EC_gen("prime256v1"), &EVP_PKEY_free);
        requireStep(key != nullptr, "生成椭圆曲线私钥");
        return key;
    }

    /**
     * @brief 在临时目录里生成自签 CA 证书与私钥
     * @param directory 临时目录
     * @return std::pair<std::filesystem::path, std::filesystem::path> 证书与私钥路径
     */
    [[nodiscard]] std::pair<std::filesystem::path, std::filesystem::path> writeAuthority(const EdgePulse::TestSupport::TemporaryDirectory &directory)
    {
        KeyHandle key = generateKey();

        CertHandle certificate(X509_new(), &X509_free);
        requireStep(certificate != nullptr, "创建 CA 证书");
        requireStep(X509_set_version(certificate.get(), 2) == 1, "设置 CA 证书版本");
        requireStep(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1, "设置 CA 序列号");
        requireStep(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) != nullptr, "设置 CA 起始时间");
        requireStep(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3650L * 24L * 60L * 60L) != nullptr, "设置 CA 到期时间");
        requireStep(X509_set_pubkey(certificate.get(), key.get()) == 1, "写入 CA 公钥");

        X509_NAME *subject = X509_get_subject_name(certificate.get());
        requireStep(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char *>("EdgePulse Renewal Test CA"), -1, -1, 0) == 1, "写入 CA 主题");
        requireStep(X509_set_issuer_name(certificate.get(), subject) == 1, "写入 CA 签发者");

        ExtHandle constraints(X509V3_EXT_nconf_nid(nullptr, nullptr, NID_basic_constraints, "critical,CA:TRUE"), &X509_EXTENSION_free);
        requireStep(constraints != nullptr, "创建 CA 基本约束");
        requireStep(X509_add_ext(certificate.get(), constraints.get(), -1) == 1, "写入 CA 基本约束");
        ExtHandle usage(X509V3_EXT_nconf_nid(nullptr, nullptr, NID_key_usage, "critical,keyCertSign,cRLSign"), &X509_EXTENSION_free);
        requireStep(usage != nullptr, "创建 CA 密钥用途");
        requireStep(X509_add_ext(certificate.get(), usage.get(), -1) == 1, "写入 CA 密钥用途");
        requireStep(X509_sign(certificate.get(), key.get(), EVP_sha256()) != 0, "签名 CA 证书");

        BioHandle certificateBio(BIO_new(BIO_s_mem()), &BIO_free);
        requireStep(certificateBio != nullptr, "创建 CA 证书缓冲");
        requireStep(PEM_write_bio_X509(certificateBio.get(), certificate.get()) == 1, "输出 CA 证书");
        BioHandle keyBio(BIO_new(BIO_s_mem()), &BIO_free);
        requireStep(keyBio != nullptr, "创建 CA 私钥缓冲");
        requireStep(PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1, "输出 CA 私钥");

        const std::filesystem::path certificateFile = directory.write("ca.pem", bioText(*certificateBio));
        const std::filesystem::path keyFile         = directory.write("ca-key.pem", bioText(*keyBio));
#ifndef _WIN32
        requireStep(::chmod(keyFile.c_str(), S_IRUSR | S_IWUSR) == 0, "设置 CA 私钥权限");
#endif
        return {certificateFile, keyFile};
    }

    /**
     * @brief 生成 PKCS#10 CSR
     * @param key 私钥
     * @param commonName CSR 主题里的 CN
     * @return std::string CSR 的 PEM 文本
     */
    [[nodiscard]] std::string makeCertificateRequest(EVP_PKEY &key, const std::string &commonName)
    {
        ReqHandle request(X509_REQ_new(), &X509_REQ_free);
        requireStep(request != nullptr, "创建 CSR");
        requireStep(X509_REQ_set_version(request.get(), 0) == 1, "设置 CSR 版本");
        requireStep(X509_REQ_set_pubkey(request.get(), &key) == 1, "写入 CSR 公钥");
        X509_NAME *subject = X509_REQ_get_subject_name(request.get());
        requireStep(subject != nullptr, "创建 CSR 主题");
        requireStep(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char *>(commonName.c_str()), -1, -1, 0) == 1, "写入 CSR 主题");
        requireStep(X509_REQ_sign(request.get(), &key, EVP_sha256()) > 0, "签名 CSR");

        BioHandle bio(BIO_new(BIO_s_mem()), &BIO_free);
        requireStep(bio != nullptr, "创建 CSR 缓冲");
        requireStep(PEM_write_bio_X509_REQ(bio.get(), request.get()) == 1, "输出 CSR");
        return bioText(*bio);
    }

    /**
     * @brief 解析证书 PEM
     * @param pem PEM 文本
     * @return CertHandle 证书句柄
     */
    [[nodiscard]] CertHandle parseCertificate(const std::string &pem)
    {
        BioHandle bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free);
        requireStep(bio != nullptr, "创建证书内存缓冲");
        CertHandle certificate(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), &X509_free);
        requireStep(certificate != nullptr, "解析证书 PEM");
        return certificate;
    }

    /**
     * @brief 计算证书 SHA-256 指纹
     * @param certificate 证书
     * @return std::string 64 位小写十六进制
     */
    [[nodiscard]] std::string fingerprintOf(X509 &certificate)
    {
        unsigned char digest[EVP_MAX_MD_SIZE]{};
        unsigned int  length = 0;
        requireStep(X509_digest(&certificate, EVP_sha256(), digest, &length) == 1, "计算证书指纹");
        requireStep(length == kFingerprintBytes, "指纹长度不是 32 字节");
        std::string text;
        text.reserve(64U);
        for (unsigned int index = 0; index < length; ++index)
        {
            text.push_back(kHexDigits[digest[index] >> 4U]);
            text.push_back(kHexDigits[digest[index] & 0x0FU]);
        }
        return text;
    }

    /**
     * @brief 构造已通过校验的对端身份
     * @param fingerprint 64 位指纹
     * @return AsynGyanis::Core::TlsPeerIdentityPtr 身份
     */
    [[nodiscard]] AsynGyanis::Core::TlsPeerIdentityPtr makeIdentity(const std::string &fingerprint)
    {
        return std::make_shared<const AsynGyanis::Core::TlsPeerIdentity>(true, std::vector<std::uint8_t>{}, fingerprint);
    }

    /**
     * @brief 构造 Agent 续期请求的 JSON
     * @param nodeId 节点标识
     * @param csrPem CSR 文本
     * @return std::string 请求正文
     */
    [[nodiscard]] std::string renewalBody(const std::int64_t nodeId, const std::string &csrPem)
    {
        return AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"csrPem", csrPem}}).dump();
    }

    /**
     * @brief 生成 Agent 指纹请求头
     * @param fingerprint 证书指纹
     * @return std::vector<std::pair<std::string, std::string>> 请求头
     */
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> agentHeaders(const std::string &fingerprint)
    {
        return {{"X-Test-Agent-Fingerprint", fingerprint}};
    }
} // namespace

// 钉住：active 与未过期 previous 都算匹配；previous 到期或节点吊销后一律不匹配
TEST(AgentCertificateMatchesNode, AcceptsActiveAndOverlappingPreviousOnly)
{
    const std::string             active(64, 'a');
    const std::string             previous(64, 'b');
    EdgePulse::Device::NodeRecord node;
    node.certFingerprint         = active;
    node.previousCertFingerprint = previous;
    node.previousCertNotAfter    = "2026-10-26 00:00:00";

    EXPECT_TRUE(EdgePulse::Device::certificateMatchesNode(makeIdentity(active), node, "2026-10-25 12:00:00"));
    EXPECT_TRUE(EdgePulse::Device::certificateMatchesNode(makeIdentity(std::string(64, 'A')), node, "2026-10-25 12:00:00"));
    EXPECT_TRUE(EdgePulse::Device::certificateMatchesNode(makeIdentity(previous), node, "2026-10-25 12:00:00"));
    EXPECT_FALSE(EdgePulse::Device::certificateMatchesNode(makeIdentity(previous), node, "2026-10-26 00:00:00"));
    EXPECT_FALSE(EdgePulse::Device::certificateMatchesNode(makeIdentity(std::string(64, 'c')), node, "2026-10-25 12:00:00"));
    EXPECT_FALSE(EdgePulse::Device::certificateMatchesNode(nullptr, node, "2026-10-25 12:00:00"));
    EXPECT_FALSE(EdgePulse::Device::certificateMatchesNode(std::make_shared<const AsynGyanis::Core::TlsPeerIdentity>(false, std::vector<std::uint8_t>{}, active), node,
                                                           "2026-10-25 12:00:00"));

    node.certRevokedAt = "2026-10-25 12:00:01";
    EXPECT_FALSE(EdgePulse::Device::certificateMatchesNode(makeIdentity(active), node, "2026-10-25 12:00:02"));
    EXPECT_FALSE(EdgePulse::Device::certificateMatchesNode(makeIdentity(previous), node, "2026-10-25 12:00:02"));
}

class AgentCertificateRenewalTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_context                             = std::make_unique<EdgePulse::TestSupport::AuthTestContext>();
        m_directory                           = std::make_unique<EdgePulse::TestSupport::TemporaryDirectory>();
        const auto [certificateFile, keyFile] = writeAuthority(*m_directory);
        m_caCertificateFile                   = certificateFile;
        m_caKeyFile                           = keyFile;

        m_nodes       = std::make_shared<EdgePulse::Device::NodeRepository>(m_context->database);
        m_tenants     = m_context->tenants;
        m_enrollRepo  = std::make_shared<EdgePulse::Device::EnrollmentRepository>(m_context->database);
        m_enrollments = std::make_shared<EdgePulse::Device::EnrollmentService>(m_enrollRepo);
        m_registry    = std::make_shared<EdgePulse::Device::AgentRegistry>(m_nodes, EdgePulse::Device::AgentRegistry::Options{});
        m_commands    = std::make_shared<EdgePulse::Command::CommandRepository>(m_context->database);
        m_channels    = std::make_shared<EdgePulse::Command::AgentChannelRegistry>();
        m_telemetry   = std::make_shared<EdgePulse::Telemetry::TelemetryRepository>(m_context->database);
        m_alarmRepo   = std::make_shared<EdgePulse::Alarm::AlarmRepository>(m_context->database);
        m_alarms      = std::make_shared<EdgePulse::Alarm::AlarmService>(m_alarmRepo);
    }

    void TearDown() override
    {
        m_server.reset();
        m_alarms.reset();
        m_alarmRepo.reset();
        m_telemetry.reset();
        m_channels.reset();
        m_commands.reset();
        m_registry.reset();
        m_enrollments.reset();
        m_enrollRepo.reset();
        m_nodes.reset();
        m_tenants.reset();
        m_context.reset();
        m_directory.reset();
    }

    /**
     * @brief 启动一个带 Agent 路由的明文测试服务器
     * @param caEnabled CA 是否启用
     * @param versionBumps 每次写入轮换前人为推进版本的次数，用于制造版本冲突
     * @param rotationHook 每次写入轮换前的附加动作，用于模拟改绑或租户停用等并发变化
     * @param requiredMode required 模式下注册必须持一次性令牌
     */
    void startServer(const bool caEnabled = true, const int versionBumps = 0, std::function<void(std::int64_t)> rotationHook = {}, const bool requiredMode = false)
    {
        if (requiredMode)
        {
            EdgePulse::Device::AgentRegistry::Options options;
            options.enrollmentMode = "required";
            options.enrollments    = m_enrollments.get();
            m_registry             = std::make_shared<EdgePulse::Device::AgentRegistry>(m_nodes, options);
        }

        EdgePulse::Device::AgentCaOptions options;
        options.enabled         = caEnabled;
        options.certificateFile = m_caCertificateFile;
        options.keyFile         = m_caKeyFile;
        options.validityDays    = 30;
        options.overlapHours    = 24;
        auto authority          = EdgePulse::Device::AgentCertificateAuthority::load(options);
        ASSERT_TRUE(authority.has_value()) << authority.error();
        m_authority.emplace(std::move(*authority));
        m_versionBumps = versionBumps;

        m_server = std::make_unique<EdgePulse::TestSupport::ServerHarness>(
                [this, rotationHook = std::move(rotationHook)](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop)
                {
                    EdgePulse::TestSupport::installTestAgentIdentity(router);
                    EdgePulse::Device::registerRoutes(router, loop, *m_registry, *m_nodes, *m_tenants, 30, nullptr, &*m_authority,
                                                      [this, rotationHook = std::move(rotationHook)](const std::int64_t nodeId)
                                                      {
                                                          if (rotationHook)
                                                          {
                                                              rotationHook(nodeId);
                                                          }
                                                          if (m_versionBumps <= 0)
                                                          {
                                                              return;
                                                          }
                                                          --m_versionBumps;
                                                          blockOnSql("UPDATE biz_node SET version = version + 1 WHERE id = " + std::to_string(nodeId));
                                                      });
                    EdgePulse::Command::CommandDependencies commandDependencies;
                    commandDependencies.nodes    = m_nodes.get();
                    commandDependencies.commands = m_commands.get();
                    EdgePulse::Command::registerCommandAgentRoutes(router, loop, commandDependencies);
                    EdgePulse::Telemetry::TelemetryDependencies telemetryDependencies;
                    telemetryDependencies.nodes     = m_nodes.get();
                    telemetryDependencies.telemetry = m_telemetry.get();
                    telemetryDependencies.alarms    = m_alarms.get();
                    EdgePulse::Telemetry::registerTelemetryAgentRoutes(router, loop, telemetryDependencies);
                });
    }

    /**
     * @brief 把给定指纹写成节点的一枚 active 证书
     * @param created 已创建的节点
     * @param fingerprint 证书指纹
     * @param notAfter 证书到期时间
     * @param serial 序列号
     * @param subject 审计主题
     * @param notBefore 生效时间
     * @return EdgePulse::Device::NodeRecord 轮换后的节点
     */
    [[nodiscard]] EdgePulse::Device::NodeRecord applyRotation(const EdgePulse::Device::NodeRecord &created, const std::string &fingerprint, const std::string &notAfter,
                                                              const std::string &serial, const std::string &subject, const std::string &notBefore)
    {
        EdgePulse::Device::AgentCertificateRotation rotation;
        rotation.fingerprint         = fingerprint;
        rotation.notAfter            = notAfter;
        rotation.previousFingerprint = created.certFingerprint;
        // 有旧证书时给出一个仍在重叠窗口内的截止时间，没有旧证书时仓储会自行清空该字段
        rotation.previousNotAfter =
                created.certFingerprint.empty() ? std::string{} : EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::hours{24});
        rotation.serial    = serial;
        rotation.subject   = subject;
        rotation.notBefore = notBefore;
        const auto rotated =
                m_runner.run(m_nodes->rotateCertificate(m_runner.loop(), EdgePulse::System::TenantScope{created.tenantId, false}, created.id, created.version, rotation));
        EXPECT_TRUE(rotated.has_value()) << (rotated.has_value() ? std::string{} : rotated.error());
        EXPECT_TRUE(rotated.has_value() && *rotated);
        const std::optional<EdgePulse::Device::NodeRecord> stored = m_runner.run(m_nodes->findById(m_runner.loop(), created.id));
        EXPECT_TRUE(stored.has_value());
        // 夹具里的轮换是既成事实，把签发时间推到过去以免最短续期间隔挡住后续请求
        blockOnSql("UPDATE biz_agent_certificate SET issued_at = '2026-01-01 00:00:00' WHERE node_id = " + std::to_string(created.id) + " AND status = 'active'");
        return stored.value_or(created);
    }

    /**
     * @brief 建一个节点并把给定指纹写成一枚 active 证书
     * @param name 节点名
     * @param fingerprint 证书指纹
     * @param notAfter 证书到期时间
     * @return EdgePulse::Device::NodeRecord 轮换后的节点
     */
    [[nodiscard]] EdgePulse::Device::NodeRecord seedNode(const std::string &name, const std::string &fingerprint, const std::string &notAfter)
    {
        const EdgePulse::Device::NodeRecord created = m_runner.run(m_nodes->create(m_runner.loop(), name, "{}"));
        return applyRotation(created, fingerprint, notAfter, "seed-serial-" + name, "CN=" + name, "2026-01-01 00:00:00");
    }

    /**
     * @brief 建节点并通过内部 CA 签发一枚真实的 active 证书
     * @param name 节点名
     * @return std::pair<EdgePulse::Device::NodeRecord, EdgePulse::Device::IssuedAgentCertificate> 节点与初始证书
     */
    [[nodiscard]] std::pair<EdgePulse::Device::NodeRecord, EdgePulse::Device::IssuedAgentCertificate> seedIssuedNode(const std::string &name)
    {
        const EdgePulse::Device::NodeRecord created = m_runner.run(m_nodes->create(m_runner.loop(), name, "{}"));
        KeyHandle                           key     = generateKey();
        const std::string                   csr     = makeCertificateRequest(*key, name);
        auto                                issued  = m_authority->issue(csr, created.id, name);
        EXPECT_TRUE(issued.has_value()) << (issued.has_value() ? std::string{} : issued.error().message);
        if (!issued.has_value())
        {
            return {created, EdgePulse::Device::IssuedAgentCertificate{}};
        }
        const EdgePulse::Device::NodeRecord seeded = applyRotation(created, issued->fingerprint, issued->notAfter, issued->serial, issued->subject, issued->notBefore);
        return {seeded, *issued};
    }

    /**
     * @brief 在测试线程上同步执行一条 SQL
     * @param sql SQL 文本
     */
    void blockOnSql(const std::string &sql)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(m_context->databasePath().string());
        EXPECT_NE(nullptr, connection);
        if (connection == nullptr)
        {
            return;
        }
        const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection->execute(sql);
        EXPECT_NE(nullptr, result) << connection->lastError();
    }

    /**
     * @brief 发一次续期请求
     * @param nodeId 节点标识
     * @param csrPem CSR 文本
     * @param fingerprint 连接证书指纹；空表示不带身份
     * @return EdgePulse::TestSupport::HttpResult 响应
     */
    [[nodiscard]] EdgePulse::TestSupport::HttpResult renew(const std::int64_t nodeId, const std::string &csrPem, const std::string &fingerprint)
    {
        const std::vector<std::pair<std::string, std::string>> headers = fingerprint.empty() ? std::vector<std::pair<std::string, std::string>>{} : agentHeaders(fingerprint);
        return m_server->post("/api/agent/certificate/renew", renewalBody(nodeId, csrPem), "application/json", headers);
    }

    /**
     * @brief 发一次心跳
     */
    [[nodiscard]] EdgePulse::TestSupport::HttpResult heartbeat(const std::int64_t nodeId, const std::string &fingerprint)
    {
        const std::string body = AsynGyanis::Base::ConfigValue::object({{"nodeId", nodeId}, {"agentVersion", "1.0.0"}}).dump();
        return m_server->post("/api/agent/heartbeat", body, "application/json", agentHeaders(fingerprint));
    }

    /**
     * @brief 取一条待执行命令
     */
    [[nodiscard]] EdgePulse::TestSupport::HttpResult nextCommand(const std::int64_t nodeId, const std::string &fingerprint)
    {
        return m_server->get("/api/agent/commands/next?nodeId=" + std::to_string(nodeId), agentHeaders(fingerprint));
    }

    /**
     * @brief 上报一条遥测样本
     */
    [[nodiscard]] EdgePulse::TestSupport::HttpResult telemetry(const std::int64_t nodeId, const std::string &fingerprint)
    {
        const std::string body =
                AsynGyanis::Base::ConfigValue::object(
                        {{"nodeId", nodeId}, {"samples", AsynGyanis::Base::ConfigValue::array({AsynGyanis::Base::ConfigValue::object({{"metric", "cpu"}, {"value", 1.0}})})}})
                        .dump();
        return m_server->post("/api/agent/telemetry", body, "application/json", agentHeaders(fingerprint));
    }

    /**
     * @brief 发一次 Agent 注册
     * @param name 节点名
     * @param fingerprint 连接证书指纹
     * @param token 注册/改绑令牌，可空
     * @return EdgePulse::TestSupport::HttpResult 响应
     */
    [[nodiscard]] EdgePulse::TestSupport::HttpResult registerNode(const std::string &name, const std::string &fingerprint, const std::string &token = {})
    {
        AsynGyanis::Base::ConfigValue body = AsynGyanis::Base::ConfigValue::object(
                {{"name", name}, {"agentVersion", "1.0.0"}, {"certFingerprint", fingerprint}, {"labels", AsynGyanis::Base::ConfigValue::object({})}});
        if (!token.empty())
        {
            body["enrollmentToken"] = token;
        }
        return m_server->post("/api/agent/register", body.dump(), "application/json", agentHeaders(fingerprint));
    }

    std::unique_ptr<EdgePulse::TestSupport::AuthTestContext>    m_context;
    EdgePulse::TestSupport::EventLoopRunner                     m_runner;
    std::unique_ptr<EdgePulse::TestSupport::TemporaryDirectory> m_directory;
    std::filesystem::path                                       m_caCertificateFile;
    std::filesystem::path                                       m_caKeyFile;
    std::optional<EdgePulse::Device::AgentCertificateAuthority> m_authority;
    std::shared_ptr<EdgePulse::Device::NodeRepository>          m_nodes;
    std::shared_ptr<EdgePulse::System::TenantRepository>        m_tenants;
    std::shared_ptr<EdgePulse::Device::EnrollmentRepository>    m_enrollRepo;
    std::shared_ptr<EdgePulse::Device::EnrollmentService>       m_enrollments;
    std::shared_ptr<EdgePulse::Device::AgentRegistry>           m_registry;
    std::shared_ptr<EdgePulse::Command::CommandRepository>      m_commands;
    std::shared_ptr<EdgePulse::Command::AgentChannelRegistry>   m_channels;
    std::shared_ptr<EdgePulse::Telemetry::TelemetryRepository>  m_telemetry;
    std::shared_ptr<EdgePulse::Alarm::AlarmRepository>          m_alarmRepo;
    std::shared_ptr<EdgePulse::Alarm::AlarmService>             m_alarms;
    std::unique_ptr<EdgePulse::TestSupport::ServerHarness>      m_server;
    int                                                         m_versionBumps{0};
};

// 钉住：没有身份或指纹不符都返回 403，且不进入签发
TEST_F(AgentCertificateRenewalTest, RejectsMissingOrMismatchedIdentity)
{
    startServer();
    const EdgePulse::Device::NodeRecord node = seedNode("renew-identity", std::string(64, 'a'), "2030-01-01 00:00:00");

    const auto missing = renew(node.id, "not-a-csr", "");
    EXPECT_EQ(403, missing.status) << missing.body;
    EXPECT_NE(std::string::npos, missing.body.find("必须出示"));

    const auto mismatched = renew(node.id, "not-a-csr", std::string(64, 'b'));
    EXPECT_EQ(403, mismatched.status) << mismatched.body;
    EXPECT_NE(std::string::npos, mismatched.body.find("不一致"));
}

// 钉住：未知节点返回 404，而不是 403
TEST_F(AgentCertificateRenewalTest, RejectsUnknownNode)
{
    startServer();
    const auto response = renew(999999, "not-a-csr", std::string(64, 'a'));
    EXPECT_EQ(404, response.status) << response.body;
    EXPECT_NE(std::string::npos, response.body.find("节点不存在"));
}

// 钉住：未启用 CA 时续期返回 503，且不尝试解析 CSR
TEST_F(AgentCertificateRenewalTest, ReturnsServiceUnavailableWhenCaDisabled)
{
    startServer(false);
    const EdgePulse::Device::NodeRecord node     = seedNode("renew-ca-disabled", std::string(64, 'a'), "2030-01-01 00:00:00");
    const auto                          response = renew(node.id, "not-a-csr", node.certFingerprint);
    EXPECT_EQ(503, response.status) << response.body;
    EXPECT_NE(std::string::npos, response.body.find("CA 未启用"));
}

// 钉住：非法 CSR 返回 400，节点证书保持不变
TEST_F(AgentCertificateRenewalTest, RejectsMalformedCsr)
{
    startServer();
    const EdgePulse::Device::NodeRecord node = seedNode("renew-bad-csr", std::string(64, 'a'), "2030-01-01 00:00:00");

    const auto missing = renew(node.id, "", node.certFingerprint);
    EXPECT_EQ(400, missing.status) << missing.body;

    const auto malformed = renew(node.id, "-----BEGIN CERTIFICATE REQUEST-----\nbm90IGEgY3Ny\n-----END CERTIFICATE REQUEST-----\n", node.certFingerprint);
    EXPECT_EQ(400, malformed.status) << malformed.body;

    const std::optional<EdgePulse::Device::NodeRecord> stored = m_runner.run(m_nodes->findById(m_runner.loop(), node.id));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(node.certFingerprint, stored->certFingerprint);
    EXPECT_TRUE(stored->previousCertFingerprint.empty());
}

// 钉住：租户停用后已登记证书也不能续期
TEST_F(AgentCertificateRenewalTest, RejectsDisabledTenant)
{
    startServer();
    const EdgePulse::Device::NodeRecord node = seedNode("renew-tenant-off", std::string(64, 'a'), "2030-01-01 00:00:00");
    ASSERT_TRUE(m_runner.run(m_tenants->update(m_runner.loop(), 1, "", 0, "")));

    const auto response = renew(node.id, "not-a-csr", node.certFingerprint);
    EXPECT_EQ(403, response.status) << response.body;
    EXPECT_NE(std::string::npos, response.body.find("租户已停用"));
}

// 钉住：续期返回新证书、写回 active/previous 与历史，且新旧证书都能走完整 Agent 面
TEST_F(AgentCertificateRenewalTest, RenewsAndBothFingerprintsWorkWithinOverlap)
{
    startServer();
    auto [node, initial] = seedIssuedNode("renew-main");
    ASSERT_FALSE(initial.fingerprint.empty());

    KeyHandle         newKey   = generateKey();
    const std::string newCsr   = makeCertificateRequest(*newKey, "ignored-by-server");
    const std::string before   = EdgePulse::Common::nowDateTimeUtc();
    const auto        response = renew(node.id, newCsr, initial.fingerprint);
    ASSERT_EQ(200, response.status) << response.body;
    EXPECT_EQ("no-store", response.header("Cache-Control").value_or(""));

    const auto body = response.json();
    ASSERT_TRUE(body.has_value()) << response.body;
    const auto       &data           = (*body)["data"];
    const std::string newFingerprint = data["fingerprint"].get<std::string>();
    const std::string certificatePem = data["certificatePem"].get<std::string>();
    const std::string notBefore      = data["notBefore"].get<std::string>();
    const std::string notAfter       = data["notAfter"].get<std::string>();
    const std::string serial         = data["serial"].get<std::string>();
    EXPECT_EQ(64U, newFingerprint.size());
    EXPECT_NE(initial.fingerprint, newFingerprint);
    EXPECT_FALSE(notBefore.empty());
    EXPECT_FALSE(notAfter.empty());
    EXPECT_FALSE(serial.empty());

    CertHandle certificate = parseCertificate(certificatePem);
    EXPECT_EQ(newFingerprint, fingerprintOf(*certificate));

    const std::optional<EdgePulse::Device::NodeRecord> stored = m_runner.run(m_nodes->findById(m_runner.loop(), node.id));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(newFingerprint, stored->certFingerprint);
    EXPECT_EQ(notAfter, stored->certNotAfter);
    EXPECT_EQ(initial.fingerprint, stored->previousCertFingerprint);
    EXPECT_FALSE(stored->previousCertNotAfter.empty());
    EXPECT_LT(before, stored->previousCertNotAfter);
    const std::string cutoff = EdgePulse::Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::hours{24});
    EXPECT_LE(stored->previousCertNotAfter, cutoff);
    EXPECT_LT(node.version, stored->version);

    const auto                                                   certificates = std::make_shared<EdgePulse::Device::AgentCertificateRepository>(m_context->database);
    const std::vector<EdgePulse::Device::AgentCertificateRecord> history      = m_runner.run(certificates->listByNode(m_runner.loop(), node.id));
    ASSERT_EQ(2U, history.size());
    EXPECT_EQ("active", history[0].status);
    EXPECT_EQ(newFingerprint, history[0].fingerprint);
    EXPECT_EQ(serial, history[0].serial);
    EXPECT_EQ("previous", history[1].status);
    EXPECT_EQ(initial.fingerprint, history[1].fingerprint);

    const auto created = m_runner.run(m_commands->create(m_runner.loop(), node.id, "reboot", "{}", 1, 30));
    ASSERT_TRUE(created.has_value());

    for (const std::string &fingerprint: {newFingerprint, initial.fingerprint})
    {
        EXPECT_EQ(200, heartbeat(node.id, fingerprint).status) << fingerprint;
        EXPECT_EQ(200, nextCommand(node.id, fingerprint).status) << fingerprint;
        EXPECT_EQ(200, telemetry(node.id, fingerprint).status) << fingerprint;
        const auto command = m_runner.run(m_commands->create(m_runner.loop(), node.id, "reboot", "{}", 1, 30));
        ASSERT_TRUE(command.has_value());
    }
}

// 钉住：同一节点两次签发之间至少间隔 60 秒，判据来自签发历史而不是进程内状态
TEST_F(AgentCertificateRenewalTest, ThrottlesRenewalWithinMinimumInterval)
{
    startServer();
    auto [node, initial] = seedIssuedNode("renew-throttle");
    ASSERT_FALSE(initial.fingerprint.empty());

    KeyHandle         firstKey = generateKey();
    const std::string firstCsr = makeCertificateRequest(*firstKey, "ignored-by-server");
    const auto        first    = renew(node.id, firstCsr, initial.fingerprint);
    ASSERT_EQ(200, first.status) << first.body;
    const std::string firstFingerprint = (*first.json())["data"]["fingerprint"].get<std::string>();

    KeyHandle         secondKey = generateKey();
    const std::string secondCsr = makeCertificateRequest(*secondKey, "ignored-by-server");
    const auto        throttled = renew(node.id, secondCsr, firstFingerprint);
    EXPECT_EQ(429, throttled.status) << throttled.body;
    EXPECT_NE(std::string::npos, throttled.body.find("频繁"));

    // 把签发历史推到过去，证明限制只取决于持久化时间而不是进程内计数
    blockOnSql("UPDATE biz_agent_certificate SET issued_at = '2026-01-01 00:00:00' WHERE node_id = " + std::to_string(node.id));
    const auto allowed = renew(node.id, secondCsr, firstFingerprint);
    EXPECT_EQ(200, allowed.status) << allowed.body;
}

// 钉住：重叠窗口过期后旧指纹失败，新证书仍有效
TEST_F(AgentCertificateRenewalTest, RejectsOldFingerprintAfterOverlapExpires)
{
    startServer();
    auto [node, initial] = seedIssuedNode("renew-expiry");
    ASSERT_FALSE(initial.fingerprint.empty());

    KeyHandle         newKey  = generateKey();
    const std::string newCsr  = makeCertificateRequest(*newKey, "ignored-by-server");
    const auto        renewed = renew(node.id, newCsr, initial.fingerprint);
    ASSERT_EQ(200, renewed.status) << renewed.body;
    const std::string newFingerprint = (*renewed.json())["data"]["fingerprint"].get<std::string>();

    blockOnSql("UPDATE biz_node SET previous_cert_not_after = '2000-01-01 00:00:00' WHERE id = " + std::to_string(node.id));

    EXPECT_EQ(403, heartbeat(node.id, initial.fingerprint).status);
    EXPECT_EQ(403, nextCommand(node.id, initial.fingerprint).status);
    EXPECT_EQ(403, telemetry(node.id, initial.fingerprint).status);
    EXPECT_EQ(403, renew(node.id, newCsr, initial.fingerprint).status);
    EXPECT_EQ(200, heartbeat(node.id, newFingerprint).status);
}

// 钉住：吊销同时清空 active/previous 并把历史行标为 revoked，之后续期与两种指纹都被拒
TEST_F(AgentCertificateRenewalTest, RevocationClearsBothFingerprintsAndHistory)
{
    startServer();
    auto [node, initial] = seedIssuedNode("renew-revoked");
    ASSERT_FALSE(initial.fingerprint.empty());

    KeyHandle         newKey  = generateKey();
    const std::string newCsr  = makeCertificateRequest(*newKey, "ignored-by-server");
    const auto        renewed = renew(node.id, newCsr, initial.fingerprint);
    ASSERT_EQ(200, renewed.status) << renewed.body;
    const std::string newFingerprint = (*renewed.json())["data"]["fingerprint"].get<std::string>();

    const std::optional<EdgePulse::Device::NodeRecord> revoked = m_runner.run(m_nodes->revokeCertificate(m_runner.loop(), node.id));
    ASSERT_TRUE(revoked.has_value());
    EXPECT_TRUE(revoked->certFingerprint.empty());
    EXPECT_TRUE(revoked->previousCertFingerprint.empty());
    EXPECT_TRUE(revoked->previousCertNotAfter.empty());
    EXPECT_FALSE(revoked->certRevokedAt.empty());

    const auto                                                   certificates = std::make_shared<EdgePulse::Device::AgentCertificateRepository>(m_context->database);
    const std::vector<EdgePulse::Device::AgentCertificateRecord> history      = m_runner.run(certificates->listByNode(m_runner.loop(), node.id));
    ASSERT_EQ(2U, history.size());
    for (const auto &record: history)
    {
        EXPECT_EQ("revoked", record.status);
        EXPECT_FALSE(record.revokedAt.empty());
    }

    EXPECT_EQ(403, renew(node.id, newCsr, newFingerprint).status);
    EXPECT_EQ(403, heartbeat(node.id, newFingerprint).status);
    EXPECT_EQ(403, heartbeat(node.id, initial.fingerprint).status);
}

// 钉住：版本冲突后重新读取并重试一次即可成功
TEST_F(AgentCertificateRenewalTest, RetriesOnceAfterVersionConflict)
{
    startServer(true, 1);
    const EdgePulse::Device::NodeRecord node = seedNode("renew-retry", std::string(64, 'a'), "2030-01-01 00:00:00");

    KeyHandle         newKey   = generateKey();
    const std::string newCsr   = makeCertificateRequest(*newKey, "ignored-by-server");
    const auto        response = renew(node.id, newCsr, node.certFingerprint);
    ASSERT_EQ(200, response.status) << response.body;
    const std::string newFingerprint = (*response.json())["data"]["fingerprint"].get<std::string>();

    const std::optional<EdgePulse::Device::NodeRecord> stored = m_runner.run(m_nodes->findById(m_runner.loop(), node.id));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(newFingerprint, stored->certFingerprint);
    EXPECT_EQ(node.certFingerprint, stored->previousCertFingerprint);
}

// 钉住：版本冲突重试前重新校验请求身份，改绑后的新证书不会被旧证书持有者覆盖
TEST_F(AgentCertificateRenewalTest, DoesNotOverwriteCertificateReboundBeforeRetry)
{
    const std::string reboundFingerprint(64, 'c');
    bool              rebound = false;
    startServer(true, 0,
                [this, &rebound, &reboundFingerprint](const std::int64_t nodeId)
                {
                    if (rebound)
                    {
                        return;
                    }
                    rebound = true;
                    blockOnSql("UPDATE biz_node SET cert_fingerprint = '" + reboundFingerprint +
                               "', previous_cert_fingerprint = '', previous_cert_not_after = NULL, "
                               "version = version + 1 WHERE id = " +
                               std::to_string(nodeId));
                });
    const EdgePulse::Device::NodeRecord node = seedNode("renew-rebind", std::string(64, 'a'), "2030-01-01 00:00:00");

    KeyHandle         newKey   = generateKey();
    const std::string newCsr   = makeCertificateRequest(*newKey, "ignored-by-server");
    const auto        response = renew(node.id, newCsr, node.certFingerprint);
    EXPECT_EQ(403, response.status) << response.body;
    EXPECT_NE(std::string::npos, response.body.find("不一致"));

    const std::optional<EdgePulse::Device::NodeRecord> stored = m_runner.run(m_nodes->findById(m_runner.loop(), node.id));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(reboundFingerprint, stored->certFingerprint);
    EXPECT_TRUE(stored->previousCertFingerprint.empty());

    // 改绑后的新证书仍能代表节点，说明写入没有被旧证书持有者截走
    EXPECT_EQ(200, heartbeat(node.id, reboundFingerprint).status);
}

// 钉住：版本冲突重试前重新检查租户状态，停用租户不能借重试完成续期
TEST_F(AgentCertificateRenewalTest, RejectsTenantDisabledBeforeRetry)
{
    bool disabled = false;
    startServer(true, 0,
                [this, &disabled](const std::int64_t nodeId)
                {
                    if (disabled)
                    {
                        return;
                    }
                    disabled = true;
                    blockOnSql("UPDATE biz_node SET version = version + 1 WHERE id = " + std::to_string(nodeId));
                    blockOnSql("UPDATE sys_tenant SET status = 0 WHERE id = 1");
                });
    const EdgePulse::Device::NodeRecord node = seedNode("renew-tenant-retry", std::string(64, 'a'), "2030-01-01 00:00:00");

    KeyHandle         newKey   = generateKey();
    const std::string newCsr   = makeCertificateRequest(*newKey, "ignored-by-server");
    const auto        response = renew(node.id, newCsr, node.certFingerprint);
    EXPECT_EQ(403, response.status) << response.body;
    EXPECT_NE(std::string::npos, response.body.find("租户已停用"));

    const std::optional<EdgePulse::Device::NodeRecord> stored = m_runner.run(m_nodes->findById(m_runner.loop(), node.id));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(node.certFingerprint, stored->certFingerprint);
    EXPECT_TRUE(stored->previousCertFingerprint.empty());
}

// 钉住：连续两次版本冲突返回 409，且不写入任何证书字段
TEST_F(AgentCertificateRenewalTest, ReturnsConflictAfterRepeatedVersionConflict)
{
    startServer(true, 2);
    const EdgePulse::Device::NodeRecord node = seedNode("renew-conflict", std::string(64, 'a'), "2030-01-01 00:00:00");

    KeyHandle         newKey   = generateKey();
    const std::string newCsr   = makeCertificateRequest(*newKey, "ignored-by-server");
    const auto        response = renew(node.id, newCsr, node.certFingerprint);
    EXPECT_EQ(409, response.status) << response.body;

    const std::optional<EdgePulse::Device::NodeRecord> stored = m_runner.run(m_nodes->findById(m_runner.loop(), node.id));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(node.certFingerprint, stored->certFingerprint);
    EXPECT_TRUE(stored->previousCertFingerprint.empty());
}

// 钉住：服务端已轮换而 Agent 没装上新证书时，重启出示上一枚证书应按同证书软重登记，
// 不消费令牌、不把 previous 提回 active，且心跳与续期继续可用
TEST_F(AgentCertificateRenewalTest, RestartWithPreviousCertificateRegistersWithoutToken)
{
    startServer(true, 0, {}, true);
    const std::string fingerprintA(64, 'a');
    const std::string fingerprintB(64, 'b');

    const auto issued = m_runner.run(m_enrollments->create(m_runner.loop(), 1, "renew-restart", "enroll", 3600, 1));
    ASSERT_TRUE(issued.has_value()) << issued.error();
    const auto first = registerNode("renew-restart", fingerprintA, issued->plaintext);
    ASSERT_EQ(200, first.status) << first.body;
    const std::int64_t nodeId = (*first.json())["data"]["id"].get<std::int64_t>();

    // 服务端已把 active 换成 B，并把 A 放进重叠窗口；Agent 死在装上新证书之前
    const std::optional<EdgePulse::Device::NodeRecord> created = m_runner.run(m_nodes->findById(m_runner.loop(), nodeId));
    ASSERT_TRUE(created.has_value());
    const EdgePulse::Device::NodeRecord rotated = applyRotation(*created, fingerprintB, "2030-01-01 00:00:00", "serial-b", "CN=renew-restart", "2026-01-01 00:00:00");
    ASSERT_EQ(fingerprintB, rotated.certFingerprint);
    ASSERT_EQ(fingerprintA, rotated.previousCertFingerprint);

    const auto restarted = registerNode("renew-restart", fingerprintA);
    ASSERT_EQ(200, restarted.status) << restarted.body;

    const std::optional<EdgePulse::Device::NodeRecord> stored = m_runner.run(m_nodes->findById(m_runner.loop(), nodeId));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(fingerprintB, stored->certFingerprint) << "上一枚证书不能被提回 active";
    EXPECT_EQ(fingerprintA, stored->previousCertFingerprint) << "重叠窗口字段不能被软重登记清掉";
    EXPECT_FALSE(stored->previousCertNotAfter.empty());
    EXPECT_GT(stored->version, rotated.version);

    EXPECT_EQ(200, heartbeat(nodeId, fingerprintA).status) << "上一枚证书在窗口内必须能心跳";

    KeyHandle         newKey  = generateKey();
    const std::string newCsr  = makeCertificateRequest(*newKey, "ignored-by-server");
    const auto        renewed = renew(nodeId, newCsr, fingerprintA);
    EXPECT_EQ(200, renewed.status) << renewed.body;
}

// 钉住：上一枚证书已出重叠窗口时不再享受软重登记，仍必须持改绑令牌
TEST_F(AgentCertificateRenewalTest, ExpiredPreviousCertificateStillNeedsToken)
{
    startServer(true, 0, {}, true);
    const std::string fingerprintA(64, 'a');
    const std::string fingerprintB(64, 'b');

    const auto issued = m_runner.run(m_enrollments->create(m_runner.loop(), 1, "renew-expired-previous", "enroll", 3600, 1));
    ASSERT_TRUE(issued.has_value()) << issued.error();
    const auto first = registerNode("renew-expired-previous", fingerprintA, issued->plaintext);
    ASSERT_EQ(200, first.status) << first.body;
    const std::int64_t nodeId = (*first.json())["data"]["id"].get<std::int64_t>();

    const std::optional<EdgePulse::Device::NodeRecord> created = m_runner.run(m_nodes->findById(m_runner.loop(), nodeId));
    ASSERT_TRUE(created.has_value());
    ASSERT_EQ(fingerprintB, applyRotation(*created, fingerprintB, "2030-01-01 00:00:00", "serial-b", "CN=renew-expired-previous", "2026-01-01 00:00:00").certFingerprint);
    blockOnSql("UPDATE biz_node SET previous_cert_not_after = '2000-01-01 00:00:00' WHERE id = " + std::to_string(nodeId));

    const auto rejected = registerNode("renew-expired-previous", fingerprintA);
    EXPECT_EQ(403, rejected.status) << rejected.body;
    EXPECT_NE(std::string::npos, rejected.body.find("改绑令牌"));
}

// 钉住：节点 JSON 同时暴露上一枚证书的指纹与重叠截止时间
TEST_F(AgentCertificateRenewalTest, NodeJsonExposesPreviousCertificateFields)
{
    startServer();
    auto [node, initial] = seedIssuedNode("renew-json");
    ASSERT_FALSE(initial.fingerprint.empty());

    KeyHandle         newKey = generateKey();
    const std::string newCsr = makeCertificateRequest(*newKey, "ignored-by-server");
    ASSERT_EQ(200, renew(node.id, newCsr, initial.fingerprint).status);

    const std::optional<EdgePulse::Device::NodeRecord> stored = m_runner.run(m_nodes->findById(m_runner.loop(), node.id));
    ASSERT_TRUE(stored.has_value());
    const AsynGyanis::Base::ConfigValue json = EdgePulse::Device::nodeToJson(*stored);
    EXPECT_EQ(initial.fingerprint, json["previousCertFingerprint"].get<std::string>());
    EXPECT_EQ(stored->previousCertNotAfter, json["previousCertNotAfter"].get<std::string>());
    EXPECT_TRUE(json["certFingerprint"].is_string());
}
