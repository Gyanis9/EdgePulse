#include "AgentApplication.h"

#include "TestSupport/TemporaryDirectory.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <system_error>

namespace EdgePulse::Agent
{
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
} // namespace EdgePulse::Agent
