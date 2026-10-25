#include "Base/Config/ConfigValue.h"
#include "Base/Database/Database.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Alarm/AlarmRepository.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Firmware/FirmwareRepository.h"
#include "Core/Firmware/FirmwareRolloutPolicy.h"
#include "Core/Firmware/FirmwareRolloutRepository.h"
#include "Core/Firmware/FirmwareRolloutService.h"
#include "Core/Firmware/FirmwareSigningPolicy.h"
#include "Core/System/LeaderGate.h"
#include "Core/System/LeaderTick.h"
#include "Core/Telemetry/TelemetryRepository.h"
#include "TestSupport/AuthTestContext.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using AsynGyanis::Base::ConfigValue;
    using TimePoint = std::chrono::system_clock::time_point;

    /**
     * @brief 固定在一个过去时刻的测试时钟，真实心跳与遥测时间总是晚于它
     */
    [[nodiscard]] TimePoint fakeBase()
    {
        return std::chrono::sys_days{std::chrono::year{2026} / std::chrono::January / 2} + std::chrono::hours{1};
    }

    [[nodiscard]] TimePoint later(const TimePoint base, const int seconds)
    {
        return base + std::chrono::seconds{seconds};
    }

    /**
     * @brief 一套临时数据库上的灰度服务与记录型命令发布口
     */
    class RolloutHarness
    {
    public:
        explicit RolloutHarness(const bool requireSignature = false)
        {
            m_context   = std::make_shared<EdgePulse::TestSupport::AuthTestContext>();
            m_runner    = std::make_shared<EdgePulse::TestSupport::EventLoopRunner>();
            m_firmware  = std::make_shared<EdgePulse::Firmware::FirmwareRepository>(m_context->database);
            m_rollouts  = std::make_shared<EdgePulse::Firmware::FirmwareRolloutRepository>(m_context->database);
            m_commands  = std::make_shared<EdgePulse::Command::CommandRepository>(m_context->database);
            m_nodes     = std::make_shared<EdgePulse::Device::NodeRepository>(m_context->database);
            m_telemetry = std::make_shared<EdgePulse::Telemetry::TelemetryRepository>(m_context->database);
            m_alarms    = std::make_shared<EdgePulse::Alarm::AlarmRepository>(m_context->database);
            m_signing   = std::make_shared<EdgePulse::Firmware::FirmwareSigningPolicy>(EdgePulse::Firmware::FirmwareSigningPolicy{requireSignature, nullptr});

            EdgePulse::Firmware::FirmwareRolloutDependencies dependencies;
            dependencies.firmware                 = m_firmware;
            dependencies.rollouts                 = m_rollouts;
            dependencies.commands                 = m_commands;
            dependencies.nodes                    = m_nodes;
            dependencies.telemetry                = m_telemetry;
            dependencies.alarms                   = m_alarms;
            dependencies.signingPolicy            = m_signing;
            dependencies.heartbeatIntervalSeconds = 1;
            dependencies.publisher                = [this](const EdgePulse::Command::CommandRecord &command) -> AsynGyanis::Core::Task<>
            {
                published.push_back(command);
                co_return;
            };
            m_service = std::make_unique<EdgePulse::Firmware::FirmwareRolloutService>(std::move(dependencies));
        }

        [[nodiscard]] std::int64_t createFirmware(const std::string &version, const std::string &signature = "sig")
        {
            EdgePulse::Firmware::FirmwareRecord record;
            record.name        = "EdgePulse OS " + version;
            record.version     = version;
            record.fileName    = "os-" + version + ".bin";
            record.sizeBytes   = 4096;
            record.sha256      = std::string(64, 'a');
            record.storagePath = "unused-" + version;
            record.signature   = signature;
            return m_runner->run(m_firmware->create(m_runner->loop(), record));
        }

        [[nodiscard]] std::int64_t createNode(const std::string &name, const std::int64_t tenantId = 1)
        {
            const auto nodes = m_nodes->scoped(EdgePulse::System::TenantScope{tenantId, false});
            return m_runner->run(nodes->create(m_runner->loop(), name, "{}")).id;
        }

        void heartbeat(const std::int64_t nodeId, const std::string &version)
        {
            static_cast<void>(m_runner->run(m_nodes->heartbeat(m_runner->loop(), nodeId, "1.0.0", version)));
        }

        [[nodiscard]] std::int64_t createRollout(const std::int64_t firmwareId, const std::string &name, const std::string &policyJson, const std::vector<std::int64_t> &nodeIds)
        {
            const auto result = m_runner->run(m_service->create(m_runner->loop(), EdgePulse::System::TenantScope{1, false}, 7, firmwareId, name, policyJson, nodeIds));
            EXPECT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error().message);
            return result.has_value() ? *result : 0;
        }

        void tick(const TimePoint now)
        {
            m_runner->run(m_service->tick(m_runner->loop(), now));
        }

        [[nodiscard]] EdgePulse::Firmware::FirmwareRolloutAggregate aggregate(const std::int64_t rolloutId)
        {
            const auto result = m_runner->run(m_rollouts->find(m_runner->loop(), rolloutId));
            EXPECT_TRUE(result.has_value());
            return result.has_value() ? *result : EdgePulse::Firmware::FirmwareRolloutAggregate{};
        }

        /**
         * @brief 模拟 Agent 完成一条命令并上报安装结果
         */
        void completeCommand(const std::size_t index, const std::string &version, const TimePoint now, const bool success = true, const bool reportHeartbeat = true)
        {
            EXPECT_LT(index, published.size());
            if (index >= published.size())
            {
                return;
            }
            const EdgePulse::Command::CommandRecord &command = published[index];
            const std::optional<ConfigValue>         payload = AsynGyanis::Base::parseConfigValue(command.payload);
            EXPECT_TRUE(payload.has_value());
            const std::int64_t firmwareId = payload.has_value() ? (*payload)["firmwareId"].get<std::int64_t>() : 0;
            if (success)
            {
                static_cast<void>(m_runner->run(m_commands->finish(m_runner->loop(), command.id, "succeeded", "{}", "ok")));
                static_cast<void>(m_runner->run(m_firmware->updateTask(m_runner->loop(), firmwareId, command.nodeId, "installed", 100, "ok", command.id)));
                if (reportHeartbeat)
                {
                    heartbeat(command.nodeId, version);
                }
                insertTelemetry(command.nodeId, now);
            } else
            {
                static_cast<void>(m_runner->run(m_commands->finish(m_runner->loop(), command.id, "failed", "{}", "安装失败")));
                static_cast<void>(m_runner->run(m_firmware->updateTask(m_runner->loop(), firmwareId, command.nodeId, "failed", 0, "安装失败", command.id)));
            }
        }

        void insertTelemetry(const std::int64_t nodeId, const TimePoint now)
        {
            EdgePulse::Telemetry::TelemetrySample sample;
            sample.metric      = "cpu.usage";
            sample.value       = 12.0;
            sample.unit        = "%";
            sample.collectedAt = EdgePulse::Common::formatDateTimeUtc(now);
            static_cast<void>(m_runner->run(m_telemetry->insertBatch(m_runner->loop(), nodeId, {sample})));
        }

        void raiseAlarm(const std::int64_t nodeId, const std::string &metric, const std::int64_t ruleId)
        {
            EdgePulse::Alarm::AlarmRuleRecord rule;
            rule.id          = ruleId;
            rule.name        = metric;
            rule.metric      = metric;
            rule.comparison  = ">";
            rule.level       = "warning";
            const auto alarm = m_runner->run(m_alarms->insertAlarm(m_runner->loop(), rule, nodeId, 99.0, "记录型测试告警"));
            EXPECT_TRUE(alarm.has_value());
        }

        void completeAll(const std::size_t from, const std::string &version, const TimePoint now)
        {
            for (std::size_t index = from; index < published.size(); ++index)
            {
                completeCommand(index, version, now);
            }
        }

        std::shared_ptr<EdgePulse::TestSupport::AuthTestContext>        m_context;
        std::shared_ptr<EdgePulse::TestSupport::EventLoopRunner>        m_runner;
        std::shared_ptr<EdgePulse::Firmware::FirmwareRepository>        m_firmware;
        std::shared_ptr<EdgePulse::Firmware::FirmwareRolloutRepository> m_rollouts;
        std::shared_ptr<EdgePulse::Command::CommandRepository>          m_commands;
        std::shared_ptr<EdgePulse::Device::NodeRepository>              m_nodes;
        std::shared_ptr<EdgePulse::Telemetry::TelemetryRepository>      m_telemetry;
        std::shared_ptr<EdgePulse::Alarm::AlarmRepository>              m_alarms;
        std::shared_ptr<EdgePulse::Firmware::FirmwareSigningPolicy>     m_signing;
        std::unique_ptr<EdgePulse::Firmware::FirmwareRolloutService>    m_service;
        std::vector<EdgePulse::Command::CommandRecord>                  published;
    };

    [[nodiscard]] std::int64_t firmwareIdOf(const EdgePulse::Command::CommandRecord &command)
    {
        const std::optional<ConfigValue> payload = AsynGyanis::Base::parseConfigValue(command.payload);
        return payload.has_value() ? (*payload)["firmwareId"].get<std::int64_t>() : 0;
    }

    [[nodiscard]] const EdgePulse::Firmware::FirmwareRolloutTargetRecord *targetOf(const EdgePulse::Firmware::FirmwareRolloutAggregate &aggregate, const std::int64_t nodeId)
    {
        for (const EdgePulse::Firmware::FirmwareRolloutTargetRecord &target: aggregate.targets)
        {
            if (target.nodeId == nodeId)
            {
                return &target;
            }
        }
        return nullptr;
    }

    constexpr const char *kThreeWavePolicy = R"({"waves":[{"size":2},{"size":2},{"size":2}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30})";
    constexpr const char *kOneWavePolicy   = R"({"waves":[{"size":2}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30})";
} // namespace

