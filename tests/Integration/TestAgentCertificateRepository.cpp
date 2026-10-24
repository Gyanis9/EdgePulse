#include "Base/Database/Database.h"
#include "Base/Runtime/IdGenerator.h"
#include "Core/Device/AgentCertificateRepository.h"
#include "Core/Device/NodeRepository.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
    /**
     * @brief 直接写入一条旧证书的 active 历史，模拟轮换前的签发记录
     * @param databasePath 用例独占的 SQLite 文件
     * @param tenantId 归属租户
     * @param nodeId 节点标识
     * @param fingerprint 旧证书指纹
     * @param notAfter 旧证书到期时间
     */
    void insertActiveHistory(const std::filesystem::path &databasePath, const std::int64_t tenantId, const std::int64_t nodeId, const std::string &fingerprint,
                             const std::string &notAfter)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(databasePath.string());
        const std::array<AsynGyanis::Database::DatabaseValue, 9>        parameters{EdgePulse::Common::nextIdentifier(), tenantId,    nodeId,
                                                                                   std::string{"serial-previous"},      fingerprint, std::string{"CN=edge-cert"},
                                                                                   std::string{"2026-01-01 00:00:00"},  notAfter,    std::string{"2026-01-01 00:00:00"}};
        const std::unique_ptr<AsynGyanis::Database::DatabaseResult>     result = connection->execute(
                "INSERT INTO biz_agent_certificate (id, tenant_id, node_id, serial, fingerprint, subject, not_before, not_after, status, issued_at, revoked_at) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'active', ?, NULL)",
                parameters);
        ASSERT_NE(nullptr, result) << connection->lastError();
    }

    /**
     * @brief 通过注册路径写入一枚已绑定证书的节点
     * @return 注册后的节点；失败为空
     */
    [[nodiscard]] std::optional<EdgePulse::Device::NodeRecord> registerNode(EdgePulse::TestSupport::EventLoopRunner &runner, EdgePulse::Device::NodeRepository &nodes,
                                                                            const std::string &name, const std::string &fingerprint, const std::string &notAfter,
                                                                            const std::int64_t tenantId)
    {
        EdgePulse::Device::AgentUpsert registration;
        registration.name            = name;
        registration.agentVersion    = "1.0.0";
        registration.certFingerprint = fingerprint;
        registration.labelsJson      = "{}";
        registration.enrollmentMode  = "off";
        registration.tenantId        = tenantId;
        registration.certNotAfter    = notAfter;
        const auto outcome           = runner.run(nodes.upsertAgent(runner.loop(), registration));
        if (!outcome.has_value())
        {
            return std::nullopt;
        }
        return outcome->node;
    }

    /**
     * @brief 构造一次续期轮换的证书字段
     */
    [[nodiscard]] EdgePulse::Device::AgentCertificateRotation makeRotation(const std::string &newFingerprint, const std::string &oldFingerprint)
    {
        EdgePulse::Device::AgentCertificateRotation rotation;
        rotation.fingerprint         = newFingerprint;
        rotation.notAfter            = "2027-10-25 00:00:00";
        rotation.previousFingerprint = oldFingerprint;
        rotation.previousNotAfter    = "2026-10-30 00:00:00";
        rotation.serial              = "serial-current";
        rotation.subject             = "CN=edge-cert";
        rotation.notBefore           = "2026-10-25 00:00:00";
        return rotation;
    }
} // namespace

