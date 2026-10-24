#include "AgentApplication.h"

#include "AgentState.h"
#include "CertificateFixture.h"
#include "AgentConfiguration.h"
#include "TestSupport/HttpClientHarness.h"
#include "TestSupport/TemporaryDirectory.h"

#include "Base/Log/LoggerRegistry.h"
#include "Core/Coroutine/Task.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace EdgePulse::Agent
{
    namespace
    {
        /**
         * @brief 用例结束时摘掉根日志器的全部 Sink
         * @details 文件 Sink 在 Windows 上一直持有日志文件，不摘掉会拦住临时目录清理。
         */
        class LoggingSinkReset
        {
        public:
            ~LoggingSinkReset()
            {
                AsynGyanis::Base::LoggerRegistry::instance().getRootLogger().clearSinks();
            }
        };
    } // namespace

    TEST(AgentApplication, RejectsStartupWhenLogFileCannotBeOpened)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      configurationPath = directory.write("agent.yaml", R"(
agent:
  server:
    base_url: https://127.0.0.1:9443
    ca_file: ca.pem
    certificate_file: agent.pem
    private_key_file: agent-key.pem
    name: edge-01
  state:
    directory: state
  logging:
    file: state/agent.log
)");

        // 把日志路径占成目录：文件 Sink 打不开它，启动必须失败而不是降级为仅控制台
        std::error_code error;
        std::filesystem::create_directories(directory.path() / "state" / "agent.log", error);
        ASSERT_FALSE(error) << error.message();

        const auto configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        EXPECT_EQ(1, application.run());
    }

    TEST(AgentApplication, RejectsStartupWhenIdentityCannotBeLoaded)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      configurationPath = directory.write("agent.yaml", R"(
agent:
  server:
    base_url: https://127.0.0.1:9443
    ca_file: missing-ca.pem
    certificate_file: missing-agent.pem
    private_key_file: missing-agent-key.pem
    name: edge-01
  state:
    directory: state
  logging:
    file: state/agent.log
)");

        LoggingSinkReset loggingReset;
        const auto       configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        EXPECT_EQ(1, application.run());
    }

    TEST(AgentApplication, RegistersAndPersistsNodeIdentity)
    {
        TestSupport::TemporaryDirectory    directory;
        const Testing::CertificateMaterial material = Testing::writeSelfSignedCertificate(directory, 30L * 24L * 60L * 60L, "agent");

        TestSupport::ServerHarness server(
                [](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &)
                {
                    router.post("/api/agent/register",
                                [](AsynGyanis::Net::HttpRequest &, AsynGyanis::Net::HttpResponse &response) -> AsynGyanis::Core::Task<>
                                {
                                    response.setStatus(200);
                                    response.setBody(R"({"code":200,"message":"ok","data":{"id":77},"traceId":"trace-agent"})");
                                    co_return;
                                });
                });

        const auto configurationPath = directory.write("agent.yaml", std::string{"agent:\n"
                                                                                 "  server:\n"
                                                                                 "    base_url: http://127.0.0.1:" +
                                                                                 std::to_string(server.port()) +
                                                                                 "\n"
                                                                                 "    allow_plaintext_for_tests: true\n"
                                                                                 "    ca_file: agent.pem\n"
                                                                                 "    certificate_file: agent.pem\n"
                                                                                 "    private_key_file: agent-key.pem\n"
                                                                                 "    name: edge-01\n"
                                                                                 "    request_timeout_seconds: 5\n"
                                                                                 "  state:\n"
                                                                                 "    directory: state\n"
                                                                                 "  logging:\n"
                                                                                 "    file: state/agent.log\n"});

        LoggingSinkReset loggingReset;
        const auto       configuration = AgentConfiguration::load(configurationPath);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();

        AgentApplication application(*configuration);
        EXPECT_EQ(0, application.run());

        auto state = AgentState::open(directory.path() / "state" / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();
        EXPECT_EQ("77", state->get("node_id").value_or(""));
        EXPECT_EQ(material.fingerprint, state->get("cert_fingerprint").value_or(""));
        EXPECT_EQ(material.expiresAt, state->get("cert_expiry").value_or(""));
        EXPECT_FALSE(state->get("agent_version").value_or("").empty());
    }
} // namespace EdgePulse::Agent
