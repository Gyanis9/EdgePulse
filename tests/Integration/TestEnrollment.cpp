#include "Base/Database/Database.h"
#include "Core/Device/AgentRegistry.h"
#include "Core/Device/EnrollmentRepository.h"
#include "Core/Device/EnrollmentService.h"
#include "Core/Device/NodeRepository.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <Core/Crypto/Digest.h>
#include <Core/Tls/TlsPeerIdentity.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <variant>
#include <vector>

// 钉住：令牌只返回一次明文，库里只存哈希；一次性消费后不能复用
TEST(Enrollment, IssuesHashesAndConsumesOnce)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    repository = std::make_shared<EdgePulse::Device::EnrollmentRepository>(context.database);
    EdgePulse::Device::EnrollmentService    service(repository);

    const auto issued = runner.run(service.create(runner.loop(), 1, "edge-enroll", "enroll", 3600, 1));
    ASSERT_TRUE(issued.has_value()) << issued.error();
    EXPECT_TRUE(issued->plaintext.starts_with("ep_enroll_"));
    EXPECT_EQ(64U, issued->plaintext.size() - 10U);
    EXPECT_EQ("edge-enroll", issued->record.nodeName);
    EXPECT_EQ("enroll", issued->record.action);

    const auto listed = runner.run(service.list(runner.loop(), "active"));
    ASSERT_EQ(1U, listed.size());
    EXPECT_EQ(issued->record.id, listed[0].id);
    EXPECT_TRUE(listed[0].usedAt.empty());
    EXPECT_TRUE(listed[0].revokedAt.empty());

    // 直接查库：落盘值必须是摘要，且不能等于明文
    const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(context.databasePath().string());
    const auto rows = connection->execute("SELECT token_hash FROM biz_agent_enrollment WHERE id = " + std::to_string(issued->record.id));
    ASSERT_NE(nullptr, rows) << connection->lastError();
    ASSERT_TRUE(rows->next());
    const std::string storedHash = std::get<std::string>(rows->getValue(0));
    EXPECT_NE(issued->plaintext, storedHash);
    EXPECT_EQ(AsynGyanis::Core::Digest::sha256Hex(issued->plaintext), storedHash);

    EXPECT_TRUE(runner.run(service.revoke(runner.loop(), issued->record.id)));
    EXPECT_TRUE(runner.run(service.list(runner.loop(), "active")).empty());
    const auto revoked = runner.run(service.list(runner.loop(), "revoked"));
    ASSERT_EQ(1U, revoked.size());
    EXPECT_FALSE(revoked[0].revokedAt.empty());
    // 已吊销的令牌不能再次吊销，也不能再出现在待用列表里
    EXPECT_FALSE(runner.run(service.revoke(runner.loop(), issued->record.id)));
}

// 钉住：创建参数越界时拒绝，不产生半条记录
TEST(Enrollment, RejectsInvalidCreateParameters)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    repository = std::make_shared<EdgePulse::Device::EnrollmentRepository>(context.database);
    EdgePulse::Device::EnrollmentService    service(repository);

    EXPECT_FALSE(runner.run(service.create(runner.loop(), 1, "edge", "rename", 3600, 1)).has_value());
    EXPECT_FALSE(runner.run(service.create(runner.loop(), 1, std::string(129, 'a'), "enroll", 3600, 1)).has_value());
    EXPECT_FALSE(runner.run(service.create(runner.loop(), 1, "edge", "enroll", 299, 1)).has_value());
    EXPECT_FALSE(runner.run(service.create(runner.loop(), 1, "edge", "enroll", 2592001, 1)).has_value());
    EXPECT_TRUE(runner.run(service.list(runner.loop(), "")).empty());
}