// 钉住：三个波次依次下发、门控通过后自动推进，最后一波完成；重复 tick 不会重复派发
TEST(FirmwareRolloutService, AdvancesThreeWavesToCompletion)
{
    RolloutHarness            harness;
    const TimePoint           base = fakeBase();
    std::vector<std::int64_t> nodes;
    for (int index = 0; index < 6; ++index)
    {
        const std::int64_t nodeId = harness.createNode("rollout-wave-node-" + std::to_string(index));
        harness.heartbeat(nodeId, "0.9.0");
        nodes.push_back(nodeId);
    }
    const std::int64_t firmwareId = harness.createFirmware("2.0.0");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "三波灰度", kThreeWavePolicy, nodes);
    ASSERT_GT(rolloutId, 0);
    auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("scheduled", aggregate.rollout.status);
    EXPECT_EQ(3, aggregate.rollout.totalWaves);
    ASSERT_EQ(3U, aggregate.waves.size());
    EXPECT_EQ(2, aggregate.waves[0].targetCount);
    for (const EdgePulse::Firmware::FirmwareRolloutTargetRecord &target: aggregate.targets)
    {
        EXPECT_EQ(0, target.previousFirmwareId);
    }

    harness.tick(base);
    EXPECT_EQ(2U, harness.published.size());
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("running", aggregate.rollout.status);
    EXPECT_EQ(0, aggregate.rollout.currentWave);
    EXPECT_EQ("observing", aggregate.waves[0].status);

    // 同一时刻再次 tick 不得重复下发
    harness.tick(base);
    EXPECT_EQ(2U, harness.published.size());

    const std::size_t firstWave = harness.published.size();
    harness.completeAll(0, "2.0.0", base);
    harness.tick(later(base, 2));
    EXPECT_EQ(firstWave, harness.published.size());
    harness.tick(later(base, 4));
    EXPECT_EQ(4U, harness.published.size());
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ(1, aggregate.rollout.currentWave);
    EXPECT_EQ("passed", aggregate.waves[0].status);

    harness.completeAll(firstWave, "2.0.0", later(base, 4));
    harness.tick(later(base, 6));
    harness.tick(later(base, 8));
    EXPECT_EQ(6U, harness.published.size());
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ(2, aggregate.rollout.currentWave);
    EXPECT_EQ("passed", aggregate.waves[1].status);

    harness.completeAll(4, "2.0.0", later(base, 8));
    harness.tick(later(base, 10));
    harness.tick(later(base, 12));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("completed", aggregate.rollout.status);
    EXPECT_FALSE(aggregate.rollout.finishedAt.empty());
    EXPECT_EQ("passed", aggregate.waves[2].status);
    for (const EdgePulse::Firmware::FirmwareRolloutTargetRecord &target: aggregate.targets)
    {
        EXPECT_EQ("healthy", target.status);
    }
    harness.tick(later(base, 20));
    EXPECT_EQ(6U, harness.published.size());
}

