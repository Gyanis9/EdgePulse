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

#include <barrier>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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

    const std::string                          fingerprint(64, 'c');
    const AsynGyanis::Core::TlsPeerIdentityPtr identity = std::make_shared<const AsynGyanis::Core::TlsPeerIdentity>(true, std::vector<std::uint8_t>{}, fingerprint);
    EdgePulse::Device::AgentRegistration       registration;
    registration.name               = "edge-mismatch";
    registration.agentVersion       = "1.0.0";
    registration.certFingerprint    = fingerprint;
    registration.enrollmentToken    = issued->plaintext;
    registration.connectionIdentity = identity;
    const auto rejected             = runner.run(registry.registerAgent(runner.loop(), registration));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(EdgePulse::Device::AgentError::Reason::FingerprintRejected, rejected.error().reason);

    // 令牌没有被消费，仍可用于后续真正的改绑
    const auto active = runner.run(service->list(runner.loop(), "active"));
    ASSERT_EQ(1U, active.size());
    EXPECT_EQ(issued->record.id, active[0].id);
}

// 钉住：节点名被软删除记录占着时，同名注册给出可操作的 409，而不是永远重试的并发冲突
TEST(Enrollment, ReRegistrationAfterSoftDeleteReportsNameConflict)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    EdgePulse::Device::AgentRegistry     registry(nodes, {});
    EdgePulse::Device::AgentRegistration registration{"edge-deleted", "1.0.0", std::string(64, 'd'), "{}"};
    const auto                           first = runner.run(registry.registerAgent(runner.loop(), registration));
    ASSERT_TRUE(first.has_value()) << first.error().message;

    ASSERT_TRUE(runner.run(nodes->remove(runner.loop(), first->id)));

    const auto rejected = runner.run(registry.registerAgent(runner.loop(), registration));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(EdgePulse::Device::AgentError::Reason::Conflict, rejected.error().reason);
    EXPECT_EQ("节点名已被其他租户或已删除节点占用，请改名或联系管理员清理", rejected.error().message);
    EXPECT_EQ(std::string::npos, rejected.error().message.find("并发"));
}

// 钉住：同名活节点落在其他租户时同样返回名字冲突，不能退化成永远重试的并发冲突
TEST(Enrollment, LiveCrossTenantNameReportsNameConflict)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    EdgePulse::Device::AgentUpsert first;
    first.name            = "edge-cross-tenant";
    first.agentVersion    = "1.0.0";
    first.certFingerprint = std::string(64, 'e');
    first.labelsJson      = "{}";
    first.enrollmentMode  = "off";
    first.tenantId        = 1;
    const auto created    = runner.run(nodes->upsertAgent(runner.loop(), first));
    ASSERT_TRUE(created.has_value());
    EXPECT_TRUE(created->created);

    // 租户 2 的注册看不到租户 1 的活节点，插入会撞全局唯一索引；必须按名字冲突返回
    EdgePulse::Device::AgentUpsert conflicting = first;
    conflicting.certFingerprint                = std::string(64, 'f');
    conflicting.tenantId                       = 2;
    const auto rejected                        = runner.run(nodes->upsertAgent(runner.loop(), conflicting));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(EdgePulse::Device::AgentUpsertFailure::NameConflict, rejected.error());

    // 经注册表返回时文案要能指导改名或清理，不能退化成「稍后重试」的并发冲突
    EdgePulse::Device::AgentRegistry     registry(nodes, {});
    EdgePulse::Device::AgentRegistration fromOtherTenant{"edge-cross-tenant", "1.0.0", std::string(64, 'f'), "{}"};
    fromOtherTenant.tenantId = 2;
    const auto rejectedByRegistry = runner.run(registry.registerAgent(runner.loop(), fromOtherTenant));
    ASSERT_FALSE(rejectedByRegistry.has_value());
    EXPECT_EQ(EdgePulse::Device::AgentError::Reason::Conflict, rejectedByRegistry.error().reason);
    EXPECT_EQ("节点名已被其他租户或已删除节点占用，请改名或联系管理员清理", rejectedByRegistry.error().message);
    EXPECT_EQ(std::string::npos, rejectedByRegistry.error().message.find("并发"));
}