// 钉住：required 模式下新节点必须持 enroll 令牌；同指纹重注册放行；异指纹必须 rebind
TEST(Enrollment, RequiredModeEnforcesEnrollAndRebind)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    nodes   = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto                                    repo    = std::make_shared<EdgePulse::Device::EnrollmentRepository>(context.database);
    auto                                    service = std::make_shared<EdgePulse::Device::EnrollmentService>(repo);

    EdgePulse::Device::AgentRegistry::Options options;
    options.enrollmentMode = "required";
    options.enrollments    = service.get();
    EdgePulse::Device::AgentRegistry registry(nodes, options);

    const std::string fingerprintA(64, 'a');
    const std::string fingerprintB(64, 'b');
    const auto        identityA = std::make_shared<const AsynGyanis::Core::TlsPeerIdentity>(true, std::vector<std::uint8_t>{}, fingerprintA);

    EdgePulse::Device::AgentRegistration missing;
    missing.name                    = "edge-required";
    missing.agentVersion            = "1.0.0";
    missing.certFingerprint         = fingerprintA;
    missing.connectionIdentity      = identityA;
    const auto rejectedWithoutToken = runner.run(registry.registerAgent(runner.loop(), missing));
    ASSERT_FALSE(rejectedWithoutToken.has_value());
    EXPECT_EQ(EdgePulse::Device::AgentError::Reason::FingerprintRejected, rejectedWithoutToken.error().reason);

    const auto issued = runner.run(service->create(runner.loop(), 1, "edge-required", "enroll", 3600, 1));
    ASSERT_TRUE(issued.has_value());
    EdgePulse::Device::AgentRegistration first = missing;
    first.enrollmentToken                      = issued->plaintext;
    const auto registered                      = runner.run(registry.registerAgent(runner.loop(), first));
    ASSERT_TRUE(registered.has_value()) << registered.error().message;

    // 同指纹重注册：不需要令牌
    EdgePulse::Device::AgentRegistration again    = missing;
    const auto                           repeated = runner.run(registry.registerAgent(runner.loop(), again));
    ASSERT_TRUE(repeated.has_value()) << repeated.error().message;

    // 异指纹：没有 rebind 令牌拒绝
    const auto                           identityB = std::make_shared<const AsynGyanis::Core::TlsPeerIdentity>(true, std::vector<std::uint8_t>{}, fingerprintB);
    EdgePulse::Device::AgentRegistration rebind    = missing;
    rebind.certFingerprint                         = fingerprintB;
    rebind.connectionIdentity                      = identityB;
    const auto rejectedRebind                      = runner.run(registry.registerAgent(runner.loop(), rebind));
    ASSERT_FALSE(rejectedRebind.has_value());
    EXPECT_EQ(EdgePulse::Device::AgentError::Reason::FingerprintRejected, rejectedRebind.error().reason);

    const auto issuedRebind = runner.run(service->create(runner.loop(), 1, "edge-required", "rebind", 3600, 1));
    ASSERT_TRUE(issuedRebind.has_value());
    rebind.enrollmentToken = issuedRebind->plaintext;
    const auto rebound     = runner.run(registry.registerAgent(runner.loop(), rebind));
    ASSERT_TRUE(rebound.has_value()) << rebound.error().message;
    EXPECT_EQ(fingerprintB, rebound->certFingerprint);

    // 同一枚改绑令牌已经消费，第二次改绑必须被拒
    EdgePulse::Device::AgentRegistration third = missing;
    third.certFingerprint                      = fingerprintA;
    third.connectionIdentity                   = identityA;
    third.enrollmentToken                      = issuedRebind->plaintext;
    const auto reusedToken                     = runner.run(registry.registerAgent(runner.loop(), third));
    ASSERT_FALSE(reusedToken.has_value());
    EXPECT_EQ(EdgePulse::Device::AgentError::Reason::FingerprintRejected, reusedToken.error().reason);
}

// 钉住：令牌动作与节点状态不匹配时拒绝，不能把改绑令牌当作新节点入场券
TEST(Enrollment, RejectsTokenActionMismatch)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    nodes   = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto                                    repo    = std::make_shared<EdgePulse::Device::EnrollmentRepository>(context.database);
    auto                                    service = std::make_shared<EdgePulse::Device::EnrollmentService>(repo);

    EdgePulse::Device::AgentRegistry::Options options;
    options.enrollmentMode = "required";
    options.enrollments    = service.get();
    EdgePulse::Device::AgentRegistry registry(nodes, options);

    const auto issued = runner.run(service->create(runner.loop(), 1, "edge-mismatch", "rebind", 3600, 1));
    ASSERT_TRUE(issued.has_value());

    EdgePulse::Device::AgentRegistration registration;
    registration.name            = "edge-mismatch";
    registration.agentVersion    = "1.0.0";
    registration.certFingerprint = std::string(64, 'c');
    registration.enrollmentToken = issued->plaintext;
    const auto rejected          = runner.run(registry.registerAgent(runner.loop(), registration));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(EdgePulse::Device::AgentError::Reason::FingerprintRejected, rejected.error().reason);

    // 令牌没有被消费，仍可用于后续真正的改绑
    const auto active = runner.run(service->list(runner.loop(), "active"));
    ASSERT_EQ(1U, active.size());
    EXPECT_EQ(issued->record.id, active[0].id);
}