// 钉住：门控失败默认暂停，不会自动回滚；失败原因写入发布与目标快照
TEST(FirmwareRolloutService, GateFailurePausesWithoutRollback)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t first  = harness.createNode("rollout-fail-a");
    const std::int64_t second = harness.createNode("rollout-fail-b");
    harness.heartbeat(first, "0.9.0");
    harness.heartbeat(second, "0.9.0");
    const std::int64_t firmwareId = harness.createFirmware("2.0.1");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "门控失败", kOneWavePolicy, {first, second});
    harness.tick(base);
    ASSERT_EQ(2U, harness.published.size());
    harness.completeCommand(0, "2.0.1", base);
    harness.completeCommand(1, "2.0.1", base, false);

    harness.tick(later(base, 2));
    const auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("paused", aggregate.rollout.status);
    EXPECT_NE(std::string::npos, aggregate.rollout.pauseReason.find("健康门控"));
    EXPECT_EQ("failed", aggregate.waves[0].status);
    ASSERT_NE(nullptr, targetOf(aggregate, first));
    ASSERT_NE(nullptr, targetOf(aggregate, second));
    EXPECT_EQ("healthy", targetOf(aggregate, first)->status);
    EXPECT_EQ("failed", targetOf(aggregate, second)->status);
    EXPECT_EQ(2U, harness.published.size());

    harness.tick(later(base, 20));
    EXPECT_EQ("paused", harness.aggregate(rolloutId).rollout.status);
    EXPECT_EQ(2U, harness.published.size());
}

