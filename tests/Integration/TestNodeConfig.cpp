#include "Base/Database/Database.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Security/PasswordHash.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/NodeConfigRepository.h"
#include "Core/Device/NodeConfigService.h"
#include "Core/Device/NodeRepository.h"
#include "Core/System/TenantRepository.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <barrier>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace
{
    constexpr std::int64_t kDefaultTenant = 1;

    /**
     * @brief 在独立连接上读一条单列整数
     * @param connection 直连
     * @param sql 返回单列 total 的查询
     * @return 查询结果；失败时为 -1
     */
    std::int64_t scalarOf(AsynGyanis::Database::DatabaseConnection &connection, const std::string &sql)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseResult> rows = connection.execute(sql);
        EXPECT_NE(nullptr, rows) << connection.lastError();
        if (!rows)
        {
            return -1;
        }
        EXPECT_TRUE(rows->next());
        return EdgePulse::Sql::integer(*rows, "total");
    }

    /**
     * @brief 申请一个独立租户，返回其标识与管理账号
     */
    std::int64_t provisionTenant(EdgePulse::TestSupport::EventLoopRunner &runner, EdgePulse::TestSupport::AuthTestContext &context, const std::string &code,
                                 const std::string &adminUsername)
    {
        EdgePulse::System::TenantProvisionRequest request;
        request.code              = code;
        request.name              = code + " 租户";
        request.adminUsername     = adminUsername;
        request.adminPasswordHash = EdgePulse::Common::hashPassword("tenant-pass-1");
        const auto provisioned    = runner.run(context.tenants->provision(runner.loop(), std::move(request), 1));
        EXPECT_TRUE(provisioned.has_value()) << (provisioned.has_value() ? "" : provisioned.error());
        return provisioned.has_value() ? provisioned->tenantId : 0;
    }
} // namespace

// 钉住：首次写入版本 1；乐观锁冲突返回 VersionConflict；回滚生成新版本而不是覆盖历史
TEST(NodeConfig, AppliesReportsConflictsAndRollsBack)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;

    auto nodes   = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto repo    = std::make_shared<EdgePulse::Device::NodeConfigRepository>(context.database);
    auto service = std::make_shared<EdgePulse::Device::NodeConfigService>(repo, nodes, nullptr, nullptr, nullptr);

    const auto node  = runner.run(nodes->create(runner.loop(), "config-node", "{}"));
    const auto first = runner.run(service->applyDesired(runner.loop(), {kDefaultTenant, false}, node.id, R"({"interval":30})", 0, "初始配置", 1));
    ASSERT_TRUE(first.has_value()) << first.error().message;
    EXPECT_EQ(1, first->desiredVersion);
    EXPECT_EQ(kDefaultTenant, first->tenantId);
    EXPECT_FALSE(first->desiredHash.empty());
    EXPECT_FALSE(first->inSync);

    const auto report = runner.run(service->report(runner.loop(), {kDefaultTenant, false}, node.id, 1, R"({"interval":30})", "applied", ""));
    ASSERT_TRUE(report.has_value()) << report.error().message;
    EXPECT_TRUE(report->inSync);

    const auto stale = runner.run(service->applyDesired(runner.loop(), {kDefaultTenant, false}, node.id, R"({"interval":60})", 0, "旧版本写入", 1));
    ASSERT_FALSE(stale.has_value());
    EXPECT_EQ(EdgePulse::Device::NodeConfigFailure::VersionConflict, stale.error().reason);

    const auto revisions = runner.run(repo->listRevisions(runner.loop(), node.id, {1, 100}));
    ASSERT_EQ(1U, revisions.rows.size());
    EXPECT_EQ(1, revisions.rows[0].version);
    EXPECT_EQ("初始配置", revisions.rows[0].note);

    const auto rolled = runner.run(service->rollback(runner.loop(), {kDefaultTenant, false}, node.id, revisions.rows[0].id, "回到 30", 1));
    ASSERT_TRUE(rolled.has_value()) << rolled.error().message;
    EXPECT_EQ(2, rolled->desiredVersion);

    // 回滚是新增版本，旧修订保持原样
    const auto history = runner.run(repo->listRevisions(runner.loop(), node.id, {1, 100}));
    ASSERT_EQ(2U, history.rows.size());
    EXPECT_EQ(2, history.rows[0].version);
    EXPECT_EQ(1, history.rows[1].version);
    EXPECT_EQ("回到 30", history.rows[0].note);

    // 不写说明时回滚备注落到被回滚的修订版本
    const auto defaultNote = runner.run(service->rollback(runner.loop(), {kDefaultTenant, false}, node.id, history.rows[1].id, "", 1));
    ASSERT_TRUE(defaultNote.has_value()) << defaultNote.error().message;
    EXPECT_EQ(3, defaultNote->desiredVersion);
    const auto latest = runner.run(repo->listRevisions(runner.loop(), node.id, {1, 100}));
    ASSERT_EQ(3U, latest.rows.size());
    EXPECT_EQ("回滚到版本 1", latest.rows[0].note);
}