// 钉住：enroll/rebind 由仓储在事务内判定；调用方给的旧快照不能当放行依据，吊销节点必须重新持令牌
TEST(Enrollment, TransactionOwnsEnrollRebindDecision)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    nodes   = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto                                    repo    = std::make_shared<EdgePulse::Device::EnrollmentRepository>(context.database);
    auto                                    service = std::make_shared<EdgePulse::Device::EnrollmentService>(repo);

    const std::string              fingerprintA(64, 'a');
    const std::string              fingerprintB(64, 'b');
    EdgePulse::Device::AgentUpsert seed;
    seed.name            = "edge-txn";
    seed.agentVersion    = "1.0.0";
    seed.certFingerprint = fingerprintA;
    seed.labelsJson      = "{}";
    seed.enrollmentMode  = "off";
    const auto seeded    = runner.run(nodes->upsertAgent(runner.loop(), seed));
    ASSERT_TRUE(seeded.has_value());
    EXPECT_TRUE(seeded->created);

    // 已存在的节点只接受 rebind：enroll 令牌即使明面上有效也不能消费
    const auto enrollToken = runner.run(service->create(runner.loop(), 1, "edge-txn", "enroll", 3600, 1));
    ASSERT_TRUE(enrollToken.has_value());
    EdgePulse::Device::AgentUpsert enrollAttempt = seed;
    enrollAttempt.certFingerprint                = fingerprintB;
    enrollAttempt.enrollmentMode                 = "required";
    enrollAttempt.enrollmentTokenHash            = EdgePulse::Device::EnrollmentService::hashToken(enrollToken->plaintext);
    const auto rejected                          = runner.run(nodes->upsertAgent(runner.loop(), enrollAttempt));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(EdgePulse::Device::AgentUpsertFailure::TokenRejected, rejected.error());
    const auto stillActive = runner.run(service->list(runner.loop(), "active"));
    ASSERT_EQ(1U, stillActive.size());
    EXPECT_EQ(enrollToken->record.id, stillActive[0].id);

    const auto rebindToken = runner.run(service->create(runner.loop(), 1, "edge-txn", "rebind", 3600, 1));
    ASSERT_TRUE(rebindToken.has_value());
    enrollAttempt.enrollmentTokenHash = EdgePulse::Device::EnrollmentService::hashToken(rebindToken->plaintext);
    const auto rebound                = runner.run(nodes->upsertAgent(runner.loop(), enrollAttempt));
    ASSERT_TRUE(rebound.has_value());
    EXPECT_FALSE(rebound->created);
    EXPECT_EQ(fingerprintB, rebound->node.certFingerprint);

    // 吊销清空指纹并写下吊销时间；required 模式下即使报同指纹也不能无令牌恢复
    const auto revoked = runner.run(nodes->revokeCertificate(runner.loop(), rebound->node.id));
    ASSERT_TRUE(revoked.has_value());
    EXPECT_FALSE(revoked->certRevokedAt.empty());
    EdgePulse::Device::AgentUpsert withoutToken = enrollAttempt;
    withoutToken.enrollmentTokenHash.clear();
    const auto denied = runner.run(nodes->upsertAgent(runner.loop(), withoutToken));
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(EdgePulse::Device::AgentUpsertFailure::TokenRejected, denied.error());
    const auto afterDenial = runner.run(nodes->findById(runner.loop(), rebound->node.id));
    ASSERT_TRUE(afterDenial.has_value());
    EXPECT_FALSE(afterDenial->certRevokedAt.empty());
    EXPECT_EQ("offline", afterDenial->status);
}