// 钉住：autoRollback 开启时失败波次转 rolling_back，下一次 tick 向上一版本重新下发并落 rolled_back
TEST(FirmwareRolloutService, AutoRollbackDispatchesPreviousVersion)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t first  = harness.createNode("rollout-auto-a");
    const std::int64_t second = harness.createNode("rollout-auto-b");
    harness.heartbeat(first, "1.0.0");
    harness.heartbeat(second, "1.0.0");
    const std::int64_t previous  = harness.createFirmware("1.0.0");
    const std::int64_t target    = harness.createFirmware("2.0.2");
    const char        *policy    = R"({"waves":[{"size":2}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30,"autoRollback":true})";
    const std::int64_t rolloutId = harness.createRollout(target, "自动回滚", policy, {first, second});
    auto               aggregate = harness.aggregate(rolloutId);
    for (const auto &record: aggregate.targets)
    {
        EXPECT_EQ(previous, record.previousFirmwareId);
    }
    harness.tick(base);
    ASSERT_EQ(2U, harness.published.size());
    harness.completeCommand(0, "2.0.2", base, false);
    harness.completeCommand(1, "2.0.2", base, false);
    harness.tick(later(base, 2));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("rolling_back", aggregate.rollout.status);

    harness.tick(later(base, 3));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("rolled_back", aggregate.rollout.status);
    ASSERT_EQ(4U, harness.published.size());
    EXPECT_EQ(previous, firmwareIdOf(harness.published[2]));
    EXPECT_EQ(previous, firmwareIdOf(harness.published[3]));
    for (const EdgePulse::Firmware::FirmwareRolloutTargetRecord &record: aggregate.targets)
    {
        EXPECT_EQ("rolled_back", record.status);
        EXPECT_GT(record.commandId, 0);
    }
}

// 钉住：历史版本未签名时自动回滚无法执行，发布落在 rollback_unavailable
TEST(FirmwareRolloutService, AutoRollbackMarksUnavailableWhenPreviousUnsigned)
{
    RolloutHarness     harness(true);
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-unsigned");
    harness.heartbeat(nodeId, "1.0.0");
    const std::int64_t previous  = harness.createFirmware("1.0.0", "");
    const std::int64_t target    = harness.createFirmware("2.0.3", "signature");
    const char        *policy    = R"({"waves":[{"size":1}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30,"autoRollback":true})";
    const std::int64_t rolloutId = harness.createRollout(target, "回滚不可用", policy, {nodeId});
    ASSERT_EQ(previous, harness.aggregate(rolloutId).targets[0].previousFirmwareId);
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());
    harness.completeCommand(0, "2.0.3", base, false);
    harness.tick(later(base, 2));
    harness.tick(later(base, 3));
    const auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("rollback_unavailable", aggregate.rollout.status);
    EXPECT_NE(std::string::npos, aggregate.rollout.lastError.find("未签名"));
    EXPECT_EQ(1U, harness.published.size());
}

// 钉住：历史版本缺失时自动回滚无法执行，发布落在 rollback_unavailable
TEST(FirmwareRolloutService, AutoRollbackMarksUnavailableWithoutPrevious)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-no-previous");
    harness.heartbeat(nodeId, "0.9.0");
    const std::int64_t firmwareId = harness.createFirmware("2.4.9");
    const char        *policy     = R"({"waves":[{"size":1}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30,"autoRollback":true})";
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "缺少历史版本", policy, {nodeId});
    EXPECT_EQ(0, harness.aggregate(rolloutId).targets[0].previousFirmwareId);
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());
    harness.completeCommand(0, "2.4.9", base, false);
    harness.tick(later(base, 2));
    EXPECT_EQ("rolling_back", harness.aggregate(rolloutId).rollout.status);

    harness.tick(later(base, 3));
    const auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("rollback_unavailable", aggregate.rollout.status);
    EXPECT_NE(std::string::npos, aggregate.rollout.lastError.find("缺少"));
    EXPECT_EQ(1U, harness.published.size());
}

// 钉住：维护窗口约束每一次新波次派发，窗口外停在波次边界等待
TEST(FirmwareRolloutService, MaintenanceWindowGatesEveryWave)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t first  = harness.createNode("rollout-window-a");
    const std::int64_t second = harness.createNode("rollout-window-b");
    harness.heartbeat(first, "0.9.0");
    harness.heartbeat(second, "0.9.0");
    const std::int64_t firmwareId = harness.createFirmware("2.4.8");
    const char *policy = R"({"waves":[{"size":1},{"size":1}],"settleSeconds":2,"observeSeconds":61,"maxWaveSeconds":120,"maintenanceWindow":{"start":"01:00","end":"01:01"}})";
    const std::int64_t rolloutId = harness.createRollout(firmwareId, "维护窗口", policy, {first, second});
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());

    harness.completeCommand(0, "2.4.8", base);
    harness.tick(later(base, 61));
    auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("running", aggregate.rollout.status);
    EXPECT_EQ(1, aggregate.rollout.currentWave);
    EXPECT_EQ("passed", aggregate.waves[0].status);
    EXPECT_TRUE(aggregate.waves[1].dispatchedAt.empty());
    EXPECT_EQ(1U, harness.published.size());

    harness.tick(later(base, 86400));
    aggregate = harness.aggregate(rolloutId);
    ASSERT_EQ(2U, harness.published.size());
    EXPECT_EQ(EdgePulse::Common::formatDateTimeUtc(later(base, 86400)), aggregate.waves[1].dispatchedAt);
}