// 钉住：非对象、超 64 KiB 的内容与非法状态都在服务层被拒绝，不落库
TEST(NodeConfig, RejectsInvalidContentAndReportStatus)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;

    auto       nodes   = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto       repo    = std::make_shared<EdgePulse::Device::NodeConfigRepository>(context.database);
    auto       service = std::make_shared<EdgePulse::Device::NodeConfigService>(repo, nodes, nullptr, nullptr, nullptr);
    const auto node    = runner.run(nodes->create(runner.loop(), "invalid-config-node", "{}"));

    const auto arrayContent = runner.run(service->applyDesired(runner.loop(), {kDefaultTenant, false}, node.id, "[]", 0, "", 1));
    ASSERT_FALSE(arrayContent.has_value());
    EXPECT_EQ(EdgePulse::Device::NodeConfigFailure::Invalid, arrayContent.error().reason);

    const auto malformed = runner.run(service->applyDesired(runner.loop(), {kDefaultTenant, false}, node.id, "{bad", 0, "", 1));
    ASSERT_FALSE(malformed.has_value());
    EXPECT_EQ(EdgePulse::Device::NodeConfigFailure::Invalid, malformed.error().reason);

    const std::string oversized = R"({"blob":")" + std::string(65U * 1024U, 'x') + R"("})";
    const auto        tooLarge  = runner.run(service->applyDesired(runner.loop(), {kDefaultTenant, false}, node.id, oversized, 0, "", 1));
    ASSERT_FALSE(tooLarge.has_value());
    EXPECT_EQ(EdgePulse::Device::NodeConfigFailure::Invalid, tooLarge.error().reason);

    ASSERT_TRUE(runner.run(service->applyDesired(runner.loop(), {kDefaultTenant, false}, node.id, R"({"interval":30})", 0, "", 1)).has_value());

    const auto badStatus = runner.run(service->report(runner.loop(), {kDefaultTenant, false}, node.id, 1, "", "unknown", ""));
    ASSERT_FALSE(badStatus.has_value());
    EXPECT_EQ(EdgePulse::Device::NodeConfigFailure::Invalid, badStatus.error().reason);

    const auto negativeVersion = runner.run(service->report(runner.loop(), {kDefaultTenant, false}, node.id, -1, "", "failed", ""));
    ASSERT_FALSE(negativeVersion.has_value());
    EXPECT_EQ(EdgePulse::Device::NodeConfigFailure::Invalid, negativeVersion.error().reason);

    const auto longMessage = runner.run(service->report(runner.loop(), {kDefaultTenant, false}, node.id, 1, "", "failed", std::string(256, 'x')));
    ASSERT_FALSE(longMessage.has_value());
    EXPECT_EQ(EdgePulse::Device::NodeConfigFailure::Invalid, longMessage.error().reason);

    const auto missingNode = runner.run(service->applyDesired(runner.loop(), {kDefaultTenant, false}, 999999, R"({"interval":30})", 0, "", 1));
    ASSERT_FALSE(missingNode.has_value());
    EXPECT_EQ(EdgePulse::Device::NodeConfigFailure::NotFound, missingNode.error().reason);

    const auto missingRevision = runner.run(service->rollback(runner.loop(), {kDefaultTenant, false}, node.id, 999999, "", 1));
    ASSERT_FALSE(missingRevision.has_value());
    EXPECT_EQ(EdgePulse::Device::NodeConfigFailure::NotFound, missingRevision.error().reason);
}

