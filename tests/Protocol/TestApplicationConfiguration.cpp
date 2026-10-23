#include "App/ApplicationConfiguration.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
    /**
     * @brief 用例独占的临时 YAML，析构时删除
     */
    class TemporaryConfigurationFile
    {
    public:
        explicit TemporaryConfigurationFile(const std::string &contents)
        {
            const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            m_path            = std::filesystem::temp_directory_path() / ("edgepulse-config-" + suffix + ".yaml");
            std::ofstream stream(m_path, std::ios::binary | std::ios::trunc);
            stream << contents;
        }

        ~TemporaryConfigurationFile()
        {
            std::error_code error;
            std::filesystem::remove(m_path, error);
        }

        TemporaryConfigurationFile(const TemporaryConfigurationFile &)            = delete;
        TemporaryConfigurationFile &operator=(const TemporaryConfigurationFile &) = delete;

        [[nodiscard]] const std::filesystem::path &path() const noexcept
        {
            return m_path;
        }

    private:
        std::filesystem::path m_path; ///< 临时文件
    };
} // namespace

// 钉住：配置允许 8 位口令与小数速率，不把合法的开发档当成越界
TEST(ApplicationConfiguration, AcceptsEightCharacterPasswordPolicyAndFractionalRateLimit)
{
    const TemporaryConfigurationFile file(R"(edgepulse:
  security:
    password_min_length: 8
server:
  rate_limit:
    requests_per_second: 0.5
)");

    const auto configuration = EdgePulse::App::ApplicationConfiguration::load(file.path());
    ASSERT_TRUE(configuration.has_value()) << configuration.error().message;
    EXPECT_EQ(8, configuration->passwordMinimumLength);
    EXPECT_DOUBLE_EQ(0.5, configuration->requestsPerSecond);
}

// 钉住：低于规范的 8 位下限在装载配置时即被拒绝
TEST(ApplicationConfiguration, RejectsPasswordPolicyBelowEight)
{
    const TemporaryConfigurationFile file(R"(edgepulse:
  security:
    password_min_length: 7
)");

    const auto configuration = EdgePulse::App::ApplicationConfiguration::load(file.path());
    ASSERT_FALSE(configuration.has_value());
    EXPECT_NE(std::string::npos, configuration.error().message.find("8 到 64"));
}

// 钉住：整数写法与小数写法等价，限流配置不因 YAML 整数被静默回落到默认值
TEST(ApplicationConfiguration, AcceptsIntegerRateLimitAndBurst)
{
    const TemporaryConfigurationFile file(R"(server:
  rate_limit:
    requests_per_second: 10
    burst_capacity: 2
)");

    const auto configuration = EdgePulse::App::ApplicationConfiguration::load(file.path());
    ASSERT_TRUE(configuration.has_value()) << configuration.error().message;
    EXPECT_DOUBLE_EQ(10.0, configuration->requestsPerSecond);
    EXPECT_DOUBLE_EQ(2.0, configuration->rateLimitBurstCapacity);
}

// 钉住：非数字与数字后跟垃圾字符都在装载阶段报错，而不是静默关闭限流
TEST(ApplicationConfiguration, RejectsNonNumericRateLimit)
{
    const TemporaryConfigurationFile text(R"(server:
  rate_limit:
    requests_per_second: fast
)");
    const auto textConfiguration = EdgePulse::App::ApplicationConfiguration::load(text.path());
    ASSERT_FALSE(textConfiguration.has_value());
    EXPECT_NE(std::string::npos, textConfiguration.error().message.find("server.rate_limit.requests_per_second"));
    EXPECT_NE(std::string::npos, textConfiguration.error().message.find("必须是数字"));

    const TemporaryConfigurationFile trailing(R"(server:
  rate_limit:
    requests_per_second: 10rps
)");
    const auto trailingConfiguration = EdgePulse::App::ApplicationConfiguration::load(trailing.path());
    ASSERT_FALSE(trailingConfiguration.has_value());
    EXPECT_NE(std::string::npos, trailingConfiguration.error().message.find("10rps"));
}

// 钉住：NaN 与无穷能被浮点解析接受，但必须被有限性检查挡在启动之前
TEST(ApplicationConfiguration, RejectsNonFiniteRateLimit)
{
    const TemporaryConfigurationFile notANumber(R"(server:
  rate_limit:
    requests_per_second: "nan"
)");
    const auto notANumberConfiguration = EdgePulse::App::ApplicationConfiguration::load(notANumber.path());
    ASSERT_FALSE(notANumberConfiguration.has_value());
    EXPECT_NE(std::string::npos, notANumberConfiguration.error().message.find("server.rate_limit.requests_per_second"));

    const TemporaryConfigurationFile infinite(R"(server:
  rate_limit:
    burst_capacity: "inf"
)");
    const auto infiniteConfiguration = EdgePulse::App::ApplicationConfiguration::load(infinite.path());
    ASSERT_FALSE(infiniteConfiguration.has_value());
    EXPECT_NE(std::string::npos, infiniteConfiguration.error().message.find("server.rate_limit.burst_capacity"));
}
