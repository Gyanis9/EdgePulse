#include "Core/Device/AgentAuthorization.h"
#include "Core/Device/NodeRepository.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <Core/Tls/TlsPeerIdentity.h>
#include <Net/Http/Message/HttpRequest.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{
    EdgePulse::Device::AgentUpsert agentUpsert(const std::string &name, const std::string &fingerprint)
    {
        EdgePulse::Device::AgentUpsert upsert;
        upsert.name            = name;
        upsert.agentVersion    = "1.0.0";
        upsert.certFingerprint = fingerprint;
        upsert.labelsJson      = "{}";
        upsert.tenantId        = 1;
        return upsert;
    }
} // namespace

// 钉住：没有客户端证书、证书未验证、指纹不符都拒绝；匹配时返回节点
TEST(AgentAuthorization, RequiresVerifiedMatchingLeafCertificate)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    const std::string fingerprint(64, 'a');
    const auto        node = runner.run(nodes->upsertAgent(runner.loop(), agentUpsert("edge-auth", fingerprint)));

    AsynGyanis::Net::HttpRequest request;

    // 没有身份：必须 403，不能把空身份当成放行
    const auto missing = runner.run(EdgePulse::Device::requireAgentNode(runner.loop(), *nodes, request, node.id));
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(403, missing.error().httpStatus);
    EXPECT_EQ(EdgePulse::Device::AgentIdentityError::Reason::Unauthenticated, missing.error().reason);

    // 身份未验证：同样拒绝
    request.setTlsPeerIdentity(std::make_shared<const AsynGyanis::Core::TlsPeerIdentity>(false, std::vector<std::uint8_t>{}, std::string(64, 'a')));
    const auto unverified = runner.run(EdgePulse::Device::requireAgentNode(runner.loop(), *nodes, request, node.id));
    ASSERT_FALSE(unverified.has_value());
    EXPECT_EQ(403, unverified.error().httpStatus);
    EXPECT_EQ(EdgePulse::Device::AgentIdentityError::Reason::Unauthenticated, unverified.error().reason);

    // 指纹不符：拒绝
    request.setTlsPeerIdentity(std::make_shared<const AsynGyanis::Core::TlsPeerIdentity>(true, std::vector<std::uint8_t>{}, std::string(64, 'b')));
    const auto mismatch = runner.run(EdgePulse::Device::requireAgentNode(runner.loop(), *nodes, request, node.id));
    ASSERT_FALSE(mismatch.has_value());
    EXPECT_EQ(403, mismatch.error().httpStatus);
    EXPECT_EQ(EdgePulse::Device::AgentIdentityError::Reason::Forbidden, mismatch.error().reason);

    // 匹配：返回节点
    request.setTlsPeerIdentity(std::make_shared<const AsynGyanis::Core::TlsPeerIdentity>(true, std::vector<std::uint8_t>{}, fingerprint));
    const auto matched = runner.run(EdgePulse::Device::requireAgentNode(runner.loop(), *nodes, request, node.id));
    ASSERT_TRUE(matched.has_value()) << matched.error().message;
    EXPECT_EQ(node.id, matched->id);
}

// 钉住：节点不存在返回 404，而不是 403
TEST(AgentAuthorization, UnknownNodeIsNotFound)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    AsynGyanis::Net::HttpRequest request;
    request.setTlsPeerIdentity(std::make_shared<const AsynGyanis::Core::TlsPeerIdentity>(true, std::vector<std::uint8_t>{}, std::string(64, 'a')));
    const auto missing = runner.run(EdgePulse::Device::requireAgentNode(runner.loop(), *nodes, request, 999999));
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(404, missing.error().httpStatus);
    EXPECT_EQ(EdgePulse::Device::AgentIdentityError::Reason::NotFound, missing.error().reason);
}

// 钉住：非正整数标识按客户端错误拒绝，不进入节点查询
TEST(AgentAuthorization, NonPositiveNodeIdIsInvalidRequest)
{
    EdgePulse::TestSupport::AuthTestContext context;
    EdgePulse::TestSupport::EventLoopRunner runner;
    auto                                    nodes = std::make_shared<EdgePulse::Device::NodeRepository>(context.database);

    AsynGyanis::Net::HttpRequest request;
    request.setTlsPeerIdentity(std::make_shared<const AsynGyanis::Core::TlsPeerIdentity>(true, std::vector<std::uint8_t>{}, std::string(64, 'a')));
    for (const std::int64_t nodeId: {std::int64_t{0}, std::int64_t{-7}})
    {
        const auto invalid = runner.run(EdgePulse::Device::requireAgentNode(runner.loop(), *nodes, request, nodeId));
        ASSERT_FALSE(invalid.has_value()) << nodeId;
        EXPECT_EQ(400, invalid.error().httpStatus) << nodeId;
        EXPECT_EQ(EdgePulse::Device::AgentIdentityError::Reason::InvalidRequest, invalid.error().reason) << nodeId;
        EXPECT_FALSE(invalid.error().message.empty()) << nodeId;
    }
}