// 钉住：一次轮换在一个事务里更新 active/previous、把旧 active 历史降级并追加新 active 记录
TEST(AgentCertificateRepository, RotatesActiveAndPreviousInOneTransaction)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                              nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    const std::string oldFingerprint(64, 'a');
    const std::string newFingerprint(64, 'b');
    const std::string oldNotAfter = "2026-11-01 00:00:00";
    const auto        seeded      = registerNode(runner, *nodes, "edge-cert", oldFingerprint, oldNotAfter, 1);
    ASSERT_TRUE(seeded.has_value());
    const std::int64_t nodeId  = seeded->id;
    const std::int64_t version = seeded->version;
    insertActiveHistory(context.databasePath(), 1, nodeId, oldFingerprint, oldNotAfter);

    const EdgePulse::Device::AgentCertificateRotation rotation = makeRotation(newFingerprint, oldFingerprint);
    const auto rotated = runner.run(nodes->rotateCertificate(runner.loop(), EdgePulse::System::TenantScope{1, false}, nodeId, version, rotation));
    ASSERT_TRUE(rotated.has_value()) << rotated.error();
    EXPECT_TRUE(*rotated);

    const std::optional<EdgePulse::Device::NodeRecord> updated = runner.run(nodes->findById(runner.loop(), nodeId));
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(newFingerprint, updated->certFingerprint);
    EXPECT_EQ(rotation.notAfter, updated->certNotAfter);
    EXPECT_EQ(oldFingerprint, updated->previousCertFingerprint);
    EXPECT_EQ(rotation.previousNotAfter, updated->previousCertNotAfter);
    EXPECT_EQ(version + 1, updated->version);

    const auto                                                   certificates = std::make_shared<EdgePulse::Device::AgentCertificateRepository>(context.database);
    const std::vector<EdgePulse::Device::AgentCertificateRecord> history      = runner.run(certificates->listByNode(runner.loop(), nodeId));
    ASSERT_EQ(2U, history.size());
    EXPECT_EQ(1, history[0].tenantId);
    EXPECT_EQ(nodeId, history[0].nodeId);
    EXPECT_EQ(newFingerprint, history[0].fingerprint);
    EXPECT_EQ(rotation.serial, history[0].serial);
    EXPECT_EQ(rotation.subject, history[0].subject);
    EXPECT_EQ(rotation.notBefore, history[0].notBefore);
    EXPECT_EQ(rotation.notAfter, history[0].notAfter);
    EXPECT_EQ("active", history[0].status);
    EXPECT_TRUE(history[0].revokedAt.empty());
    EXPECT_FALSE(history[0].issuedAt.empty());

    EXPECT_EQ(oldFingerprint, history[1].fingerprint);
    EXPECT_EQ("serial-previous", history[1].serial);
    EXPECT_EQ("previous", history[1].status);
    EXPECT_TRUE(history[1].revokedAt.empty());
}

// 钉住：版本不匹配返回可恢复的冲突，节点与历史都不产生任何写入
TEST(AgentCertificateRepository, VersionConflictLeavesNodeAndHistoryUntouched)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                              nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    const std::string oldFingerprint(64, 'a');
    const std::string newFingerprint(64, 'b');
    const std::string conflictingFingerprint(64, 'c');
    const auto        seeded = registerNode(runner, *nodes, "edge-cert-conflict", oldFingerprint, "2026-11-01 00:00:00", 1);
    ASSERT_TRUE(seeded.has_value());
    const std::int64_t nodeId  = seeded->id;
    const std::int64_t version = seeded->version;
    insertActiveHistory(context.databasePath(), 1, nodeId, oldFingerprint, "2026-11-01 00:00:00");

    const EdgePulse::Device::AgentCertificateRotation rotation = makeRotation(newFingerprint, oldFingerprint);
    const auto rotated = runner.run(nodes->rotateCertificate(runner.loop(), EdgePulse::System::TenantScope{1, false}, nodeId, version, rotation));
    ASSERT_TRUE(rotated.has_value()) << rotated.error();
    ASSERT_TRUE(*rotated);

    // 用轮换前的旧版本再提交一次：条件更新必须落空，且不能留下第二枚 active 证据
    const auto conflict =
            runner.run(nodes->rotateCertificate(runner.loop(), EdgePulse::System::TenantScope{1, false}, nodeId, version, makeRotation(conflictingFingerprint, newFingerprint)));
    ASSERT_TRUE(conflict.has_value()) << conflict.error();
    EXPECT_FALSE(*conflict);

    const std::optional<EdgePulse::Device::NodeRecord> unchanged = runner.run(nodes->findById(runner.loop(), nodeId));
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(newFingerprint, unchanged->certFingerprint);
    EXPECT_EQ(oldFingerprint, unchanged->previousCertFingerprint);
    EXPECT_EQ(version + 1, unchanged->version);

    const auto                                                   certificates = std::make_shared<EdgePulse::Device::AgentCertificateRepository>(context.database);
    const std::vector<EdgePulse::Device::AgentCertificateRecord> history      = runner.run(certificates->listByNode(runner.loop(), nodeId));
    ASSERT_EQ(2U, history.size());
    EXPECT_EQ(newFingerprint, history[0].fingerprint);
    EXPECT_EQ("active", history[0].status);
    EXPECT_EQ(oldFingerprint, history[1].fingerprint);
    EXPECT_EQ("previous", history[1].status);
}

