#include "Core/Device/AgentRegistry.h"
#include "Core/Device/NodeService.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <Core/Tls/TlsPeerIdentity.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// 钉住：节点创建、分页、乐观锁冲突与软删除
TEST(DeviceService, CreatesListsUpdatesAndRemovesNode)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;

    const auto                     repository = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    EdgePulse::Device::NodeService service(repository);

    const auto created = runner.run(service.create(runner.loop(), "edge-1", R"({"site":"shanghai"})"));
    ASSERT_TRUE(created.has_value()) << created.error().message;
    EXPECT_EQ("edge-1", created->name);
    EXPECT_EQ("offline", created->status);
    EXPECT_EQ(1, created->version);

    const EdgePulse::Device::NodePage page = runner.run(service.list(runner.loop(), EdgePulse::Common::PageParameters{.pageNum = 1, .pageSize = 10}, "", ""));
    ASSERT_EQ(1U, page.rows.size());
    EXPECT_EQ(1, page.total);

    const auto updated = runner.run(service.update(runner.loop(), created->id, std::string{"edge-1b"}, std::nullopt, std::nullopt, 1));
    ASSERT_TRUE(updated.has_value()) << updated.error().message;
    EXPECT_EQ("edge-1b", updated->name);
    EXPECT_EQ(2, updated->version);

    const auto stale = runner.run(service.update(runner.loop(), created->id, std::string{"edge-1c"}, std::nullopt, std::nullopt, 1));
    ASSERT_FALSE(stale.has_value());
    EXPECT_EQ(EdgePulse::Device::NodeServiceError::Reason::Conflict, stale.error().reason);

    EXPECT_TRUE(runner.run(service.remove(runner.loop(), created->id)));
    const EdgePulse::Device::NodePage afterRemoval = runner.run(service.list(runner.loop(), EdgePulse::Common::PageParameters{.pageNum = 1, .pageSize = 10}, "", ""));
    EXPECT_TRUE(afterRemoval.rows.empty());
    EXPECT_EQ(0, afterRemoval.total);
}

// 钉住：白名单外指纹被拒；注册与每次心跳都把结果交给心跳钩子，是否发布由钩子按 becameOnline 决定
TEST(DeviceService, AgentRegistrationChecksFingerprintAndCallsHeartbeatHook)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;

    const auto                                repository = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    const std::string                         fingerprint(64, 'a');
    std::vector<bool>                         onlineFlags;
    EdgePulse::Device::AgentRegistry::Options options;
    options.allowedFingerprints = {fingerprint};
    options.publisher           = [&onlineFlags](const EdgePulse::Device::NodeRecord &, const bool becameOnline) -> AsynGyanis::Core::Task<>
    {
        onlineFlags.push_back(becameOnline);
        co_return;
    };
    EdgePulse::Device::AgentRegistry registry(repository, options);

    const auto rejected = runner.run(registry.registerAgent(runner.loop(), EdgePulse::Device::AgentRegistration{"edge-a", "1.0.0", std::string(64, 'b'), ""}));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(EdgePulse::Device::AgentError::Reason::FingerprintRejected, rejected.error().reason);
    EXPECT_TRUE(onlineFlags.empty());

    const auto registered = runner.run(registry.registerAgent(runner.loop(), EdgePulse::Device::AgentRegistration{"edge-a", "1.0.0", fingerprint, R"({"zone":"edge"})"}));
    ASSERT_TRUE(registered.has_value()) << registered.error().message;
    EXPECT_EQ("online", registered->status);
    ASSERT_EQ(1U, onlineFlags.size());
    EXPECT_TRUE(onlineFlags[0]);

    const auto heartbeated = runner.run(registry.heartbeat(runner.loop(), registered->id, "1.0.1"));
    ASSERT_TRUE(heartbeated.has_value()) << heartbeated.error().message;
    EXPECT_EQ("online", heartbeated->node.status);
    EXPECT_EQ("1.0.1", heartbeated->node.agentVersion);
    EXPECT_FALSE(heartbeated->node.lastSeen.empty());
    EXPECT_FALSE(heartbeated->becameOnline);
    // 非转换心跳同样进钩子：生命周期要靠这次调用清掉扫描插入的离线告警
    ASSERT_EQ(2U, onlineFlags.size());
    EXPECT_FALSE(onlineFlags[1]);

    // 扫描转离线后再心跳是一次新的在线转换
    EXPECT_EQ(1U, runner.run(repository->markStaleOffline(runner.loop(), "9999-12-31 23:59:59", 10)).size());
    const auto recovered = runner.run(registry.heartbeat(runner.loop(), registered->id, "1.0.1"));
    ASSERT_TRUE(recovered.has_value()) << recovered.error().message;
    EXPECT_EQ("online", recovered->node.status);
    EXPECT_TRUE(recovered->becameOnline);
    ASSERT_EQ(3U, onlineFlags.size());
    EXPECT_TRUE(onlineFlags[2]);
}