// 钉住：上报内容由服务端规范化并重算 hash；空内容只改状态，保留上一次上报正文
TEST(NodeConfig, CanonicalizesContentAndPreservesReportedContentWhenOmitted)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;

    auto       nodes   = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto       repo    = std::make_shared<EdgePulse::Device::NodeConfigRepository>(context.database);
    auto       service = std::make_shared<EdgePulse::Device::NodeConfigService>(repo, nodes, nullptr, nullptr, nullptr);
    const auto node    = runner.run(nodes->create(runner.loop(), "canonical-config-node", "{}"));

    const auto applied = runner.run(service->applyDesired(runner.loop(), {kDefaultTenant, false}, node.id, R"( { "b" : 2, "a" : 1 } )", 0, "", 1));
    ASSERT_TRUE(applied.has_value()) << applied.error().message;
    EXPECT_EQ(R"({"a":1,"b":2})", applied->desiredJson);

    const auto reported = runner.run(service->report(runner.loop(), {kDefaultTenant, false}, node.id, 1, R"({"b":2,"a":1})", "applied", ""));
    ASSERT_TRUE(reported.has_value()) << reported.error().message;
    EXPECT_EQ(applied->desiredHash, reported->reportedHash);
    EXPECT_TRUE(reported->inSync);

    const auto failed = runner.run(service->report(runner.loop(), {kDefaultTenant, false}, node.id, 1, "", "failed", "应用失败"));
    ASSERT_TRUE(failed.has_value()) << failed.error().message;
    EXPECT_FALSE(failed->inSync);
    EXPECT_EQ("failed", failed->reportedStatus);
    EXPECT_EQ("应用失败", failed->reportedMessage);
    EXPECT_EQ(reported->reportedJson, failed->reportedJson);
    EXPECT_EQ(reported->reportedHash, failed->reportedHash);

    const auto recovered = runner.run(service->report(runner.loop(), {kDefaultTenant, false}, node.id, 1, "", "applied", ""));
    ASSERT_TRUE(recovered.has_value());
    EXPECT_TRUE(recovered->inSync);
    // 每次上报都重写说明，空说明不会残留上一次的失败原因
    EXPECT_TRUE(recovered->reportedMessage.empty());

    // 版本或内容任一不同都不算同步
    const auto futureVersion = runner.run(service->report(runner.loop(), {kDefaultTenant, false}, node.id, 2, "", "applied", ""));
    ASSERT_TRUE(futureVersion.has_value());
    EXPECT_FALSE(futureVersion->inSync);

    const auto drifted = runner.run(service->report(runner.loop(), {kDefaultTenant, false}, node.id, 1, R"({"a":9,"b":2})", "applied", ""));
    ASSERT_TRUE(drifted.has_value());
    EXPECT_FALSE(drifted->inSync);
    EXPECT_NE(drifted->desiredHash, drifted->reportedHash);
}

// 钉住：每节点只保留最近一段修订，窗口外的旧版本在同一事务里删除
TEST(NodeConfig, PrunesRevisionsOutsideRetentionWindow)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;

    auto       nodes   = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto       repo    = std::make_shared<EdgePulse::Device::NodeConfigRepository>(context.database);
    auto       service = std::make_shared<EdgePulse::Device::NodeConfigService>(repo, nodes, nullptr, nullptr, nullptr);
    const auto node    = runner.run(nodes->create(runner.loop(), "prune-config-node", "{}"));

    constexpr std::int64_t totalVersions = 105;
    for (std::int64_t version = 0; version < totalVersions; ++version)
    {
        const auto applied =
                runner.run(service->applyDesired(runner.loop(), {kDefaultTenant, false}, node.id, R"({"generation":)" + std::to_string(version + 1) + "}", version, "", 1));
        ASSERT_TRUE(applied.has_value()) << "第 " << (version + 1) << " 次写入失败：" << applied.error().message;
        EXPECT_EQ(version + 1, applied->desiredVersion);
    }

    const auto firstPage = runner.run(repo->listRevisions(runner.loop(), node.id, {1, 100}));
    // 保留窗口严格是最新 100 版：105 版时只剩 6..105
    EXPECT_EQ(100, firstPage.total);
    ASSERT_EQ(100U, firstPage.rows.size());
    EXPECT_EQ(totalVersions, firstPage.rows.front().version);
    EXPECT_EQ(totalVersions - 99, firstPage.rows.back().version);

    const auto secondPage = runner.run(repo->listRevisions(runner.loop(), node.id, {2, 100}));
    EXPECT_EQ(100, secondPage.total);
    EXPECT_TRUE(secondPage.rows.empty());
}