// 钉住：previous 只能是被替换的当前证书，调用方不能借轮换把任意指纹塞进重叠窗口
TEST(AgentCertificateRepository, RejectsPreviousFingerprintMismatch)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                              nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    const std::string oldFingerprint(64, 'a');
    const std::string newFingerprint(64, 'b');
    const std::string unrelatedFingerprint(64, 'e');
    const auto        seeded = registerNode(runner, *nodes, "edge-cert-mismatch", oldFingerprint, "2026-11-01 00:00:00", 1);
    ASSERT_TRUE(seeded.has_value());
    const std::int64_t nodeId  = seeded->id;
    const std::int64_t version = seeded->version;

    const auto rejected =
            runner.run(nodes->rotateCertificate(runner.loop(), EdgePulse::System::TenantScope{1, false}, nodeId, version, makeRotation(newFingerprint, unrelatedFingerprint)));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_NE(std::string::npos, rejected.error().find("不一致"));

    const std::optional<EdgePulse::Device::NodeRecord> unchanged = runner.run(nodes->findById(runner.loop(), nodeId));
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(oldFingerprint, unchanged->certFingerprint);
    EXPECT_EQ(version, unchanged->version);
    const auto certificates = std::make_shared<EdgePulse::Device::AgentCertificateRepository>(context.database);
    EXPECT_TRUE(runner.run(certificates->listByNode(runner.loop(), nodeId)).empty());
}

// 钉住：重新入场/改绑不是续期，写入新证书时必须清掉 previous，旧证书不能借重叠窗口继续生效
TEST(AgentCertificateRepository, RebindClearsPreviousCertificate)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                              nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    const std::string oldFingerprint(64, 'a');
    const std::string renewedFingerprint(64, 'b');
    const std::string reboundFingerprint(64, 'd');
    const auto        seeded = registerNode(runner, *nodes, "edge-cert-rebind", oldFingerprint, "2026-11-01 00:00:00", 1);
    ASSERT_TRUE(seeded.has_value());
    const std::int64_t nodeId  = seeded->id;
    const std::int64_t version = seeded->version;

    const auto rotated =
            runner.run(nodes->rotateCertificate(runner.loop(), EdgePulse::System::TenantScope{1, false}, nodeId, version, makeRotation(renewedFingerprint, oldFingerprint)));
    ASSERT_TRUE(rotated.has_value()) << rotated.error();
    ASSERT_TRUE(*rotated);

    const auto rebound = registerNode(runner, *nodes, "edge-cert-rebind", reboundFingerprint, "2027-11-01 00:00:00", 1);
    ASSERT_TRUE(rebound.has_value());
    EXPECT_EQ(reboundFingerprint, rebound->certFingerprint);
    EXPECT_TRUE(rebound->previousCertFingerprint.empty());
    EXPECT_TRUE(rebound->previousCertNotAfter.empty());

    const std::optional<EdgePulse::Device::NodeRecord> stored = runner.run(nodes->findById(runner.loop(), nodeId));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(reboundFingerprint, stored->certFingerprint);
    EXPECT_TRUE(stored->previousCertFingerprint.empty());
    EXPECT_TRUE(stored->previousCertNotAfter.empty());
}