// 钉住：超过 maxWaveSeconds 仍未终结时，onTimeout 决定暂停还是失败
TEST(FirmwareRolloutService, TimeoutPausesOrFailsByPolicy)
{
    const char *pausePolicy = R"({"waves":[{"size":1}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":10,"onTimeout":"pause"})";
    const char *failPolicy  = R"({"waves":[{"size":1}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":10,"onTimeout":"fail"})";

    RolloutHarness     pauseHarness;
    const TimePoint    base      = fakeBase();
    const std::int64_t pauseNode = pauseHarness.createNode("rollout-timeout-pause");
    pauseHarness.heartbeat(pauseNode, "0.9.0");
    const std::int64_t pauseFirmware = pauseHarness.createFirmware("2.0.4");
    const std::int64_t pauseRollout  = pauseHarness.createRollout(pauseFirmware, "超时暂停", pausePolicy, {pauseNode});
    pauseHarness.tick(base);
    ASSERT_EQ(1U, pauseHarness.published.size());
    // 命令迟迟不结束，但心跳与遥测正常：观察窗口结束后继续等到 maxWave 才按超时处理
    pauseHarness.insertTelemetry(pauseNode, later(base, 2));
    pauseHarness.tick(later(base, 5));
    EXPECT_EQ("running", pauseHarness.aggregate(pauseRollout).rollout.status);
    pauseHarness.tick(later(base, 11));
    auto aggregate = pauseHarness.aggregate(pauseRollout);
    EXPECT_EQ("paused", aggregate.rollout.status);
    EXPECT_EQ("failed", aggregate.waves[0].status);
    EXPECT_NE(std::string::npos, aggregate.rollout.pauseReason.find("超时"));

    RolloutHarness     failHarness;
    const std::int64_t failNode = failHarness.createNode("rollout-timeout-fail");
    failHarness.heartbeat(failNode, "0.9.0");
    const std::int64_t failFirmware = failHarness.createFirmware("2.0.5");
    const std::int64_t failRollout  = failHarness.createRollout(failFirmware, "超时失败", failPolicy, {failNode});
    failHarness.tick(base);
    failHarness.insertTelemetry(failNode, later(base, 2));
    failHarness.tick(later(base, 11));
    aggregate = failHarness.aggregate(failRollout);
    EXPECT_EQ("failed", aggregate.rollout.status);
    EXPECT_FALSE(aggregate.rollout.finishedAt.empty());
}

// 钉住：节点离线与缺少新鲜遥测都判失败
TEST(FirmwareRolloutService, OfflineNodeAndMissingTelemetryFailGate)
{
    RolloutHarness     harness;
    const TimePoint    base    = fakeBase();
    const std::int64_t online  = harness.createNode("rollout-online");
    const std::int64_t offline = harness.createNode("rollout-offline");
    harness.heartbeat(online, "0.9.0");
    const std::int64_t firmwareId = harness.createFirmware("2.0.6");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "离线目标", kOneWavePolicy, {online, offline});
    harness.tick(base);
    ASSERT_EQ(2U, harness.published.size());
    harness.completeCommand(0, "2.0.6", base);
    // 第二台保持离线：命令与任务完成但没有心跳，节点仍判离线
    harness.completeCommand(1, "2.0.6", base, true, false);
    harness.tick(later(base, 2));
    auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("paused", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, offline));
    EXPECT_EQ("failed", targetOf(aggregate, offline)->status);
    EXPECT_NE(std::string::npos, targetOf(aggregate, offline)->healthJson.find("节点离线"));

    RolloutHarness     noTelemetry;
    const std::int64_t first  = noTelemetry.createNode("rollout-telemetry-a");
    const std::int64_t second = noTelemetry.createNode("rollout-telemetry-b");
    noTelemetry.heartbeat(first, "0.9.0");
    noTelemetry.heartbeat(second, "0.9.0");
    const std::int64_t firmware = noTelemetry.createFirmware("2.0.7");
    const std::int64_t rollout  = noTelemetry.createRollout(firmware, "缺少遥测", kOneWavePolicy, {first, second});
    noTelemetry.tick(base);
    noTelemetry.completeCommand(0, "2.0.7", base);
    noTelemetry.completeCommand(1, "2.0.7", base);
    // completeCommand 会为两条命令各写一条遥测；这里删掉第二台的遥测来构造缺口
    const auto deleteTelemetry = [&noTelemetry](const std::int64_t nodeId)
    {
        const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(noTelemetry.m_context->databasePath().string());
        ASSERT_NE(nullptr, connection);
        ASSERT_NE(nullptr, connection->execute("DELETE FROM biz_telemetry WHERE node_id = " + std::to_string(nodeId))) << connection->lastError();
    };
    deleteTelemetry(second);
    noTelemetry.tick(later(base, 2));
    aggregate = noTelemetry.aggregate(rollout);
    EXPECT_EQ("paused", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, second));
    EXPECT_EQ("failed", targetOf(aggregate, second)->status);
    EXPECT_NE(std::string::npos, targetOf(aggregate, second)->healthJson.find("遥测"));
}

