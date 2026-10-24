#include "App/ApplicationConfiguration.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

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

// 钉住：h3 传输层空闲超时从 server.quic.idle_timeout_seconds 读出，45 秒原样暴露
TEST(ApplicationConfiguration, AcceptsH3IdleTimeout)
{
    const TemporaryConfigurationFile file(R"(server:
  quic:
    idle_timeout_seconds: 45
)");

    const auto configuration = EdgePulse::App::ApplicationConfiguration::load(file.path());
    ASSERT_TRUE(configuration.has_value()) << configuration.error().message;
    EXPECT_EQ(45, configuration->h3IdleTimeoutSeconds);
}

// 钉住：0 在 QUIC 里表示不启用空闲超时（RFC 9000 §18.2），会失去有限上界；3601 超出允许上限。两者都在启动时被拒绝且报出键名
TEST(ApplicationConfiguration, RejectsOutOfRangeH3IdleTimeout)
{
    for (const std::string value: {"0", "3601"})
    {
        const TemporaryConfigurationFile file("server:\n  quic:\n    idle_timeout_seconds: " + value + "\n");

        const auto configuration = EdgePulse::App::ApplicationConfiguration::load(file.path());
        ASSERT_FALSE(configuration.has_value()) << "应拒绝 idle_timeout_seconds: " << value;
        EXPECT_NE(std::string::npos, configuration.error().message.find("server.quic.idle_timeout_seconds")) << value;
        EXPECT_NE(std::string::npos, configuration.error().message.find("1 到 3600")) << value;
    }
}

// 钉住：引号、小数与带单位后缀的写法都不算整数，装载时直接报错，不静默回落到默认 30 秒
TEST(ApplicationConfiguration, RejectsNonIntegerH3IdleTimeout)
{
    for (const std::string value: {"\"45\"", "45.0", "45s"})
    {
        const TemporaryConfigurationFile file("server:\n  quic:\n    idle_timeout_seconds: " + value + "\n");

        const auto configuration = EdgePulse::App::ApplicationConfiguration::load(file.path());
        ASSERT_FALSE(configuration.has_value()) << "应拒绝 idle_timeout_seconds: " << value;
        EXPECT_NE(std::string::npos, configuration.error().message.find("server.quic.idle_timeout_seconds")) << value;
        EXPECT_NE(std::string::npos, configuration.error().message.find("整数")) << value;
    }
}

// 钉住：缺键时默认 30 秒，1 与 3600 是闭区间边界，都按原值暴露
TEST(ApplicationConfiguration, H3IdleTimeoutDefaultsToThirtyAndAcceptsBounds)
{
    const TemporaryConfigurationFile absent("edgepulse:\n  threads: 1\n");
    const auto defaultConfiguration = EdgePulse::App::ApplicationConfiguration::load(absent.path());
    ASSERT_TRUE(defaultConfiguration.has_value()) << defaultConfiguration.error().message;
    EXPECT_EQ(30, defaultConfiguration->h3IdleTimeoutSeconds);

    for (const auto &[text, expected]: {std::pair{"1", std::int64_t{1}}, std::pair{"3600", std::int64_t{3600}}})
    {
        const TemporaryConfigurationFile file("server:\n  quic:\n    idle_timeout_seconds: " + std::string{text} + "\n");

        const auto configuration = EdgePulse::App::ApplicationConfiguration::load(file.path());
        ASSERT_TRUE(configuration.has_value()) << "应接受 idle_timeout_seconds: " << text << "：" << configuration.error().message;
        EXPECT_EQ(expected, configuration->h3IdleTimeoutSeconds);
    }
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