// 钉住：跨租户轮换按不存在处理，节点与历史对租户外视图都不可见
TEST(AgentCertificateRepository, RotationAndHistoryStayInsideTenant)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                              nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    const std::string oldFingerprint(64, 'a');
    const std::string newFingerprint(64, 'b');
    const auto        seeded = registerNode(runner, *nodes, "edge-cert-tenant", oldFingerprint, "2026-11-01 00:00:00", 1);
    ASSERT_TRUE(seeded.has_value());
    const std::int64_t nodeId  = seeded->id;
    const std::int64_t version = seeded->version;
    insertActiveHistory(context.databasePath(), 1, nodeId, oldFingerprint, "2026-11-01 00:00:00");

    const auto denied =
            runner.run(nodes->rotateCertificate(runner.loop(), EdgePulse::System::TenantScope{2, false}, nodeId, version, makeRotation(newFingerprint, oldFingerprint)));
    ASSERT_TRUE(denied.has_value()) << denied.error();
    EXPECT_FALSE(*denied);

    const std::optional<EdgePulse::Device::NodeRecord> unchanged = runner.run(nodes->findById(runner.loop(), nodeId));
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(oldFingerprint, unchanged->certFingerprint);
    EXPECT_EQ(version, unchanged->version);
    EXPECT_TRUE(unchanged->previousCertFingerprint.empty());

    const auto certificates = std::make_shared<EdgePulse::Device::AgentCertificateRepository>(context.database);
    EXPECT_TRUE(runner.run(certificates->scoped(EdgePulse::System::TenantScope{2, false})->listByNode(runner.loop(), nodeId)).empty());
    EXPECT_EQ(1U, runner.run(certificates->listByNode(runner.loop(), nodeId)).size());
    EXPECT_FALSE(runner.run(nodes->scoped(EdgePulse::System::TenantScope{2, false})->findById(runner.loop(), nodeId)).has_value());
}

// 钉住：版本已被并发推进时，即使调用方拿着旧指纹，也应得到可重试冲突而不是指纹不一致
TEST(AgentCertificateRepository, StaleVersionAndFingerprintReturnConflict)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                              nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    const std::string oldFingerprint(64, 'a');
    const std::string newFingerprint(64, 'b');
    const std::string conflictingFingerprint(64, 'c');
    const auto        seeded = registerNode(runner, *nodes, "edge-cert-stale", oldFingerprint, "2026-11-01 00:00:00", 1);
    ASSERT_TRUE(seeded.has_value());
    const std::int64_t nodeId  = seeded->id;
    const std::int64_t version = seeded->version;
    insertActiveHistory(context.databasePath(), 1, nodeId, oldFingerprint, "2026-11-01 00:00:00");

    const auto rotated =
            runner.run(nodes->rotateCertificate(runner.loop(), EdgePulse::System::TenantScope{1, false}, nodeId, version, makeRotation(newFingerprint, oldFingerprint)));
    ASSERT_TRUE(rotated.has_value()) << rotated.error();
    ASSERT_TRUE(*rotated);

    // 旧版本 + 旧指纹：冲突判定必须先于指纹比较，调用方才能重新读取后重试
    const auto conflict =
            runner.run(nodes->rotateCertificate(runner.loop(), EdgePulse::System::TenantScope{1, false}, nodeId, version, makeRotation(conflictingFingerprint, oldFingerprint)));
    ASSERT_TRUE(conflict.has_value()) << conflict.error();
    EXPECT_FALSE(*conflict);

    const std::optional<EdgePulse::Device::NodeRecord> unchanged = runner.run(nodes->findById(runner.loop(), nodeId));
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(newFingerprint, unchanged->certFingerprint);
    EXPECT_EQ(oldFingerprint, unchanged->previousCertFingerprint);
    EXPECT_EQ(version + 1, unchanged->version);

    const auto                                                   certificates = std::make_shared<EdgePulse::Device::AgentCertificateRepository>(context.database);
    const std::vector<EdgePulse::Device::AgentCertificateRecord> history      = runner.run(certificates->listByNode(runner.loop(), nodeId));
    ASSERT_EQ(2U, history.size());
    EXPECT_EQ(newFingerprint, history[0].fingerprint);
    EXPECT_EQ("active", history[0].status);
    EXPECT_EQ(oldFingerprint, history[1].fingerprint);
    EXPECT_EQ("previous", history[1].status);
}