// 钉住：观察窗口内新增的非豁免告警判失败，豁免指标不影响门控
TEST(FirmwareRolloutService, GateUsesNewAlarmsAndExemptions)
{
    RolloutHarness     harness;
    const TimePoint    base    = fakeBase();
    const std::int64_t alarmed = harness.createNode("rollout-alarm");
    const std::int64_t exempt  = harness.createNode("rollout-alarm-exempt");
    harness.heartbeat(alarmed, "0.9.0");
    harness.heartbeat(exempt, "0.9.0");
    const std::int64_t firmwareId = harness.createFirmware("2.4.7");
    const char        *policy     = R"({"waves":[{"size":2}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30,
        "minWaveSuccessRatio":0.5,"minCumulativeSuccessRatio":0.5,"exemptAlarmMetrics":["cpu.ignore"]})";
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "告警门控", policy, {alarmed, exempt});
    harness.tick(base);
    ASSERT_EQ(2U, harness.published.size());
    harness.completeCommand(0, "2.4.7", base);
    harness.completeCommand(1, "2.4.7", base);
    harness.raiseAlarm(alarmed, "cpu.high", 101);
    harness.raiseAlarm(exempt, "cpu.ignore", 102);

    harness.tick(later(base, 2));
    EXPECT_EQ("running", harness.aggregate(rolloutId).rollout.status);
    harness.tick(later(base, 4));
    const auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("completed", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, alarmed));
    ASSERT_NE(nullptr, targetOf(aggregate, exempt));
    EXPECT_EQ("failed", targetOf(aggregate, alarmed)->status);
    EXPECT_NE(std::string::npos, targetOf(aggregate, alarmed)->healthJson.find("告警"));
    EXPECT_EQ("healthy", targetOf(aggregate, exempt)->status);
}

// 钉住：maxConcurrentPerWave 限制每拍派发数量，全部绑定后才进入观察
TEST(FirmwareRolloutService, HonorsMaxConcurrentPerWave)
{
    RolloutHarness            harness;
    const TimePoint           base = fakeBase();
    std::vector<std::int64_t> nodes;
    for (int index = 0; index < 4; ++index)
    {
        const std::int64_t nodeId = harness.createNode("rollout-cap-" + std::to_string(index));
        harness.heartbeat(nodeId, "0.9.0");
        nodes.push_back(nodeId);
    }
    const std::int64_t firmwareId = harness.createFirmware("2.0.8");
    const char        *policy     = R"({"waves":[{"size":4}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30,"maxConcurrentPerWave":2})";
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "并发上限", policy, nodes);
    harness.tick(base);
    EXPECT_EQ(2U, harness.published.size());
    EXPECT_EQ("running", harness.aggregate(rolloutId).rollout.status);
    harness.completeCommand(0, "2.0.8", base);
    harness.completeCommand(1, "2.0.8", base);
    harness.tick(later(base, 1));
    EXPECT_EQ(4U, harness.published.size());
    harness.completeAll(2, "2.0.8", later(base, 1));
    harness.tick(later(base, 3));
    harness.tick(later(base, 5));
    EXPECT_EQ("completed", harness.aggregate(rolloutId).rollout.status);
}

// 钉住：人工暂停、恢复、提升与中止都能驱动状态机，恢复会重开观察窗口
TEST(FirmwareRolloutService, ManualPauseResumePromoteAbort)
{
    RolloutHarness            harness;
    const TimePoint           base = fakeBase();
    std::vector<std::int64_t> nodes;
    for (int index = 0; index < 6; ++index)
    {
        const std::int64_t nodeId = harness.createNode("rollout-manual-" + std::to_string(index));
        harness.heartbeat(nodeId, "0.9.0");
        nodes.push_back(nodeId);
    }
    const std::int64_t firmwareId = harness.createFirmware("2.0.9");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "人工动作", kThreeWavePolicy, nodes);
    harness.tick(base);
    ASSERT_EQ(2U, harness.published.size());

    auto paused = harness.m_runner->run(harness.m_service->pause(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rolloutId, "维护窗口外"));
    ASSERT_TRUE(paused.has_value()) << (paused.has_value() ? "" : paused.error().message);
    auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("paused", aggregate.rollout.status);
    EXPECT_EQ("维护窗口外", aggregate.rollout.pauseReason);
    harness.tick(later(base, 3));
    EXPECT_EQ(2U, harness.published.size());

    const auto resumed = harness.m_runner->run(harness.m_service->resume(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rolloutId, later(base, 4)));
    ASSERT_TRUE(resumed.has_value()) << (resumed.has_value() ? "" : resumed.error().message);
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("running", aggregate.rollout.status);
    EXPECT_EQ(EdgePulse::Common::formatDateTimeUtc(later(base, 4)), aggregate.waves[0].dispatchedAt);

    harness.completeAll(0, "2.0.9", later(base, 4));
    harness.tick(later(base, 6));
    harness.tick(later(base, 8));
    EXPECT_EQ(4U, harness.published.size());
    EXPECT_EQ(1, harness.aggregate(rolloutId).rollout.currentWave);

    const auto promoted = harness.m_runner->run(harness.m_service->promote(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rolloutId, later(base, 9)));
    ASSERT_TRUE(promoted.has_value()) << (promoted.has_value() ? "" : promoted.error().message);
    EXPECT_EQ(2, harness.aggregate(rolloutId).rollout.currentWave);

    const auto aborted = harness.m_runner->run(harness.m_service->abort(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rolloutId, later(base, 10)));
    ASSERT_TRUE(aborted.has_value()) << (aborted.has_value() ? "" : aborted.error().message);
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("aborted", aggregate.rollout.status);
    EXPECT_FALSE(aggregate.rollout.finishedAt.empty());
    harness.tick(later(base, 20));
    EXPECT_EQ(4U, harness.published.size());
}