// 钉住：两条连接同时注册同名节点不抛异常、不产生第二行；冲突只以可重试的并发失败返回，
// 同租户的活节点不能被误判成永久的 NameConflict
// 说明：MySQL 上落败方插入会确定地撞 duplicate key；SQLite 夹具通常以 database is locked 收口，
// 无法稳定制造该路径，本用例覆盖不变量，MySQL 唯一键路径未在本地强制复现。
TEST(Enrollment, ConcurrentRegistrationsShareOneNode)
{
    EdgePulse::TestSupport::AuthTestContext context;
    auto                                    nodesA = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto                                    nodesB = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    EdgePulse::TestSupport::EventLoopRunner runnerA;
    EdgePulse::TestSupport::EventLoopRunner runnerB;

    const std::string              fingerprint(64, 'a');
    EdgePulse::Device::AgentUpsert upsert;
    upsert.name            = "edge-race";
    upsert.agentVersion    = "1.0.0";
    upsert.certFingerprint = fingerprint;
    upsert.labelsJson      = "{}";
    upsert.enrollmentMode  = "off";

    using UpsertResult   = std::expected<EdgePulse::Device::AgentUpsertOutcome, EdgePulse::Device::AgentUpsertFailure>;
    using OptionalResult = std::optional<UpsertResult>;
    OptionalResult     resultA;
    OptionalResult     resultB;
    std::exception_ptr failureA;
    std::exception_ptr failureB;

    // 两条线程都到齐后才发起注册，避免退化成先后执行
    std::barrier barrier{2};
    const auto   worker =
            [&barrier, &upsert](EdgePulse::TestSupport::EventLoopRunner &runner, EdgePulse::Device::NodeRepository &repository, OptionalResult &result, std::exception_ptr &failure)
    {
        barrier.arrive_and_wait();
        try
        {
            result = runner.run(repository.upsertAgent(runner.loop(), upsert));
        } catch (...)
        {
            failure = std::current_exception();
        }
    };
    std::thread threadA(worker, std::ref(runnerA), std::ref(*nodesA), std::ref(resultA), std::ref(failureA));
    std::thread threadB(worker, std::ref(runnerB), std::ref(*nodesB), std::ref(resultB), std::ref(failureB));
    threadA.join();
    threadB.join();

    for (const std::exception_ptr &failure: {failureA, failureB})
    {
        if (failure != nullptr)
        {
            try
            {
                std::rethrow_exception(failure);
            } catch (const std::exception &error)
            {
                FAIL() << "并发注册抛出异常，注册路径不能把冲突升级成 500：" << error.what();
            } catch (...)
            {
                FAIL() << "并发注册抛出未知异常";
            }
        }
    }
    ASSERT_TRUE(resultA.has_value());
    ASSERT_TRUE(resultB.has_value());

    int successCount = 0;
    int createdCount = 0;
    for (const OptionalResult *result: {&resultA, &resultB})
    {
        if (result->has_value())
        {
            ++successCount;
            if (result->value()->created)
            {
                ++createdCount;
            }
            continue;
        }
        // 数据库仍忙时只允许返回可重试的并发失败，不能是令牌拒绝或未捕获异常
        EXPECT_EQ(EdgePulse::Device::AgentUpsertFailure::ConcurrentRegistration, result->value().error());
    }
    EXPECT_GE(successCount, 1);
    if (successCount == 2)
    {
        EXPECT_EQ(1, createdCount);
    }

    // 无论哪条线程先落库，最终只能存在一行
    const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(context.databasePath().string());
    const auto                                                      rows       = connection->execute("SELECT COUNT(*) FROM biz_node WHERE name = 'edge-race' AND deleted = 0");
    ASSERT_NE(nullptr, rows) << connection->lastError();
    ASSERT_TRUE(rows->next());
    EXPECT_EQ(1, std::get<std::int64_t>(rows->getValue(0)));
}