// 钉住：重叠窗口内的同证书重注册不是改绑，previous 必须保留到原定截止时间
TEST(AgentCertificateRepository, ReRegistrationKeepsPreviousOverlap)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                              nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    const std::string oldFingerprint(64, 'a');
    const std::string newFingerprint(64, 'b');
    const auto        seeded = registerNode(runner, *nodes, "edge-cert-reregister", oldFingerprint, "2026-11-01 00:00:00", 1);
    ASSERT_TRUE(seeded.has_value());
    const std::int64_t nodeId  = seeded->id;
    const std::int64_t version = seeded->version;
    insertActiveHistory(context.databasePath(), 1, nodeId, oldFingerprint, "2026-11-01 00:00:00");

    const EdgePulse::Device::AgentCertificateRotation rotation = makeRotation(newFingerprint, oldFingerprint);
    const auto rotated = runner.run(nodes->rotateCertificate(runner.loop(), EdgePulse::System::TenantScope{1, false}, nodeId, version, rotation));
    ASSERT_TRUE(rotated.has_value()) << rotated.error();
    ASSERT_TRUE(*rotated);

    const auto reRegistered = registerNode(runner, *nodes, "edge-cert-reregister", newFingerprint, rotation.notAfter, 1);
    ASSERT_TRUE(reRegistered.has_value());
    EXPECT_EQ(newFingerprint, reRegistered->certFingerprint);
    EXPECT_EQ(oldFingerprint, reRegistered->previousCertFingerprint);
    EXPECT_EQ(rotation.previousNotAfter, reRegistered->previousCertNotAfter);

    const std::optional<EdgePulse::Device::NodeRecord> stored = runner.run(nodes->findById(runner.loop(), nodeId));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(newFingerprint, stored->certFingerprint);
    EXPECT_EQ(oldFingerprint, stored->previousCertFingerprint);
    EXPECT_EQ(rotation.previousNotAfter, stored->previousCertNotAfter);

    const auto                                                   certificates = std::make_shared<EdgePulse::Device::AgentCertificateRepository>(context.database);
    const std::vector<EdgePulse::Device::AgentCertificateRecord> history      = runner.run(certificates->listByNode(runner.loop(), nodeId));
    ASSERT_EQ(2U, history.size());
    EXPECT_EQ(newFingerprint, history[0].fingerprint);
    EXPECT_EQ("active", history[0].status);
    EXPECT_EQ(oldFingerprint, history[1].fingerprint);
    EXPECT_EQ("previous", history[1].status);
}

// 钉住：改绑清掉节点 previous 时，历史里的旧 active 行也必须退出 active，吊销与审计才不会作用在错证书上
TEST(AgentCertificateRepository, RebindClosesStaleActiveHistory)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                              nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    const std::string oldFingerprint(64, 'a');
    const std::string renewedFingerprint(64, 'b');
    const std::string reboundFingerprint(64, 'd');
    const auto        seeded = registerNode(runner, *nodes, "edge-cert-rebind-history", oldFingerprint, "2026-11-01 00:00:00", 1);
    ASSERT_TRUE(seeded.has_value());
    const std::int64_t nodeId  = seeded->id;
    const std::int64_t version = seeded->version;
    insertActiveHistory(context.databasePath(), 1, nodeId, oldFingerprint, "2026-11-01 00:00:00");

    const auto rotated =
            runner.run(nodes->rotateCertificate(runner.loop(), EdgePulse::System::TenantScope{1, false}, nodeId, version, makeRotation(renewedFingerprint, oldFingerprint)));
    ASSERT_TRUE(rotated.has_value()) << rotated.error();
    ASSERT_TRUE(*rotated);

    const auto rebound = registerNode(runner, *nodes, "edge-cert-rebind-history", reboundFingerprint, "2027-11-01 00:00:00", 1);
    ASSERT_TRUE(rebound.has_value());
    EXPECT_EQ(reboundFingerprint, rebound->certFingerprint);
    EXPECT_TRUE(rebound->previousCertFingerprint.empty());

    const auto                                                   certificates = std::make_shared<EdgePulse::Device::AgentCertificateRepository>(context.database);
    const std::vector<EdgePulse::Device::AgentCertificateRecord> history      = runner.run(certificates->listByNode(runner.loop(), nodeId));
    ASSERT_EQ(2U, history.size());
    for (const EdgePulse::Device::AgentCertificateRecord &record: history)
    {
        EXPECT_NE("active", record.status) << record.fingerprint;
    }
    EXPECT_EQ(renewedFingerprint, history[0].fingerprint);
    EXPECT_EQ("previous", history[0].status);
    EXPECT_EQ(oldFingerprint, history[1].fingerprint);
    EXPECT_EQ("previous", history[1].status);
}