// 钉住：强制提升必须有原因；人工回滚从暂停转入 rolling_back 并由 tick 下发历史版本
TEST(FirmwareRolloutService, ForcePromoteRequiresReasonAndManualRollback)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t first  = harness.createNode("rollout-force-a");
    const std::int64_t second = harness.createNode("rollout-force-b");
    harness.heartbeat(first, "1.0.0");
    harness.heartbeat(second, "1.0.0");
    const std::int64_t target    = harness.createFirmware("2.1.0");
    const std::int64_t rolloutId = harness.createRollout(target, "强制提升与回滚", kOneWavePolicy, {first, second});

    const auto missingReason = harness.m_runner->run(harness.m_service->forcePromote(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rolloutId, ""));
    ASSERT_FALSE(missingReason.has_value());
    EXPECT_EQ(EdgePulse::Firmware::FirmwareRolloutErrorKind::InvalidInput, missingReason.error().kind);

    harness.tick(base);
    ASSERT_EQ(2U, harness.published.size());
    const auto paused = harness.m_runner->run(harness.m_service->pause(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rolloutId, {}));
    ASSERT_TRUE(paused.has_value());
    const auto forced =
            harness.m_runner->run(harness.m_service->forcePromote(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rolloutId, "现场确认可接受", later(base, 3)));
    ASSERT_TRUE(forced.has_value()) << (forced.has_value() ? "" : forced.error().message);
    auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("completed", aggregate.rollout.status);
    EXPECT_NE(std::string::npos, aggregate.waves[0].gateJson.find("现场确认可接受"));

    RolloutHarness     rollbackHarness;
    const std::int64_t nodeId = rollbackHarness.createNode("rollout-manual-rollback");
    rollbackHarness.heartbeat(nodeId, "1.0.0");
    const std::int64_t rollbackPrevious = rollbackHarness.createFirmware("1.0.0");
    const std::int64_t rollbackTarget   = rollbackHarness.createFirmware("2.1.1");
    const char        *policy           = R"({"waves":[{"size":1}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30})";
    const std::int64_t rollbackId       = rollbackHarness.createRollout(rollbackTarget, "人工回滚", policy, {nodeId});
    rollbackHarness.tick(base);
    ASSERT_EQ(1U, rollbackHarness.published.size());
    static_cast<void>(rollbackHarness.m_runner->run(rollbackHarness.m_service->pause(rollbackHarness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rollbackId, {})));
    const auto rolledBack =
            rollbackHarness.m_runner->run(rollbackHarness.m_service->rollback(rollbackHarness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rollbackId));
    ASSERT_TRUE(rolledBack.has_value()) << (rolledBack.has_value() ? "" : rolledBack.error().message);
    EXPECT_EQ("rolling_back", rollbackHarness.aggregate(rollbackId).rollout.status);
    rollbackHarness.tick(later(base, 3));
    EXPECT_EQ("rolled_back", rollbackHarness.aggregate(rollbackId).rollout.status);
    ASSERT_EQ(2U, rollbackHarness.published.size());
    EXPECT_EQ(rollbackPrevious, firmwareIdOf(rollbackHarness.published[1]));
}

// 钉住：非 leader 不推进，取得领导权后才派发
TEST(FirmwareRolloutService, OnlyLeaderTickDispatches)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-leader");
    harness.heartbeat(nodeId, "0.9.0");
    const std::int64_t firmwareId = harness.createFirmware("2.1.2");
    const char        *policy     = R"({"waves":[{"size":1}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30})";
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "单飞", policy, {nodeId});

    bool                          leader = false;
    EdgePulse::System::LeaderGate gate("灰度单飞测试", "instance-test", [&leader] { return leader; });
    EdgePulse::System::LeaderTick tick(gate, [&harness, base]() -> AsynGyanis::Core::Task<> { co_await harness.m_service->tick(harness.m_runner->loop(), base); });
    harness.m_runner->run(tick.run());
    EXPECT_EQ(0U, harness.published.size());
    EXPECT_EQ("scheduled", harness.aggregate(rolloutId).rollout.status);

    leader = true;
    harness.m_runner->run(tick.run());
    EXPECT_EQ(1U, harness.published.size());
    EXPECT_EQ("running", harness.aggregate(rolloutId).rollout.status);
}

