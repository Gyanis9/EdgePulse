#include "AgentConfiguration.h"

#include "TestSupport/TemporaryDirectory.h"

#include <gtest/gtest.h>

#include <string>

namespace EdgePulse::Agent
{
    TEST(AgentConfiguration, LoadsAndValidates)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      path          = directory.write("agent.yaml", R"(
agent:
  server:
    base_url: https://127.0.0.1:9443
    ca_file: ca.pem
    certificate_file: agent.pem
    private_key_file: agent-key.pem
    name: edge-01
    heartbeat_interval_seconds: 30
    command_poll_interval_seconds: 15
  telemetry:
    interval_seconds: 30
  state:
    directory: state
  logging:
    file: state/agent.log
  executors:
    config:
      path: state/config.json
    firmware:
      install_hook: state/install-firmware.sh
      trusted_keys_dir: state/firmware-keys
      staging_directory: state/firmware
  update:
    package_trusted_keys_dir: state/agent-keys
    installer_script: state/install-agent.sh
    staging_directory: state/update
)");
        const auto                      configuration = AgentConfiguration::load(path);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();
        EXPECT_EQ("https://127.0.0.1:9443", configuration->baseUrl);
        EXPECT_EQ("edge-01", configuration->name);
        EXPECT_EQ(30, configuration->heartbeatIntervalSeconds);
    }

    TEST(AgentConfiguration, RejectsMissingIdentityAndPlaintext)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      path          = directory.write("agent.yaml", R"(
agent:
  server:
    base_url: http://127.0.0.1:9443
    name: edge-01
)");
        const auto                      configuration = AgentConfiguration::load(path);
        ASSERT_FALSE(configuration.has_value());
        EXPECT_NE(std::string::npos, configuration.error().find("https"));
    }

    TEST(AgentConfiguration, LoadsCertificateRenewalSettings)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      path          = directory.write("agent.yaml", R"(
agent:
  server:
    base_url: https://127.0.0.1:9443
    ca_file: ca.pem
    certificate_file: agent.pem
    private_key_file: agent-key.pem
    name: edge-01
  certificate:
    renew_before_days: 14
    renewal_check_seconds: 120
  state:
    directory: state
  logging:
    file: state/agent.log
)");
        const auto                      configuration = AgentConfiguration::load(path);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();
        EXPECT_EQ(14, configuration->certificateRenewBeforeDays);
        EXPECT_EQ(120, configuration->certificateRenewalCheckSeconds);
    }

    TEST(AgentConfiguration, CertificateRenewalDefaultsWhenSectionMissing)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      path          = directory.write("agent.yaml", R"(
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
        const auto                      configuration = AgentConfiguration::load(path);
        ASSERT_TRUE(configuration.has_value()) << configuration.error();
        EXPECT_EQ(7, configuration->certificateRenewBeforeDays);
        EXPECT_EQ(3600, configuration->certificateRenewalCheckSeconds);
    }

    TEST(AgentConfiguration, RejectsCertificateRenewalOutOfRange)
    {
        TestSupport::TemporaryDirectory directory;
        const auto                      daysPath = directory.write("days.yaml", R"(
agent:
  server:
    base_url: https://127.0.0.1:9443
    ca_file: ca.pem
    certificate_file: agent.pem
    private_key_file: agent-key.pem
    name: edge-01
  certificate:
    renew_before_days: 0
  state:
    directory: state
  logging:
    file: state/agent.log
)");
        const auto                      days     = AgentConfiguration::load(daysPath);
        ASSERT_FALSE(days.has_value());
        EXPECT_NE(std::string::npos, days.error().find("renew_before_days"));

        const auto intervalPath = directory.write("interval.yaml", R"(
agent:
  server:
    base_url: https://127.0.0.1:9443
    ca_file: ca.pem
    certificate_file: agent.pem
    private_key_file: agent-key.pem
    name: edge-01
  certificate:
    renewal_check_seconds: 30
  state:
    directory: state
  logging:
    file: state/agent.log
)");
        const auto interval     = AgentConfiguration::load(intervalPath);
        ASSERT_FALSE(interval.has_value());
        EXPECT_NE(std::string::npos, interval.error().find("renewal_check_seconds"));
    }
} // namespace EdgePulse::Agent