// 钉住：自报指纹必须与这条 mTLS 连接出示的叶证书一致
TEST(DeviceService, AgentRegistrationBindsConnectionCertificate)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;

    const auto                                 repository = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    EdgePulse::Device::AgentRegistry           registry(repository, {});
    const std::string                          fingerprint(64, 'a');
    const AsynGyanis::Core::TlsPeerIdentityPtr identity = std::make_shared<const AsynGyanis::Core::TlsPeerIdentity>(true, std::vector<std::uint8_t>{}, fingerprint);

    EdgePulse::Device::AgentRegistration mismatched{"edge-cert", "1.0.0", std::string(64, 'b'), ""};
    mismatched.connectionIdentity = identity;
    const auto rejected           = runner.run(registry.registerAgent(runner.loop(), std::move(mismatched)));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(EdgePulse::Device::AgentError::Reason::FingerprintRejected, rejected.error().reason);
    EXPECT_NE(std::string::npos, rejected.error().message.find("不一致"));

    // 自报值大小写不影响比较，落库仍是小写归一值
    EdgePulse::Device::AgentRegistration matched{"edge-cert", "1.0.0", std::string(64, 'A'), ""};
    matched.connectionIdentity = identity;
    const auto registered      = runner.run(registry.registerAgent(runner.loop(), std::move(matched)));
    ASSERT_TRUE(registered.has_value()) << registered.error().message;
    EXPECT_EQ(fingerprint, registered->certFingerprint);
}

// 钉住：节点列表按归属部门与创建人过滤，ALL 不受限
TEST(DeviceService, NodeListHonoursScope)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    const auto                              repository = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);
    EdgePulse::Device::NodeService          service(repository);

    ASSERT_TRUE(runner.run(service.create(runner.loop(), "dept-a", "{}", 200, 101, 7)).has_value());
    ASSERT_TRUE(runner.run(service.create(runner.loop(), "dept-b", "{}", 200, 102, 8)).has_value());

    EdgePulse::Device::NodeListScope byDept;
    byDept.all            = false;
    byDept.allowedDeptIds = {101};
    const auto deptPage   = runner.run(service.list(runner.loop(), EdgePulse::Common::PageParameters{1, 10}, "", "", 0, byDept));
    ASSERT_EQ(1U, deptPage.rows.size());
    EXPECT_EQ("dept-a", deptPage.rows[0].name);

    EdgePulse::Device::NodeListScope withoutDept;
    withoutDept.all      = false;
    const auto emptyPage = runner.run(service.list(runner.loop(), EdgePulse::Common::PageParameters{1, 10}, "", "", 0, withoutDept));
    EXPECT_EQ(0, emptyPage.total);

    EdgePulse::Device::NodeListScope bySelf;
    bySelf.all          = false;
    bySelf.selfOnly     = true;
    bySelf.userId       = 8;
    const auto selfPage = runner.run(service.list(runner.loop(), EdgePulse::Common::PageParameters{1, 10}, "", "", 0, bySelf));
    ASSERT_EQ(1U, selfPage.rows.size());
    EXPECT_EQ("dept-b", selfPage.rows[0].name);

    const auto allPage = runner.run(service.list(runner.loop(), EdgePulse::Common::PageParameters{1, 10}, "", ""));
    EXPECT_EQ(2, allPage.total);
}