// 钉住：配置写入、查询与修订都受租户边界约束，跨租户写入在仓储与服务两层都被拒绝
TEST(NodeConfig, ScopesConfigAndRevisionsByTenant)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;

    auto nodes   = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto repo    = std::make_shared<EdgePulse::Device::NodeConfigRepository>(context.database);
    auto service = std::make_shared<EdgePulse::Device::NodeConfigService>(repo, nodes, nullptr, nullptr, nullptr);

    const std::int64_t otherTenant = provisionTenant(runner, context, "config_tenant", "config-admin");
    ASSERT_GT(otherTenant, 0);
    const auto otherNode = runner.run(nodes->scoped({otherTenant, false})->create(runner.loop(), "other-tenant-config-node", "{}"));
    ASSERT_EQ(otherTenant, otherNode.tenantId);

    const auto foreignWrite = runner.run(service->applyDesired(runner.loop(), {kDefaultTenant, false}, otherNode.id, R"({"interval":30})", 0, "", 1));
    ASSERT_FALSE(foreignWrite.has_value());
    EXPECT_EQ(EdgePulse::Device::NodeConfigFailure::NotFound, foreignWrite.error().reason);

    // 绕过服务直接写仓储也必须被节点归属拦住
    const auto repositoryWrite = runner.run(repo->scoped({kDefaultTenant, false})->applyDesired(runner.loop(), otherNode.id, R"({"interval":30})", std::string(64, 'a'), 0, "", 1));
    ASSERT_FALSE(repositoryWrite.has_value());
    EXPECT_EQ(EdgePulse::Device::NodeConfigFailure::NotFound, repositoryWrite.error());

    const auto applied = runner.run(service->applyDesired(runner.loop(), {otherTenant, false}, otherNode.id, R"({"interval":30})", 0, "", 1));
    ASSERT_TRUE(applied.has_value()) << applied.error().message;
    EXPECT_EQ(otherTenant, applied->tenantId);

    EXPECT_FALSE(runner.run(repo->scoped({kDefaultTenant, false})->find(runner.loop(), otherNode.id)).has_value());
    EXPECT_EQ(0, runner.run(repo->scoped({kDefaultTenant, false})->desiredVersion(runner.loop(), otherNode.id)));
    EXPECT_EQ(0, runner.run(repo->scoped({kDefaultTenant, false})->listRevisions(runner.loop(), otherNode.id, {1, 100})).total);
    EXPECT_FALSE(runner.run(repo->scoped({kDefaultTenant, false})->report(runner.loop(), otherNode.id, 1, "", "", "applied", "")).has_value());

    // 全租户视图按节点自身租户落库，平台管理员不会把配置写进自己的租户
    const auto crossTenantWrite = runner.run(repo->scoped({0, true})->applyDesired(runner.loop(), otherNode.id, R"({"interval":60})", std::string(64, 'b'), 1, "", 1));
    ASSERT_TRUE(crossTenantWrite.has_value()) << static_cast<int>(crossTenantWrite.error());
    EXPECT_EQ(otherTenant, crossTenantWrite->tenantId);
    EXPECT_EQ(2, crossTenantWrite->desiredVersion);

    const auto otherView = runner.run(repo->scoped({otherTenant, false})->find(runner.loop(), otherNode.id));
    ASSERT_TRUE(otherView.has_value());
    EXPECT_EQ(R"({"interval":60})", otherView->desiredJson);
}