// 钉住：策略默认值与非法输入在创建阶段被拒绝
TEST(FirmwareRolloutService, PolicyDefaultsAndValidation)
{
    const auto defaults = EdgePulse::Firmware::FirmwareRolloutPolicy::parse(R"({"waves":[{"percent":5},{"percent":25},{"percent":100}]})", 30);
    ASSERT_TRUE(defaults.has_value()) << defaults.error();
    EXPECT_EQ(60, defaults->settleSeconds);
    EXPECT_EQ(120, defaults->observeSeconds);
    EXPECT_EQ(1800, defaults->maxWaveSeconds);
    EXPECT_DOUBLE_EQ(1.0, defaults->minWaveSuccessRatio);
    EXPECT_DOUBLE_EQ(1.0, defaults->minCumulativeSuccessRatio);
    EXPECT_FALSE(defaults->autoRollback);
    EXPECT_EQ("pause", defaults->onTimeout);
    EXPECT_EQ(300, defaults->requiredFreshTelemetrySeconds);
    ASSERT_EQ(3U, defaults->waves.size());
    EXPECT_TRUE(defaults->waves[0].isPercent);

    EXPECT_FALSE(EdgePulse::Firmware::FirmwareRolloutPolicy::parse(R"({"waves":[]})", 30).has_value());
    EXPECT_FALSE(EdgePulse::Firmware::FirmwareRolloutPolicy::parse(R"({"waves":[{"percent":0}]})", 30).has_value());
    EXPECT_FALSE(EdgePulse::Firmware::FirmwareRolloutPolicy::parse(R"({"waves":[{"size":0}]})", 30).has_value());
    EXPECT_FALSE(EdgePulse::Firmware::FirmwareRolloutPolicy::parse(R"({"waves":[{"percent":5,"size":2}]})", 30).has_value());
    EXPECT_FALSE(EdgePulse::Firmware::FirmwareRolloutPolicy::parse(R"({"waves":[{"percent":5}],"settleSeconds":1})", 30).has_value());
    EXPECT_FALSE(EdgePulse::Firmware::FirmwareRolloutPolicy::parse(R"({"waves":[{"percent":5}],"onTimeout":"resume"})", 30).has_value());
    EXPECT_FALSE(EdgePulse::Firmware::FirmwareRolloutPolicy::parse(R"({"waves":[{"percent":5}],"unknown":1})", 30).has_value());
    EXPECT_FALSE(EdgePulse::Firmware::FirmwareRolloutPolicy::parse(R"({"waves":[{"percent":5}],"maxWaveSeconds":10})", 30).has_value());

    const auto planned = EdgePulse::Firmware::planFirmwareRolloutWaves(*defaults, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20});
    ASSERT_TRUE(planned.has_value()) << planned.error();
    ASSERT_EQ(3U, planned->size());
    EXPECT_EQ(1U, (*planned)[0].size());
    EXPECT_EQ(4U, (*planned)[1].size());
    EXPECT_EQ(15U, (*planned)[2].size());

    const auto maintenance =
            EdgePulse::Firmware::FirmwareRolloutPolicy::parse(R"({"waves":[{"percent":100}],"maintenanceWindow":{"start":"23:00","end":"01:00","days":[1,2,3]}})", 30);
    ASSERT_TRUE(maintenance.has_value()) << maintenance.error();
    EXPECT_FALSE(maintenance->allowsTime(std::chrono::sys_days{std::chrono::year{2026} / std::chrono::January / 5} + std::chrono::hours{12}));
}

// 钉住：创建阶段拒绝空目标、重复目标、跨租户目标与未签名固件
TEST(FirmwareRolloutService, CreateValidatesTargetsAndSignature)
{
    RolloutHarness        harness(true);
    const std::int64_t    nodeId           = harness.createNode("rollout-create-a");
    const std::int64_t    otherId          = harness.createNode("rollout-create-b", 2);
    const std::int64_t    unsignedFirmware = harness.createFirmware("3.0.0", "");
    const std::int64_t    signedFirmware   = harness.createFirmware("3.0.1", "signature");
    constexpr const char *policy           = R"({"waves":[{"size":1}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30})";

    const auto emptyTargets =
            harness.m_runner->run(harness.m_service->create(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, 7, signedFirmware, "空目标", policy, {}));
    ASSERT_FALSE(emptyTargets.has_value());
    EXPECT_EQ(EdgePulse::Firmware::FirmwareRolloutErrorKind::InvalidInput, emptyTargets.error().kind);

    const auto duplicate = harness.m_runner->run(
            harness.m_service->create(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, 7, signedFirmware, "重复目标", policy, {nodeId, nodeId}));
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_NE(std::string::npos, duplicate.error().message.find("重复"));

    const auto crossTenant =
            harness.m_runner->run(harness.m_service->create(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, 7, signedFirmware, "跨租户", policy, {otherId}));
    ASSERT_FALSE(crossTenant.has_value());
    EXPECT_EQ(EdgePulse::Firmware::FirmwareRolloutErrorKind::InvalidInput, crossTenant.error().kind);

    const auto unsignedTarget =
            harness.m_runner->run(harness.m_service->create(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, 7, unsignedFirmware, "未签名", policy, {nodeId}));
    ASSERT_FALSE(unsignedTarget.has_value());
    EXPECT_NE(std::string::npos, unsignedTarget.error().message.find("签名"));

    const auto missingFirmware =
            harness.m_runner->run(harness.m_service->create(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, 7, 999999999, "不存在", policy, {nodeId}));
    ASSERT_FALSE(missingFirmware.has_value());
    EXPECT_EQ(EdgePulse::Firmware::FirmwareRolloutErrorKind::NotFound, missingFirmware.error().kind);
}