// 钉住：未知节点按不存在处理，返回 false 而不是输入错误
TEST(AgentCertificateRepository, UnknownNodeReturnsFalse)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                              nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    const auto unknown =
            runner.run(nodes->rotateCertificate(runner.loop(), EdgePulse::System::TenantScope{1, false}, 987654321, 1, makeRotation(std::string(64, 'b'), std::string(64, 'a'))));
    ASSERT_TRUE(unknown.has_value()) << unknown.error();
    EXPECT_FALSE(*unknown);
}

// 钉住：已吊销节点只能重新入场，续期不能把证书重新挂回去
TEST(AgentCertificateRepository, RevokedNodeRotationReturnsFalse)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                              nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    const std::string oldFingerprint(64, 'a');
    const std::string newFingerprint(64, 'b');
    const auto        seeded = registerNode(runner, *nodes, "edge-cert-revoked", oldFingerprint, "2026-11-01 00:00:00", 1);
    ASSERT_TRUE(seeded.has_value());
    const std::int64_t nodeId = seeded->id;

    const std::optional<EdgePulse::Device::NodeRecord> revoked = runner.run(nodes->revokeCertificate(runner.loop(), nodeId));
    ASSERT_TRUE(revoked.has_value());
    ASSERT_FALSE(revoked->certRevokedAt.empty());

    const auto rejected =
            runner.run(nodes->rotateCertificate(runner.loop(), EdgePulse::System::TenantScope{1, false}, nodeId, revoked->version, makeRotation(newFingerprint, oldFingerprint)));
    ASSERT_TRUE(rejected.has_value()) << rejected.error();
    EXPECT_FALSE(*rejected);

    const std::optional<EdgePulse::Device::NodeRecord> stored = runner.run(nodes->findById(runner.loop(), nodeId));
    ASSERT_TRUE(stored.has_value());
    EXPECT_TRUE(stored->certFingerprint.empty());
    EXPECT_FALSE(stored->certRevokedAt.empty());
    EXPECT_TRUE(stored->previousCertFingerprint.empty());
    const auto certificates = std::make_shared<EdgePulse::Device::AgentCertificateRepository>(context.database);
    EXPECT_TRUE(runner.run(certificates->listByNode(runner.loop(), nodeId)).empty());
}