// 钉住：同一节点被两条连接同时以 expectedVersion=0 写入时只有一条推进到版本 1，另一条以冲突收口而不是抛异常
TEST(NodeConfig, ConcurrentAppliesReturnConflictInsteadOfThrowing)
{
    EdgePulse::TestSupport::AuthTestContext context;
    auto                                    nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto                                    repoA = std::make_shared<EdgePulse::Device::NodeConfigRepository>(context.database);
    auto                                    repoB = std::make_shared<EdgePulse::Device::NodeConfigRepository>(context.database);
    EdgePulse::TestSupport::EventLoopRunner runnerA;
    EdgePulse::TestSupport::EventLoopRunner runnerB;

    const auto node = runnerA.run(nodes->create(runnerA.loop(), "config-race-node", "{}"));

    using ApplyResult    = std::expected<EdgePulse::Device::NodeConfigRecord, EdgePulse::Device::NodeConfigFailure>;
    using OptionalResult = std::optional<ApplyResult>;
    OptionalResult     resultA;
    OptionalResult     resultB;
    std::exception_ptr failureA;
    std::exception_ptr failureB;

    // 两条线程都到齐后才发起写入，避免退化成先后执行
    std::barrier barrier{2};
    const auto   worker = [&barrier, nodeId = node.id, hash = std::string(64, 'a')](EdgePulse::TestSupport::EventLoopRunner &runner,
                                                                                    EdgePulse::Device::NodeConfigRepository &repository, OptionalResult &result,
                                                                                    std::exception_ptr &failure)
    {
        barrier.arrive_and_wait();
        try
        {
            result = runner.run(repository.applyDesired(runner.loop(), nodeId, R"({"interval":30})", hash, 0, "并发写入", 1));
        } catch (...)
        {
            failure = std::current_exception();
        }
    };
    std::thread threadA(worker, std::ref(runnerA), std::ref(*repoA), std::ref(resultA), std::ref(failureA));
    std::thread threadB(worker, std::ref(runnerB), std::ref(*repoB), std::ref(resultB), std::ref(failureB));
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
                FAIL() << "并发写入抛出异常，锁冲突必须收口为版本冲突而不是 500：" << error.what();
            } catch (...)
            {
                FAIL() << "并发写入抛出未知异常";
            }
        }
    }
    ASSERT_TRUE(resultA.has_value());
    ASSERT_TRUE(resultB.has_value());

    int successCount  = 0;
    int conflictCount = 0;
    for (const OptionalResult *result: {&resultA, &resultB})
    {
        ASSERT_TRUE(result->has_value());
        if (result->value().has_value())
        {
            ++successCount;
            EXPECT_EQ(1, result->value()->desiredVersion);
            continue;
        }
        ++conflictCount;
        EXPECT_EQ(EdgePulse::Device::NodeConfigFailure::VersionConflict, result->value().error());
    }
    // expectedVersion=0 只可能成功一次；重试后读到的版本已是 1，另一条稳定落到冲突
    EXPECT_EQ(1, successCount);
    EXPECT_EQ(1, conflictCount);

    // 无论哪条线程先落库，配置行与修订都只能各有一条，版本从 1 起
    const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(context.databasePath().string());
    EXPECT_EQ(1, scalarOf(*connection, "SELECT COUNT(*) AS total FROM biz_node_config WHERE node_id = " + std::to_string(node.id)));
    EXPECT_EQ(1, scalarOf(*connection, "SELECT COUNT(*) AS total FROM biz_node_config_revision WHERE node_id = " + std::to_string(node.id)));
    EXPECT_EQ(1, scalarOf(*connection, "SELECT MAX(version) AS total FROM biz_node_config_revision WHERE node_id = " + std::to_string(node.id)));
}

// 钉住：配置已提交后命令创建或通知失败只记日志，接口仍返回已生效的配置
TEST(NodeConfig, KeepsCommittedConfigWhenDispatchFails)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;

    auto nodes    = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    auto repo     = std::make_shared<EdgePulse::Device::NodeConfigRepository>(context.database);
    auto commands = std::make_shared<EdgePulse::Command::CommandRepository>(context.database);
    // 命令创建成功后通知总线抛错：配置此时已经提交，失败不能传染给调用方
    const auto notifier = [](const EdgePulse::Command::CommandRecord &) -> AsynGyanis::Core::Task<>
    {
        throw std::runtime_error("通知总线不可用");
        co_return;
    };
    auto       service = std::make_shared<EdgePulse::Device::NodeConfigService>(repo, nodes, commands, nullptr, notifier);
    const auto node    = runner.run(nodes->create(runner.loop(), "config-dispatch-node", "{}"));

    const auto applied = runner.run(service->applyDesired(runner.loop(), {kDefaultTenant, false}, node.id, R"({"interval":30})", 0, "下发失败", 1));
    ASSERT_TRUE(applied.has_value()) << applied.error().message;
    EXPECT_EQ(1, applied->desiredVersion);
    EXPECT_EQ(R"({"interval":30})", applied->desiredJson);
}
