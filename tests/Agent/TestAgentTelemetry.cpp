#include "AgentClient.h"
#include "AgentConfiguration.h"
#include "AgentIdentity.h"
#include "AgentLoop.h"
#include "AgentState.h"
#include "Executors/TelemetryPublisher.h"
#include "TelemetryCollector.h"
#include "TestSupport/HttpClientHarness.h"
#include "TestSupport/TemporaryDirectory.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Database/Database.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Net/Http/Message/HttpRequest.h"
#include "Net/Http/Message/HttpResponse.h"
#include "Net/Http/Routing/Router.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace EdgePulse::Agent
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;
        using AsynGyanis::Core::Task;
        using AsynGyanis::Database::DatabaseValue;

        /**
         * @brief 回环服务端收到的遥测请求
         */
        struct TelemetryCapture
        {
            std::mutex               mutex;         ///< 保护 bodies
            std::vector<std::string> bodies;        ///< 按到达顺序保存的请求正文
            std::vector<int>         statuses{200}; ///< 第 n 次请求返回的状态码，超出后沿用最后一个
            std::atomic<int>         requests{0};   ///< 已收到的请求次数
        };

        /**
         * @brief 测试身份：指纹固定，不依赖真实证书文件
         * @return AgentIdentity 身份
         */
        [[nodiscard]] AgentIdentity testIdentity()
        {
            AgentIdentity identity;
            identity.fingerprint = std::string(64, 'a');
            identity.expiresAt   = "2030-01-01T00:00:00Z";
            return identity;
        }

        /**
         * @brief 组装允许明文的测试配置
         * @param port 回环服务端端口
         * @return AgentConfiguration 配置
         */
        [[nodiscard]] AgentConfiguration plaintextConfiguration(const std::uint16_t port)
        {
            AgentConfiguration configuration;
            configuration.baseUrl                = "http://127.0.0.1:" + std::to_string(port);
            configuration.allowPlaintextForTests = true;
            configuration.name                   = "edge-01";
            configuration.tenantCode             = "tenant-a";
            configuration.requestTimeoutSeconds  = 10;
            return configuration;
        }

        /**
         * @brief 构造一条单位与采集时刻固定的样本
         * @param metric 指标名
         * @param value 指标值
         * @param collectedAt 采集时刻
         * @return TelemetrySample 样本
         */
        [[nodiscard]] TelemetrySample makeSample(std::string metric, const double value, std::string collectedAt = "2026-10-24 12:00:00")
        {
            return TelemetrySample{std::move(metric), value, "%", std::move(collectedAt)};
        }

        /**
         * @brief 注册一条按序返回状态码的遥测路由
         * @param router 测试服务器路由器
         * @param capture 请求捕获点
         */
        void installTelemetryRoute(AsynGyanis::Net::Router &router, const std::shared_ptr<TelemetryCapture> &capture)
        {
            router.post("/api/agent/telemetry",
                        [capture](AsynGyanis::Net::HttpRequest &request, AsynGyanis::Net::HttpResponse &response) -> Task<>
                        {
                            const int sequence = capture->requests.fetch_add(1, std::memory_order_relaxed);
                            {
                                const std::lock_guard lock(capture->mutex);
                                capture->bodies.push_back(std::string{request.body()});
                            }
                            const auto statusIndex = std::min(static_cast<std::size_t>(sequence), capture->statuses.size() - 1);
                            const int  status      = capture->statuses[statusIndex];
                            response.setStatus(status);
                            if (status == 200)
                            {
                                response.setBody(R"({"code":200,"message":"ok","data":{"accepted":1},"traceId":"trace-telemetry"})");
                            } else
                            {
                                response.setBody(R"({"code":)" + std::to_string(status) + R"(,"message":"服务端拒绝","data":null,"traceId":"trace-telemetry"})");
                            }
                            co_return;
                        });
        }

        /**
         * @brief 取出已捕获的请求正文副本
         * @param capture 请求捕获点
         * @return std::vector<std::string> 按到达顺序排列的正文
         */
        [[nodiscard]] std::vector<std::string> capturedBodies(const std::shared_ptr<TelemetryCapture> &capture)
        {
            const std::lock_guard lock(capture->mutex);
            return capture->bodies;
        }

        /**
         * @brief 解析一条遥测请求正文
         * @param body 请求正文
         * @return 成功返回 nodeId 与 samples；正文不合法时为空
         */
        [[nodiscard]] std::optional<ConfigValue> parseTelemetryRequest(const std::string &body)
        {
            auto parsed = AsynGyanis::Base::parseConfigValue(body);
            if (!parsed.has_value() || !parsed->is_object() || !parsed->contains("samples") || !parsed->at("samples").is_array())
            {
                return std::nullopt;
            }
            return parsed;
        }

        /**
         * @brief 判断文本是否为 UTC 的 YYYY-MM-DD HH:MM:SS
         * @param text 待判文本
         * @return true 表示格式符合
         */
        [[nodiscard]] bool isUtcDateTime(const std::string_view text)
        {
            if (text.size() != 19)
            {
                return false;
            }
            for (std::size_t index = 0; index < text.size(); ++index)
            {
                const bool separator = index == 4 || index == 7 || index == 10 || index == 13 || index == 16;
                if (separator)
                {
                    const char expected = (index == 4 || index == 7) ? '-' : (index == 10 ? ' ' : ':');
                    if (text[index] != expected)
                    {
                        return false;
                    }
                    continue;
                }
                if (text[index] < '0' || text[index] > '9')
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 生成与状态库一致的 UTC 文本
         * @return std::string 形如 2026-10-24T12:00:00Z
         */
        [[nodiscard]] std::string utcNow()
        {
            return std::format("{:%Y-%m-%dT%H:%M:%S}Z", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        }
    } // namespace

    TEST(AgentTelemetry, ComputesLinuxCpuPercentFromProcStat)
    {
        const std::string first  = "cpu  100 0 50 800 50 0 0 0 0 0\ncpu0 10 0 5 80 5 0 0 0 0 0\n";
        const std::string second = "cpu  200 0 100 900 50 0 0 0 0 0\n";

        const auto previous = parseLinuxCpuStat(first);
        ASSERT_TRUE(previous.has_value()) << previous.error();
        EXPECT_EQ(150U, previous->busy);
        EXPECT_EQ(1000U, previous->total);

        const auto current = parseLinuxCpuStat(second);
        ASSERT_TRUE(current.has_value()) << current.error();
        EXPECT_EQ(300U, current->busy);
        EXPECT_EQ(1250U, current->total);

        const auto percent = computeCpuPercent(*previous, *current);
        ASSERT_TRUE(percent.has_value()) << percent.error();
        EXPECT_DOUBLE_EQ(60.0, *percent);

        const auto unchanged = computeCpuPercent(*previous, *previous);
        ASSERT_FALSE(unchanged.has_value());
        EXPECT_NE(std::string::npos, unchanged.error().find("0"));

        const auto malformed = parseLinuxCpuStat("intr 1 2 3\n");
        EXPECT_FALSE(malformed.has_value());
        EXPECT_FALSE(malformed.error().empty());
    }

    TEST(AgentTelemetry, ParsesLinuxMemInfoWithAvailableAndFallback)
    {
        const std::string available        = "MemTotal:       1000000 kB\n"
                                             "MemFree:         100000 kB\n"
                                             "MemAvailable:    400000 kB\n"
                                             "Buffers:          50000 kB\n"
                                             "Cached:          250000 kB\n";
        const auto        availablePercent = parseLinuxMemInfo(available);
        ASSERT_TRUE(availablePercent.has_value()) << availablePercent.error();
        EXPECT_DOUBLE_EQ(60.0, *availablePercent);

        const std::string fallback        = "MemTotal:       1000 kB\n"
                                            "MemFree:         100 kB\n"
                                            "Buffers:         100 kB\n"
                                            "Cached:          200 kB\n";
        const auto        fallbackPercent = parseLinuxMemInfo(fallback);
        ASSERT_TRUE(fallbackPercent.has_value()) << fallbackPercent.error();
        EXPECT_DOUBLE_EQ(60.0, *fallbackPercent);

        const auto missingTotal = parseLinuxMemInfo("MemAvailable: 10 kB\n");
        EXPECT_FALSE(missingTotal.has_value());
        EXPECT_FALSE(missingTotal.error().empty());

        const auto missingAvailable = parseLinuxMemInfo("MemTotal: 1000 kB\nMemFree: 100 kB\n");
        EXPECT_FALSE(missingAvailable.has_value());
        EXPECT_FALSE(missingAvailable.error().empty());
    }

    TEST(AgentTelemetry, ComputesDiskPercentAndOmitsZeroTotal)
    {
        const auto quarter = diskUsagePercent(50, 200);
        ASSERT_TRUE(quarter.has_value());
        EXPECT_DOUBLE_EQ(25.0, *quarter);

        EXPECT_FALSE(diskUsagePercent(0, 0).has_value());

        const auto clamped = diskUsagePercent(300, 200);
        ASSERT_TRUE(clamped.has_value());
        EXPECT_DOUBLE_EQ(100.0, *clamped);
    }

    TEST(AgentTelemetry, CollectorReportsHostMetricsWithSharedTimestamp)
    {
        TestSupport::TemporaryDirectory directory;
        TelemetryCollector              collector(directory.path());
        // 两次 CPU 采样之间需要真实的时钟推进，否则总时间差为 0 时按约定跳过 CPU
        std::this_thread::sleep_for(std::chrono::milliseconds{120});

        const std::vector<TelemetrySample>     samples = collector.collect();
        std::map<std::string, TelemetrySample> byMetric;
        for (const TelemetrySample &sample: samples)
        {
            byMetric[sample.metric] = sample;
        }
        for (const std::string &name: {"cpu.usage", "memory.usage", "disk.usage"})
        {
            ASSERT_TRUE(byMetric.contains(name)) << "主机应能采集 " << name << "，实际只返回 " << samples.size() << " 条样本";
            const TelemetrySample &sample = byMetric.at(name);
            EXPECT_EQ("%", sample.unit);
            EXPECT_TRUE(std::isfinite(sample.value));
            EXPECT_GE(sample.value, 0.0);
            EXPECT_LE(sample.value, 100.0);
            EXPECT_TRUE(isUtcDateTime(sample.collectedAt)) << sample.collectedAt;
        }
        for (const TelemetrySample &sample: samples)
        {
            EXPECT_EQ(samples.front().collectedAt, sample.collectedAt) << "同一轮采集的样本必须共享一个 collectedAt";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{120});
        const std::string metricsJson = collector.metricsJson();
        const auto        metrics     = AsynGyanis::Base::parseConfigValue(metricsJson);
        ASSERT_TRUE(metrics.has_value()) << metricsJson;
        ASSERT_TRUE(metrics->is_array());
        ASSERT_GE(metrics->size(), 3U);
        for (const ConfigValue &entry: *metrics)
        {
            ASSERT_TRUE(entry.is_object());
            EXPECT_FALSE(entry.value("metric", std::string{}).empty());
            EXPECT_TRUE(entry.contains("value"));
            EXPECT_TRUE(entry.at("value").is_number());
            EXPECT_EQ("%", entry.value("unit", std::string{}));
            EXPECT_FALSE(entry.contains("collectedAt"));
        }
    }

    TEST(AgentTelemetry, PublisherQueuesOnServerFailureAndFlushesInFifoOrder)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = AgentState::open(directory.path() / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();

        auto capture      = std::make_shared<TelemetryCapture>();
        capture->statuses = {500, 200};
        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &) { installTelemetryRoute(router, capture); });

        AgentLoop          loop;
        AgentClient        client(loop.loop(), plaintextConfiguration(server.port()), testIdentity());
        TelemetryPublisher publisher(client, *state);
        LoopStopGuard      stopGuard{loop};

        const std::vector<TelemetrySample> samples{
                makeSample("cpu.usage", 12.5),
                makeSample("memory.usage", 40.0),
                makeSample("disk.usage", 60.25),
        };
        ASSERT_TRUE(loop.run(publisher.publish(77, samples), std::chrono::seconds{10}, "首次遥测上报"));
        EXPECT_EQ(3U, state->telemetryQueueDepth()) << "5xx 后三条样本都必须进入离线队列";

        const auto firstBodies = capturedBodies(capture);
        ASSERT_EQ(1U, firstBodies.size());
        const auto firstRequest = parseTelemetryRequest(firstBodies.front());
        ASSERT_TRUE(firstRequest.has_value());
        EXPECT_EQ(77, firstRequest->value("nodeId", 0));
        ASSERT_EQ(3U, firstRequest->at("samples").size());

        ASSERT_TRUE(loop.run(publisher.flush(77), std::chrono::seconds{10}, "离线补报"));
        EXPECT_EQ(0U, state->telemetryQueueDepth()) << "HTTP 200 后必须删掉已确认的队列行";

        const auto bodies = capturedBodies(capture);
        ASSERT_EQ(2U, bodies.size());
        const auto flushed = parseTelemetryRequest(bodies.back());
        ASSERT_TRUE(flushed.has_value());
        ASSERT_EQ(3U, flushed->at("samples").size());
        const std::vector<std::string> expected{"cpu.usage", "memory.usage", "disk.usage"};
        for (std::size_t index = 0; index < expected.size(); ++index)
        {
            const auto &sample = flushed->at("samples")[index];
            EXPECT_EQ(expected[index], sample.value("metric", std::string{})) << "补报必须保持入队顺序";
            EXPECT_DOUBLE_EQ(samples[index].value, sample.value("value", -1.0));
            EXPECT_EQ(samples[index].collectedAt, sample.value("collectedAt", std::string{}));
            EXPECT_EQ("%", sample.value("unit", std::string{}));
        }
    }

    TEST(AgentTelemetry, TerminalRejectionDropsBatchWithoutWedgingLaterRows)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = AgentState::open(directory.path() / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();

        auto capture      = std::make_shared<TelemetryCapture>();
        capture->statuses = {400, 400, 200};
        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &) { installTelemetryRoute(router, capture); });

        AgentLoop          loop;
        AgentClient        client(loop.loop(), plaintextConfiguration(server.port()), testIdentity());
        TelemetryPublisher publisher(client, *state);
        LoopStopGuard      stopGuard{loop};

        ASSERT_TRUE(loop.run(publisher.publish(77, {makeSample("cpu.usage", 1.0)}), std::chrono::seconds{10}, "终态拒绝上报"));
        EXPECT_EQ(0U, state->telemetryQueueDepth()) << "终态 4xx 不应把样本放进离线队列";

        state->enqueueTelemetry(serializeTelemetrySample(makeSample("memory.usage", 2.0)).value_or(""));
        state->enqueueTelemetry(serializeTelemetrySample(makeSample("disk.usage", 3.0)).value_or(""));
        ASSERT_EQ(2U, state->telemetryQueueDepth());
        ASSERT_TRUE(loop.run(publisher.flush(77), std::chrono::seconds{10}, "终态拒绝补报"));
        EXPECT_EQ(0U, state->telemetryQueueDepth()) << "补报被终态 4xx 拒绝后必须删除该批行，不能永久堵塞队列";

        state->enqueueTelemetry(serializeTelemetrySample(makeSample("cpu.usage", 4.0)).value_or(""));
        ASSERT_TRUE(loop.run(publisher.flush(77), std::chrono::seconds{10}, "拒绝后再次补报"));
        EXPECT_EQ(0U, state->telemetryQueueDepth());

        const auto bodies = capturedBodies(capture);
        ASSERT_EQ(3U, bodies.size());
        const auto delivered = parseTelemetryRequest(bodies.back());
        ASSERT_TRUE(delivered.has_value());
        ASSERT_EQ(1U, delivered->at("samples").size());
        EXPECT_DOUBLE_EQ(4.0, delivered->at("samples")[0].value("value", -1.0));
    }

    TEST(AgentTelemetry, CorruptQueueRowsAreDroppedAndValidRowIsDelivered)
    {
        TestSupport::TemporaryDirectory directory;
        auto                            state = AgentState::open(directory.path() / "agent.db");
        ASSERT_TRUE(state.has_value()) << state.error();

        auto                       capture = std::make_shared<TelemetryCapture>();
        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &) { installTelemetryRoute(router, capture); });

        AgentLoop          loop;
        AgentClient        client(loop.loop(), plaintextConfiguration(server.port()), testIdentity());
        TelemetryPublisher publisher(client, *state);
        LoopStopGuard      stopGuard{loop};

        state->enqueueTelemetry("not-json");
        state->enqueueTelemetry(serializeTelemetrySample(makeSample("cpu.usage", 5.0)).value_or(""));
        state->enqueueTelemetry(R"({"metric":"","value":1,"unit":"%","collectedAt":"2026-10-24 12:00:00"})");
        ASSERT_EQ(3U, state->telemetryQueueDepth());

        ASSERT_TRUE(loop.run(publisher.flush(77), std::chrono::seconds{10}, "损坏行补报"));
        EXPECT_EQ(0U, state->telemetryQueueDepth()) << "损坏行与已确认行都必须从队列删除";

        const auto bodies = capturedBodies(capture);
        ASSERT_EQ(1U, bodies.size());
        const auto request = parseTelemetryRequest(bodies.front());
        ASSERT_TRUE(request.has_value());
        ASSERT_EQ(1U, request->at("samples").size());
        EXPECT_EQ("cpu.usage", request->at("samples")[0].value("metric", std::string{}));
        EXPECT_DOUBLE_EQ(5.0, request->at("samples")[0].value("value", -1.0));
    }

    TEST(AgentTelemetry, FlushSplitsBatchesAtFiveHundredRows)
    {
        TestSupport::TemporaryDirectory directory;
        const std::filesystem::path     databasePath = directory.path() / "agent.db";
        {
            auto state = AgentState::open(databasePath);
            ASSERT_TRUE(state.has_value()) << state.error();
        }

        const std::string createdAt = utcNow();
        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = Common::Database::openDirectSqlite(databasePath.string());
            ASSERT_NE(nullptr, connection);
            ASSERT_NE(nullptr, connection->execute("BEGIN IMMEDIATE")) << connection->lastError();
            for (int index = 0; index < 501; ++index)
            {
                const auto payload = serializeTelemetrySample(makeSample("cpu.usage", static_cast<double>(index)));
                ASSERT_TRUE(payload.has_value());
                const auto inserted = connection->execute("INSERT INTO telemetry_queue(payload, created_at) VALUES(?, ?)", std::vector<DatabaseValue>{*payload, createdAt});
                ASSERT_NE(nullptr, inserted) << connection->lastError();
            }
            ASSERT_NE(nullptr, connection->execute("COMMIT")) << connection->lastError();
        }

        auto state = AgentState::open(databasePath);
        ASSERT_TRUE(state.has_value()) << state.error();
        ASSERT_EQ(501U, state->telemetryQueueDepth());

        auto capture      = std::make_shared<TelemetryCapture>();
        capture->statuses = {200};
        TestSupport::ServerHarness server([capture](AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &) { installTelemetryRoute(router, capture); });

        AgentLoop          loop;
        AgentClient        client(loop.loop(), plaintextConfiguration(server.port()), testIdentity());
        TelemetryPublisher publisher(client, *state);
        LoopStopGuard      stopGuard{loop};

        ASSERT_TRUE(loop.run(publisher.flush(77), std::chrono::seconds{20}, "分批补报"));
        EXPECT_EQ(0U, state->telemetryQueueDepth());

        const auto bodies = capturedBodies(capture);
        ASSERT_EQ(2U, bodies.size()) << "501 条必须拆成 500 与 1 两批";
        const auto first  = parseTelemetryRequest(bodies[0]);
        const auto second = parseTelemetryRequest(bodies[1]);
        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
        ASSERT_EQ(500U, first->at("samples").size());
        ASSERT_EQ(1U, second->at("samples").size());
        EXPECT_DOUBLE_EQ(0.0, first->at("samples")[0].value("value", -1.0));
        EXPECT_DOUBLE_EQ(500.0, second->at("samples")[0].value("value", -1.0));
    }
} // namespace EdgePulse::Agent