// 钉住：新指纹、旧指纹、序列号、主题与时间都在写库前校验，且校验失败不推进版本
TEST(AgentCertificateRepository, RejectsInvalidCertificateFields)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                              nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    const std::string oldFingerprint(64, 'a');
    const std::string newFingerprint(64, 'b');
    const auto        seeded = registerNode(runner, *nodes, "edge-cert-invalid", oldFingerprint, "2026-11-01 00:00:00", 1);
    ASSERT_TRUE(seeded.has_value());
    const std::int64_t nodeId  = seeded->id;
    const std::int64_t version = seeded->version;

    const auto expectRejected = [&](const EdgePulse::Device::AgentCertificateRotation &rotation, const std::string &reason)
    {
        const auto result = runner.run(nodes->rotateCertificate(runner.loop(), EdgePulse::System::TenantScope{1, false}, nodeId, version, rotation));
        ASSERT_FALSE(result.has_value()) << reason;
        EXPECT_NE(std::string::npos, result.error().find(reason));
    };

    EdgePulse::Device::AgentCertificateRotation rotation = makeRotation(newFingerprint, oldFingerprint);
    rotation.fingerprint                                 = "not-a-fingerprint";
    expectRejected(rotation, "64 位十六进制");

    rotation                     = makeRotation(newFingerprint, oldFingerprint);
    rotation.previousFingerprint = "not-a-fingerprint";
    expectRejected(rotation, "旧证书指纹");

    rotation        = makeRotation(newFingerprint, oldFingerprint);
    rotation.serial = "";
    expectRejected(rotation, "序列号");

    rotation         = makeRotation(newFingerprint, oldFingerprint);
    rotation.subject = std::string(256, 's');
    expectRejected(rotation, "主题");

    rotation           = makeRotation(newFingerprint, oldFingerprint);
    rotation.notBefore = "";
    rotation.notAfter  = "";
    expectRejected(rotation, "时间");

    rotation           = makeRotation(newFingerprint, oldFingerprint);
    rotation.notBefore = "2027-01-01 00:00:00";
    rotation.notAfter  = "2026-01-01 00:00:00";
    expectRejected(rotation, "晚于");

    rotation                     = makeRotation(oldFingerprint, oldFingerprint);
    rotation.previousFingerprint = oldFingerprint;
    expectRejected(rotation, "相同");

    const std::optional<EdgePulse::Device::NodeRecord> unchanged = runner.run(nodes->findById(runner.loop(), nodeId));
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(oldFingerprint, unchanged->certFingerprint);
    EXPECT_EQ(version, unchanged->version);
    EXPECT_TRUE(unchanged->previousCertFingerprint.empty());
    const auto certificates = std::make_shared<EdgePulse::Device::AgentCertificateRepository>(context.database);
    EXPECT_TRUE(runner.run(certificates->listByNode(runner.loop(), nodeId)).empty());
}

// 钉住：没有旧证书时不允许写重叠截止时间，避免空 previous 也留下一个可被误读的时间点
TEST(AgentCertificateRepository, EmptyPreviousFingerprintDropsOverlapDeadline)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                              nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    const std::string newFingerprint(64, 'b');
    const auto        seeded = registerNode(runner, *nodes, "edge-cert-no-previous", "", "", 1);
    ASSERT_TRUE(seeded.has_value());
    const std::int64_t nodeId  = seeded->id;
    const std::int64_t version = seeded->version;

    EdgePulse::Device::AgentCertificateRotation rotation = makeRotation(newFingerprint, "");
    rotation.previousNotAfter                            = "2026-10-30 00:00:00";
    const auto rotated                                   = runner.run(nodes->rotateCertificate(runner.loop(), EdgePulse::System::TenantScope{1, false}, nodeId, version, rotation));
    ASSERT_TRUE(rotated.has_value()) << rotated.error();
    ASSERT_TRUE(*rotated);

    const std::optional<EdgePulse::Device::NodeRecord> stored = runner.run(nodes->findById(runner.loop(), nodeId));
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(newFingerprint, stored->certFingerprint);
    EXPECT_TRUE(stored->previousCertFingerprint.empty());
    EXPECT_TRUE(stored->previousCertNotAfter.empty());

    const auto                                                   certificates = std::make_shared<EdgePulse::Device::AgentCertificateRepository>(context.database);
    const std::vector<EdgePulse::Device::AgentCertificateRecord> history      = runner.run(certificates->listByNode(runner.loop(), nodeId));
    ASSERT_EQ(1U, history.size());
    EXPECT_EQ(newFingerprint, history[0].fingerprint);
    EXPECT_EQ("active", history[0].status);
}
