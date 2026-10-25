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
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
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
            m_service   = std::make_unique<EdgePulse::Firmware::FirmwareRolloutService>(dependencies());
        }

        /**
         * @brief 按同一批仓储重建服务，模拟进程重启后从同一数据库恢复
         */
        void restartService()
        {
            m_service = std::make_unique<EdgePulse::Firmware::FirmwareRolloutService>(dependencies());
        }

        /**
         * @brief 组装服务依赖；进程重启与时序用例共用同一发布口
         */
        [[nodiscard]] EdgePulse::Firmware::FirmwareRolloutDependencies dependencies()
        {
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
                if (failPublishes > 0)
                {
                    --failPublishes;
                    throw std::runtime_error("命令总线不可用");
                }
                if (afterPublish)
                {
                    afterPublish();
                }
                co_return;
            };
            return dependencies;
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

        /**
         * @brief 让心跳时间与 tick 的注入时钟一致，测试不依赖真实时间
         */
        void heartbeatAt(const std::int64_t nodeId, const std::string &version, const TimePoint at)
        {
            heartbeat(nodeId, version);
            executeSql("UPDATE biz_node SET last_seen = '" + EdgePulse::Common::formatDateTimeUtc(at) + "' WHERE id = " + std::to_string(nodeId));
        }

        /**
         * @brief 直接改库，模拟真实时间之外的遥测接收时间与节点状态
         */
        void executeSql(const std::string &sql)
        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseConnection> connection = EdgePulse::Common::Database::openDirectSqlite(m_context->databasePath().string());
            ASSERT_NE(nullptr, connection);
            ASSERT_NE(nullptr, connection->execute(sql)) << connection->lastError();
        }

        void setNodeOffline(const std::int64_t nodeId)
        {
            executeSql("UPDATE biz_node SET status = 'offline' WHERE id = " + std::to_string(nodeId));
        }

        void pauseRolloutDirect(const std::int64_t rolloutId)
        {
            executeSql("UPDATE biz_firmware_rollout SET status = 'paused', version = version + 1 WHERE id = " + std::to_string(rolloutId));
        }

        void setRolloutStatusDirect(const std::int64_t rolloutId, const std::string &status)
        {
            executeSql("UPDATE biz_firmware_rollout SET status = '" + status + "', version = version + 1 WHERE id = " + std::to_string(rolloutId));
        }

        /**
         * @brief 让指定目标写回 rolling_back 时抛错，模拟回滚派发循环中途被打断
         */
        void failRollbackTargetUpdate(const std::int64_t nodeId)
        {
            executeSql("CREATE TRIGGER fail_rollback_target_update BEFORE UPDATE OF status ON biz_firmware_rollout_target "
                       "WHEN NEW.node_id = " +
                       std::to_string(nodeId) +
                       " AND NEW.status = 'rolling_back' "
                       "BEGIN SELECT RAISE(ABORT, '回滚目标写入失败'); END");
        }

        void dropTrigger(const std::string &name)
        {
            executeSql("DROP TRIGGER IF EXISTS " + name);
        }

        void failTaskBinds()
        {
            executeSql("CREATE TRIGGER fail_task_bind BEFORE UPDATE OF command_id ON biz_firmware_task "
                       "WHEN NEW.command_id > 0 AND OLD.command_id <= 0 BEGIN SELECT RAISE(ABORT, '绑定失败'); END");
        }

        [[nodiscard]] std::string commandStatus(const std::int64_t commandId)
        {
            const auto command = m_runner->run(m_commands->find(m_runner->loop(), commandId));
            return command.has_value() ? command->status : std::string{};
        }

        [[nodiscard]] std::optional<EdgePulse::Firmware::FirmwareTaskRecord> task(const std::int64_t taskId)
        {
            return m_runner->run(m_firmware->findTask(m_runner->loop(), taskId));
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
                    heartbeatAt(command.nodeId, version, now);
                }
                insertTelemetryAt(command.nodeId, now, now);
            } else
            {
                static_cast<void>(m_runner->run(m_commands->finish(m_runner->loop(), command.id, "failed", "{}", "安装失败")));
                static_cast<void>(m_runner->run(m_firmware->updateTask(m_runner->loop(), firmwareId, command.nodeId, "failed", 0, "安装失败", command.id)));
            }
        }

        /**
         * @brief 只完成命令与任务，不写遥测，供新鲜度等待用例构造缺口
         */
        void finishCommandWithoutTelemetry(const std::size_t index, const std::string &version, const TimePoint at)
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
            static_cast<void>(m_runner->run(m_commands->finish(m_runner->loop(), command.id, "succeeded", "{}", "ok")));
            static_cast<void>(m_runner->run(m_firmware->updateTask(m_runner->loop(), firmwareId, command.nodeId, "installed", 100, "ok", command.id)));
            heartbeatAt(command.nodeId, version, at);
        }

        void insertTelemetry(const std::int64_t nodeId, const TimePoint now)
        {
            insertTelemetryAt(nodeId, now, now);
        }

        /**
         * @brief 写入一条遥测并把服务端接收时间对齐到注入时钟
         */
        void insertTelemetryAt(const std::int64_t nodeId, const TimePoint collectedAt, const TimePoint createdAt)
        {
            EdgePulse::Telemetry::TelemetrySample sample;
            sample.metric      = "cpu.usage";
            sample.value       = 12.0;
            sample.unit        = "%";
            sample.collectedAt = EdgePulse::Common::formatDateTimeUtc(collectedAt);
            static_cast<void>(m_runner->run(m_telemetry->insertBatch(m_runner->loop(), nodeId, {sample})));
            executeSql("UPDATE biz_telemetry SET created_at = '" + EdgePulse::Common::formatDateTimeUtc(createdAt) +
                       "' WHERE id = (SELECT MAX(id) FROM biz_telemetry WHERE node_id = " + std::to_string(nodeId) + ")");
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
        int                                                             failPublishes{0};
        std::function<void()>                                           afterPublish;
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
    constexpr const char *kSinglePolicy    = R"({"waves":[{"size":1}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30})";
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
    harness.completeAll(0, "2.0.0", later(base, 2));
    harness.tick(later(base, 2));
    EXPECT_EQ(firstWave, harness.published.size());
    harness.tick(later(base, 4));
    EXPECT_EQ(4U, harness.published.size());
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ(1, aggregate.rollout.currentWave);
    EXPECT_EQ("passed", aggregate.waves[0].status);

    harness.completeAll(firstWave, "2.0.0", later(base, 6));
    harness.tick(later(base, 6));
    harness.tick(later(base, 8));
    EXPECT_EQ(6U, harness.published.size());
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ(2, aggregate.rollout.currentWave);
    EXPECT_EQ("passed", aggregate.waves[1].status);

    harness.completeAll(4, "2.0.0", later(base, 10));
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
    harness.completeCommand(0, "2.0.1", later(base, 2));
    harness.completeCommand(1, "2.0.1", later(base, 2), false);

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
    EXPECT_EQ("rolling_back", aggregate.rollout.status);
    ASSERT_EQ(4U, harness.published.size());
    EXPECT_EQ(previous, firmwareIdOf(harness.published[2]));
    EXPECT_EQ(previous, firmwareIdOf(harness.published[3]));
    for (const EdgePulse::Firmware::FirmwareRolloutTargetRecord &record: aggregate.targets)
    {
        EXPECT_EQ("rolling_back", record.status);
        EXPECT_GT(record.commandId, 0);
    }

    harness.completeCommand(2, "1.0.0", later(base, 4));
    harness.completeCommand(3, "1.0.0", later(base, 4));
    harness.tick(later(base, 5));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("rolled_back", aggregate.rollout.status);
    for (const EdgePulse::Firmware::FirmwareRolloutTargetRecord &record: aggregate.targets)
    {
        EXPECT_EQ("rolled_back", record.status);
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

    harness.completeCommand(0, "2.4.8", later(base, 2));
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

// 钉住：观察窗口结束时仍未确认的目标必须落失败，onTimeout 决定暂停还是失败
TEST(FirmwareRolloutService, ObserveEndUnresolvedPausesOrFailsByPolicy)
{
    const char *pausePolicy = R"({"waves":[{"size":1}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":10,"onTimeout":"pause"})";
    const char *failPolicy  = R"({"waves":[{"size":1}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":10,"onTimeout":"fail"})";

    RolloutHarness     pauseHarness;
    const TimePoint    base      = fakeBase();
    const std::int64_t pauseNode = pauseHarness.createNode("rollout-timeout-pause");
    pauseHarness.heartbeatAt(pauseNode, "0.9.0", later(base, 1));
    const std::int64_t pauseFirmware = pauseHarness.createFirmware("2.0.4");
    const std::int64_t pauseRollout  = pauseHarness.createRollout(pauseFirmware, "超时暂停", pausePolicy, {pauseNode});
    pauseHarness.tick(base);
    ASSERT_EQ(1U, pauseHarness.published.size());
    // 命令迟迟不结束：观察窗口一到就判失败，不再无限等到 maxWave
    pauseHarness.insertTelemetry(pauseNode, later(base, 2));
    pauseHarness.tick(later(base, 2));
    EXPECT_EQ("running", pauseHarness.aggregate(pauseRollout).rollout.status);
    pauseHarness.tick(later(base, 5));
    auto aggregate = pauseHarness.aggregate(pauseRollout);
    EXPECT_EQ("paused", aggregate.rollout.status);
    EXPECT_EQ("failed", aggregate.waves[0].status);
    EXPECT_NE(std::string::npos, aggregate.rollout.pauseReason.find("超时"));

    RolloutHarness     failHarness;
    const std::int64_t failNode = failHarness.createNode("rollout-timeout-fail");
    failHarness.heartbeatAt(failNode, "0.9.0", later(base, 1));
    const std::int64_t failFirmware = failHarness.createFirmware("2.0.5");
    const std::int64_t failRollout  = failHarness.createRollout(failFirmware, "超时失败", failPolicy, {failNode});
    failHarness.tick(base);
    failHarness.insertTelemetry(failNode, later(base, 2));
    failHarness.tick(later(base, 5));
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
    harness.completeCommand(0, "2.0.6", later(base, 2));
    // 第二台保持离线：命令与任务完成但没有心跳，节点仍判离线
    harness.completeCommand(1, "2.0.6", later(base, 2), true, false);
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
    noTelemetry.completeCommand(0, "2.0.7", later(base, 2));
    noTelemetry.completeCommand(1, "2.0.7", later(base, 2));
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
    EXPECT_EQ("running", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, second));
    EXPECT_EQ("installing", targetOf(aggregate, second)->status);

    // 遥测一直不到：观察窗口内保持等待，窗口结束立即判失败
    noTelemetry.tick(later(base, 5));
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
    harness.completeCommand(0, "2.4.7", later(base, 2));
    harness.completeCommand(1, "2.4.7", later(base, 2));
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
    harness.completeCommand(0, "2.0.8", later(base, 3));
    harness.completeCommand(1, "2.0.8", later(base, 3));
    harness.tick(later(base, 1));
    EXPECT_EQ(4U, harness.published.size());
    auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("observing", aggregate.waves[0].status);
    EXPECT_EQ(EdgePulse::Common::formatDateTimeUtc(later(base, 1)), aggregate.waves[0].dispatchedAt);
    harness.completeAll(2, "2.0.8", later(base, 3));
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

    harness.completeAll(0, "2.0.9", later(base, 6));
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

// 钉住：暂停与强制提升的原因按列上限截断，超长正文不能写穿状态机
TEST(FirmwareRolloutService, BoundsPauseAndForcePromoteReasons)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-bounded-reason");
    harness.heartbeatAt(nodeId, "0.9.0", later(base, 1));
    const std::int64_t firmwareId = harness.createFirmware("2.1.2");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "原因截断", kSinglePolicy, {nodeId});
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());

    std::string longReason;
    for (int index = 0; index < 200; ++index)
    {
        longReason += "中";
    }
    const auto paused = harness.m_runner->run(harness.m_service->pause(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rolloutId, longReason));
    ASSERT_TRUE(paused.has_value()) << (paused.has_value() ? "" : paused.error().message);
    auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ(240U, aggregate.rollout.pauseReason.size());
    EXPECT_EQ("中", aggregate.rollout.pauseReason.substr(237));

    const auto resumed = harness.m_runner->run(harness.m_service->resume(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rolloutId, later(base, 2)));
    ASSERT_TRUE(resumed.has_value()) << (resumed.has_value() ? "" : resumed.error().message);
    const auto forced = harness.m_runner->run(harness.m_service->forcePromote(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rolloutId, longReason,
                                                                              later(base, 3)));
    ASSERT_TRUE(forced.has_value()) << (forced.has_value() ? "" : forced.error().message);
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("completed", aggregate.rollout.status);
    EXPECT_EQ(480U, aggregate.rollout.lastError.size());
    EXPECT_NE(std::string::npos, aggregate.waves[0].gateJson.find(longReason.substr(0, 480)));
}

// 钉住：发布头 CAS 失败时不能先写波次或取消目标，人工动作必须在头写入成功后收尾
TEST(FirmwareRolloutService, FailedHeaderCasLeavesActionWritesUntouched)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-action-cas");
    harness.heartbeatAt(nodeId, "0.9.0", later(base, 1));
    const std::int64_t firmwareId = harness.createFirmware("2.7.3");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "动作 CAS", kSinglePolicy, {nodeId});
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());
    harness.executeSql("CREATE TRIGGER fail_rollout_complete BEFORE UPDATE OF status ON biz_firmware_rollout "
                       "WHEN NEW.status = 'completed' BEGIN SELECT RAISE(ABORT, '状态写入失败'); END");

    bool threw = false;
    try
    {
        static_cast<void>(harness.m_runner->run(harness.m_service->promote(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rolloutId, later(base, 1))));
    } catch (const std::exception &)
    {
        threw = true;
    }
    EXPECT_TRUE(threw) << "数据库拒绝发布头写入时动作应失败";
    const auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("running", aggregate.rollout.status);
    ASSERT_EQ(1U, aggregate.waves.size());
    EXPECT_EQ("observing", aggregate.waves[0].status);
    ASSERT_NE(nullptr, targetOf(aggregate, nodeId));
    EXPECT_EQ("installing", targetOf(aggregate, nodeId)->status);
    EXPECT_EQ("pending", harness.commandStatus(harness.published[0].id)) << "头写入失败前不能取消目标命令";
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
    const auto blankReason = harness.m_runner->run(harness.m_service->forcePromote(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rolloutId, "   "));
    ASSERT_FALSE(blankReason.has_value());
    EXPECT_EQ(EdgePulse::Firmware::FirmwareRolloutErrorKind::InvalidInput, blankReason.error().kind);

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
    auto rollbackAggregate = rollbackHarness.aggregate(rollbackId);
    EXPECT_EQ("rolling_back", rollbackAggregate.rollout.status);
    ASSERT_EQ(2U, rollbackHarness.published.size());
    EXPECT_EQ(rollbackPrevious, firmwareIdOf(rollbackHarness.published[1]));
    EXPECT_EQ("rolling_back", rollbackAggregate.targets[0].status);

    rollbackHarness.completeCommand(1, "1.0.0", later(base, 4));
    rollbackHarness.tick(later(base, 5));
    EXPECT_EQ("rolled_back", rollbackHarness.aggregate(rollbackId).rollout.status);
}

// 钉住：回滚派发中途抛错后，下一拍补齐剩余目标，未确认前不落 rolled_back
TEST(FirmwareRolloutService, RollbackResumesRemainingTargetsAfterInterruption)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t first  = harness.createNode("rollout-resume-a");
    const std::int64_t second = harness.createNode("rollout-resume-b");
    harness.heartbeatAt(first, "1.0.0", later(base, 1));
    harness.heartbeatAt(second, "1.0.0", later(base, 1));
    const std::int64_t previous  = harness.createFirmware("1.0.0");
    const std::int64_t target    = harness.createFirmware("2.9.0");
    const char        *policy    = R"({"waves":[{"size":2}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30,"autoRollback":true})";
    const std::int64_t rolloutId = harness.createRollout(target, "回滚中断恢复", policy, {first, second});
    harness.tick(base);
    ASSERT_EQ(2U, harness.published.size());
    // 两台都已装上新固件，第二台因告警判失败；回滚未完成前它仍停留在新固件
    harness.completeCommand(0, "2.9.0", later(base, 2));
    harness.completeCommand(1, "2.9.0", later(base, 2));
    harness.raiseAlarm(second, "cpu.high", 201);
    harness.tick(later(base, 2));
    ASSERT_EQ("rolling_back", harness.aggregate(rolloutId).rollout.status);

    harness.failRollbackTargetUpdate(second);
    harness.tick(later(base, 3));
    ASSERT_EQ(3U, harness.published.size());
    ASSERT_EQ(first, harness.published[2].nodeId);
    ASSERT_EQ(previous, firmwareIdOf(harness.published[2]));
    auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("rolling_back", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, first));
    ASSERT_NE(nullptr, targetOf(aggregate, second));
    EXPECT_EQ("rolling_back", targetOf(aggregate, first)->status);
    // 第二台的目标绑定被中断，回滚命令没有发布，目标回到派发前状态
    EXPECT_EQ("failed", targetOf(aggregate, second)->status);
    harness.dropTrigger("fail_rollback_target_update");

    harness.tick(later(base, 4));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("rolling_back", aggregate.rollout.status);
    ASSERT_EQ(4U, harness.published.size());
    EXPECT_EQ(second, harness.published[3].nodeId);
    ASSERT_NE(nullptr, targetOf(aggregate, second));
    EXPECT_EQ("rolling_back", targetOf(aggregate, second)->status);
    EXPECT_EQ(harness.published[3].id, targetOf(aggregate, second)->commandId);
    // 恢复时沿用已经绑定的任务与命令，不重复建单
    EXPECT_EQ(4U, harness.published.size());
    harness.tick(later(base, 4));
    EXPECT_EQ(4U, harness.published.size());

    harness.completeCommand(2, "1.0.0", later(base, 5));
    harness.tick(later(base, 5));
    aggregate = harness.aggregate(rolloutId);
    // 第一台确认后第二台仍未确认，不得提前落 rolled_back
    EXPECT_EQ("rolling_back", aggregate.rollout.status);
    EXPECT_EQ("rolled_back", targetOf(aggregate, first)->status);
    EXPECT_EQ("rolling_back", targetOf(aggregate, second)->status);

    harness.completeCommand(3, "1.0.0", later(base, 6));
    harness.tick(later(base, 6));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("rolled_back", aggregate.rollout.status);
    EXPECT_EQ("rolled_back", targetOf(aggregate, first)->status);
    EXPECT_EQ("rolled_back", targetOf(aggregate, second)->status);
}

// 钉住：进程重启后从同一数据库恢复回滚派发，未确认的目标不会被跳过
TEST(FirmwareRolloutService, RollbackResumesAfterServiceRestart)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t first  = harness.createNode("rollout-restart-a");
    const std::int64_t second = harness.createNode("rollout-restart-b");
    harness.heartbeatAt(first, "1.0.0", later(base, 1));
    harness.heartbeatAt(second, "1.0.0", later(base, 1));
    const std::int64_t previous  = harness.createFirmware("1.0.0");
    const std::int64_t target    = harness.createFirmware("2.9.1");
    const char        *policy    = R"({"waves":[{"size":2}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30,"autoRollback":true})";
    const std::int64_t rolloutId = harness.createRollout(target, "回滚重启恢复", policy, {first, second});
    harness.tick(base);
    ASSERT_EQ(2U, harness.published.size());
    harness.completeCommand(0, "2.9.1", later(base, 2));
    harness.completeCommand(1, "2.9.1", later(base, 2), false);
    harness.tick(later(base, 2));
    ASSERT_EQ("rolling_back", harness.aggregate(rolloutId).rollout.status);

    // 第一台回滚命令发出后直接暂停，模拟派发循环被进程边界截断
    harness.afterPublish = [&harness, rolloutId]
    {
        if (harness.published.size() == 3)
        {
            harness.pauseRolloutDirect(rolloutId);
        }
    };
    harness.tick(later(base, 3));
    harness.afterPublish = nullptr;
    ASSERT_EQ(3U, harness.published.size());
    auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("paused", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, first));
    ASSERT_NE(nullptr, targetOf(aggregate, second));
    // 命令已送达但发布被暂停：命令取消、目标恢复派发前状态，恢复后重新派发
    EXPECT_EQ("healthy", targetOf(aggregate, first)->status);
    EXPECT_EQ("failed", targetOf(aggregate, second)->status);

    harness.setRolloutStatusDirect(rolloutId, "rolling_back");
    harness.restartService();
    harness.tick(later(base, 4));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("rolling_back", aggregate.rollout.status);
    EXPECT_EQ("rolling_back", targetOf(aggregate, first)->status);
    EXPECT_EQ("rolling_back", targetOf(aggregate, second)->status);
    ASSERT_EQ(5U, harness.published.size());
    EXPECT_EQ(first, harness.published[3].nodeId);
    EXPECT_EQ(second, harness.published[4].nodeId);
    EXPECT_EQ(previous, firmwareIdOf(harness.published[3]));
    EXPECT_EQ(previous, firmwareIdOf(harness.published[4]));

    harness.completeCommand(3, "1.0.0", later(base, 5));
    harness.tick(later(base, 5));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("rolling_back", aggregate.rollout.status);
    EXPECT_EQ("rolled_back", targetOf(aggregate, first)->status);
    EXPECT_EQ("rolling_back", targetOf(aggregate, second)->status);

    harness.completeCommand(4, "1.0.0", later(base, 6));
    harness.tick(later(base, 6));
    EXPECT_EQ("rolled_back", harness.aggregate(rolloutId).rollout.status);
}

// 钉住：安装中的任务同样在中止时被关闭，而不是留在 installing
TEST(FirmwareRolloutService, AbortCancelsInstallingTasks)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-abort-installing");
    harness.heartbeatAt(nodeId, "0.9.0", later(base, 1));
    const std::int64_t firmwareId = harness.createFirmware("2.9.2");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "中止安装中任务", kSinglePolicy, {nodeId});
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());
    auto aggregate = harness.aggregate(rolloutId);
    ASSERT_NE(nullptr, targetOf(aggregate, nodeId));
    const std::int64_t taskId = targetOf(aggregate, nodeId)->taskId;
    ASSERT_GT(taskId, 0);
    static_cast<void>(harness.m_runner->run(harness.m_firmware->updateTask(harness.m_runner->loop(), firmwareId, nodeId, "installing", 100, "安装中", harness.published[0].id)));

    const auto aborted = harness.m_runner->run(harness.m_service->abort(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rolloutId, later(base, 1)));
    ASSERT_TRUE(aborted.has_value()) << (aborted.has_value() ? "" : aborted.error().message);
    EXPECT_EQ("canceled", harness.commandStatus(harness.published[0].id));
    const auto task = harness.task(taskId);
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ("failed", task->status);
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("failed", targetOf(aggregate, nodeId)->status);
}

// 钉住：已进入观察的波次不会每拍重写 updated_at
TEST(FirmwareRolloutService, ObservingWaveIsNotRewrittenEachTick)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t first  = harness.createNode("rollout-observe-a");
    const std::int64_t second = harness.createNode("rollout-observe-b");
    harness.heartbeatAt(first, "0.9.0", later(base, 1));
    harness.heartbeatAt(second, "0.9.0", later(base, 1));
    const std::int64_t firmwareId = harness.createFirmware("2.9.3");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "观察不重写", kOneWavePolicy, {first, second});
    harness.tick(base);
    ASSERT_EQ(2U, harness.published.size());
    auto aggregate = harness.aggregate(rolloutId);
    ASSERT_EQ(1U, aggregate.waves.size());
    ASSERT_EQ("observing", aggregate.waves[0].status);
    harness.executeSql("UPDATE biz_firmware_rollout_wave SET updated_at = '2000-01-01 00:00:00' WHERE id = " + std::to_string(aggregate.waves[0].id));

    harness.tick(base);
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("2000-01-01 00:00:00", aggregate.waves[0].updatedAt);
    EXPECT_EQ(2U, harness.published.size());
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
    EXPECT_TRUE(defaults->requireInstalledVersion);
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

    // 相同百分比允许出现空波，最终覆盖全部目标；最后一波未覆盖全部目标时拒绝
    const auto equal = EdgePulse::Firmware::FirmwareRolloutPolicy::parse(R"({"waves":[{"percent":50},{"percent":50},{"percent":100}]})", 30);
    ASSERT_TRUE(equal.has_value()) << equal.error();
    const auto equalPlan = EdgePulse::Firmware::planFirmwareRolloutWaves(*equal, {1, 2, 3, 4});
    ASSERT_TRUE(equalPlan.has_value()) << equalPlan.error();
    EXPECT_EQ(2U, (*equalPlan)[0].size());
    EXPECT_TRUE((*equalPlan)[1].empty());
    EXPECT_EQ(2U, (*equalPlan)[2].size());

    const auto partial = EdgePulse::Firmware::FirmwareRolloutPolicy::parse(R"({"waves":[{"percent":50}]})", 30);
    ASSERT_TRUE(partial.has_value()) << partial.error();
    const auto partialPlan = EdgePulse::Firmware::planFirmwareRolloutWaves(*partial, {1, 2, 3, 4});
    ASSERT_FALSE(partialPlan.has_value());
    EXPECT_NE(std::string::npos, partialPlan.error().find("覆盖"));

    const auto maintenance = EdgePulse::Firmware::FirmwareRolloutPolicy::parse(R"({"waves":[{"percent":100}],"maintenanceWindow":{"start":"23:00","end":"01:00","days":[1]}})", 30);
    ASSERT_TRUE(maintenance.has_value()) << maintenance.error();
    const TimePoint monday  = std::chrono::sys_days{std::chrono::year{2026} / std::chrono::January / 5};
    const TimePoint tuesday = monday + std::chrono::hours{24};
    EXPECT_TRUE(maintenance->allowsTime(monday + std::chrono::hours{23} + std::chrono::minutes{30}));
    EXPECT_TRUE(maintenance->allowsTime(tuesday + std::chrono::minutes{30}));
    EXPECT_FALSE(maintenance->allowsTime(tuesday + std::chrono::hours{1} + std::chrono::minutes{30}));
    EXPECT_FALSE(maintenance->allowsTime(monday + std::chrono::hours{12}));
    EXPECT_FALSE(maintenance->allowsTime(monday + std::chrono::hours{22}));
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

// 钉住：派发后旧的遥测不立即判失败，等观察窗口内出现新样本后仍可通过
TEST(FirmwareRolloutService, TelemetryFreshnessWaitsForNewSample)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-telemetry-fresh");
    harness.heartbeatAt(nodeId, "1.0.0", later(base, 1));
    const std::int64_t firmwareId = harness.createFirmware("2.5.0");
    const char        *policy     = R"({"waves":[{"size":1}],"settleSeconds":2,"observeSeconds":6,"maxWaveSeconds":30,"requiredFreshTelemetrySeconds":3})";
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "遥测刷新", policy, {nodeId});
    harness.insertTelemetryAt(nodeId, later(base, -10), later(base, -10));
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());

    harness.finishCommandWithoutTelemetry(0, "2.5.0", later(base, 2));
    harness.tick(later(base, 2));
    auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("running", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, nodeId));
    EXPECT_EQ("installing", targetOf(aggregate, nodeId)->status);
    EXPECT_NE(std::string::npos, targetOf(aggregate, nodeId)->healthJson.find("遥测"));

    harness.insertTelemetryAt(nodeId, later(base, 3), later(base, 3));
    harness.tick(later(base, 4));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("running", aggregate.rollout.status);
    EXPECT_EQ("healthy", targetOf(aggregate, nodeId)->status);

    harness.tick(later(base, 6));
    EXPECT_EQ("completed", harness.aggregate(rolloutId).rollout.status);
}

// 钉住：缺少或错配已安装版本时等待到观察窗口结束仍不确认，最终按超时失败
TEST(FirmwareRolloutService, InstalledVersionEvidenceWaitsThenTimesOut)
{
    RolloutHarness     harness;
    const TimePoint    base       = fakeBase();
    const std::int64_t first      = harness.createNode("rollout-version-missing");
    const std::int64_t second     = harness.createNode("rollout-version-mismatch");
    const std::int64_t firmwareId = harness.createFirmware("2.6.0");
    const char        *policy     = R"({"waves":[{"size":2}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":8})";
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "安装版本证据", policy, {first, second});
    harness.tick(base);
    ASSERT_EQ(2U, harness.published.size());

    harness.finishCommandWithoutTelemetry(0, "", later(base, 2));
    harness.finishCommandWithoutTelemetry(1, "1.9.0", later(base, 2));
    harness.insertTelemetryAt(first, later(base, 2), later(base, 2));
    harness.insertTelemetryAt(second, later(base, 2), later(base, 2));
    harness.tick(later(base, 2));
    EXPECT_EQ("running", harness.aggregate(rolloutId).rollout.status);
    // 观察窗口结束（4 秒）仍未拿到版本证据，立即按失败收尾
    harness.tick(later(base, 5));
    const auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("paused", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, first));
    ASSERT_NE(nullptr, targetOf(aggregate, second));
    EXPECT_EQ("failed", targetOf(aggregate, first)->status);
    EXPECT_EQ("failed", targetOf(aggregate, second)->status);
    EXPECT_NE(std::string::npos, targetOf(aggregate, first)->healthJson.find("已安装"));
    EXPECT_NE(std::string::npos, targetOf(aggregate, second)->healthJson.find("不一致"));
}

// 钉住：中止会取消所有未结束命令并把任务与目标收尾，后续 tick 不再派发
TEST(FirmwareRolloutService, AbortCancelsPendingCommandsAndTargets)
{
    RolloutHarness            harness;
    const TimePoint           base = fakeBase();
    std::vector<std::int64_t> nodes;
    for (int index = 0; index < 6; ++index)
    {
        const std::int64_t nodeId = harness.createNode("rollout-abort-" + std::to_string(index));
        harness.heartbeatAt(nodeId, "0.9.0", later(base, 1));
        nodes.push_back(nodeId);
    }
    const std::int64_t firmwareId = harness.createFirmware("2.7.0");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "中止取消", kThreeWavePolicy, nodes);
    harness.tick(base);
    ASSERT_EQ(2U, harness.published.size());
    auto aggregate = harness.aggregate(rolloutId);

    const auto aborted = harness.m_runner->run(harness.m_service->abort(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rolloutId, later(base, 1)));
    ASSERT_TRUE(aborted.has_value()) << (aborted.has_value() ? "" : aborted.error().message);
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("aborted", aggregate.rollout.status);
    for (const EdgePulse::Firmware::FirmwareRolloutWaveRecord &wave: aggregate.waves)
    {
        EXPECT_EQ("skipped", wave.status);
    }
    for (const EdgePulse::Firmware::FirmwareRolloutTargetRecord &target: aggregate.targets)
    {
        EXPECT_EQ("failed", target.status);
    }
    for (const EdgePulse::Command::CommandRecord &command: harness.published)
    {
        EXPECT_EQ("canceled", harness.commandStatus(command.id));
    }
    for (const EdgePulse::Firmware::FirmwareRolloutTargetRecord &target: aggregate.targets)
    {
        if (target.taskId > 0)
        {
            const auto task = harness.task(target.taskId);
            ASSERT_TRUE(task.has_value());
            EXPECT_EQ("failed", task->status);
        }
    }

    harness.tick(later(base, 5));
    EXPECT_EQ(2U, harness.published.size());
}

// 钉住：提升跳过的目标同样取消未结束命令，后续波次照常派发
TEST(FirmwareRolloutService, PromoteCancelsSkippedTargets)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t first  = harness.createNode("rollout-promote-skip-a");
    const std::int64_t second = harness.createNode("rollout-promote-skip-b");
    harness.heartbeatAt(first, "0.9.0", later(base, 1));
    harness.heartbeatAt(second, "0.9.0", later(base, 1));
    const std::int64_t firmwareId = harness.createFirmware("2.7.1");
    const char        *policy     = R"({"waves":[{"size":1},{"size":1}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30})";
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "提升跳过", policy, {first, second});
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());
    auto aggregate = harness.aggregate(rolloutId);
    ASSERT_NE(nullptr, targetOf(aggregate, first));
    const std::int64_t firstTaskId = targetOf(aggregate, first)->taskId;

    const auto promoted = harness.m_runner->run(harness.m_service->promote(harness.m_runner->loop(), EdgePulse::System::TenantScope{1, false}, rolloutId, later(base, 1)));
    ASSERT_TRUE(promoted.has_value()) << (promoted.has_value() ? "" : promoted.error().message);
    EXPECT_EQ("canceled", harness.commandStatus(harness.published[0].id));
    const auto task = harness.task(firstTaskId);
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ("failed", task->status);
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("failed", targetOf(aggregate, first)->status);
    EXPECT_EQ("passed", aggregate.waves[0].status);
    EXPECT_EQ(1, aggregate.rollout.currentWave);

    harness.tick(later(base, 2));
    EXPECT_EQ(2U, harness.published.size());
    EXPECT_EQ(second, harness.published[1].nodeId);
}

// 钉住：派发过程中人工暂停后不再继续给剩余目标下发
TEST(FirmwareRolloutService, PauseDuringDispatchStopsRemainingTargets)
{
    RolloutHarness            harness;
    const TimePoint           base = fakeBase();
    std::vector<std::int64_t> nodes;
    for (int index = 0; index < 4; ++index)
    {
        const std::int64_t nodeId = harness.createNode("rollout-mid-dispatch-" + std::to_string(index));
        harness.heartbeatAt(nodeId, "0.9.0", later(base, 1));
        nodes.push_back(nodeId);
    }
    const std::int64_t firmwareId = harness.createFirmware("2.7.2");
    const char        *policy     = R"({"waves":[{"size":4}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30})";
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "派发中暂停", policy, nodes);
    harness.afterPublish          = [&harness, rolloutId] { harness.pauseRolloutDirect(rolloutId); };

    harness.tick(base);
    EXPECT_EQ(1U, harness.published.size());
    const auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("paused", aggregate.rollout.status);
    EXPECT_EQ("dispatching", aggregate.waves[0].status);
    ASSERT_NE(nullptr, targetOf(aggregate, nodes[0]));
    EXPECT_EQ("installing", targetOf(aggregate, nodes[0])->status);
    EXPECT_EQ("canceled", harness.commandStatus(harness.published[0].id)) << "暂停后必须取消刚发出的命令";
    const auto pausedTask = harness.task(targetOf(aggregate, nodes[0])->taskId);
    ASSERT_TRUE(pausedTask.has_value());
    EXPECT_EQ("failed", pausedTask->status);
    ASSERT_NE(nullptr, targetOf(aggregate, nodes[1]));
    EXPECT_EQ("pending", targetOf(aggregate, nodes[1])->status);
}

// 钉住：排队期间节点版本变化时，派发前重新解析上一固件并写回目标与任务
TEST(FirmwareRolloutService, ResolvesPreviousFirmwareAtDispatch)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-previous-refresh");
    harness.heartbeatAt(nodeId, "1.0.0", later(base, 1));
    const std::int64_t previous  = harness.createFirmware("1.0.0");
    const std::int64_t target    = harness.createFirmware("2.8.0");
    const char        *policy    = R"({"waves":[{"size":1}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30})";
    const std::int64_t rolloutId = harness.createRollout(target, "派发前刷新", policy, {nodeId});
    EXPECT_EQ(previous, harness.aggregate(rolloutId).targets[0].previousFirmwareId);

    const std::int64_t newer = harness.createFirmware("1.1.0");
    harness.heartbeatAt(nodeId, "1.1.0", later(base, 2));
    harness.tick(base);

    const auto aggregate = harness.aggregate(rolloutId);
    ASSERT_EQ(1U, aggregate.targets.size());
    EXPECT_EQ(newer, aggregate.targets[0].previousFirmwareId);
    const auto task = harness.task(aggregate.targets[0].taskId);
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(newer, task->previousFirmwareId);
}

// 钉住：回滚命令失败落入 failed，而不是 rollback_unavailable
TEST(FirmwareRolloutService, RollbackFailureIsFailedNotUnavailable)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-rollback-fail");
    harness.heartbeatAt(nodeId, "1.0.0", later(base, 1));
    const std::int64_t previous  = harness.createFirmware("1.0.0");
    const std::int64_t target    = harness.createFirmware("2.8.1");
    const char        *policy    = R"({"waves":[{"size":1}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30,"autoRollback":true})";
    const std::int64_t rolloutId = harness.createRollout(target, "回滚失败", policy, {nodeId});
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());
    harness.completeCommand(0, "2.8.1", later(base, 2), false);
    harness.tick(later(base, 2));
    EXPECT_EQ("rolling_back", harness.aggregate(rolloutId).rollout.status);

    harness.tick(later(base, 3));
    ASSERT_EQ(2U, harness.published.size());
    EXPECT_EQ(previous, firmwareIdOf(harness.published[1]));
    harness.completeCommand(1, "1.0.0", later(base, 4), false);
    harness.tick(later(base, 5));

    const auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("failed", aggregate.rollout.status);
    EXPECT_NE(std::string::npos, aggregate.rollout.lastError.find("回滚失败"));
    ASSERT_EQ(1U, aggregate.targets.size());
    EXPECT_EQ("failed", aggregate.targets[0].status);
}

// 钉住：部分目标缺少历史版本时整体落入 failed，保留已完成回滚目标的观察状态
TEST(FirmwareRolloutService, PartialRollbackMarksFailed)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t first  = harness.createNode("rollout-partial-rollback-a");
    const std::int64_t second = harness.createNode("rollout-partial-rollback-b");
    harness.heartbeatAt(first, "1.0.0", later(base, 1));
    harness.heartbeatAt(second, "0.9.0", later(base, 1));
    const std::int64_t previous  = harness.createFirmware("1.0.0");
    const std::int64_t target    = harness.createFirmware("2.8.2");
    const char        *policy    = R"({"waves":[{"size":2}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30,"autoRollback":true})";
    const std::int64_t rolloutId = harness.createRollout(target, "部分回滚", policy, {first, second});
    auto               aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ(previous, targetOf(aggregate, first)->previousFirmwareId);
    EXPECT_EQ(0, targetOf(aggregate, second)->previousFirmwareId);

    harness.tick(base);
    ASSERT_EQ(2U, harness.published.size());
    harness.completeCommand(0, "2.8.2", later(base, 2), false);
    harness.completeCommand(1, "2.8.2", later(base, 2), false);
    harness.tick(later(base, 2));
    EXPECT_EQ("rolling_back", harness.aggregate(rolloutId).rollout.status);

    harness.tick(later(base, 3));
    aggregate = harness.aggregate(rolloutId);
    // 第二台没有历史版本，但第一台仍在回滚中：不能提前落 failed
    EXPECT_EQ("rolling_back", aggregate.rollout.status);
    EXPECT_EQ("rolling_back", targetOf(aggregate, first)->status);

    // 回滚期限到达后先把所有未决目标判失败，再写发布头
    harness.tick(later(base, 33));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("failed", aggregate.rollout.status);
    EXPECT_NE(std::string::npos, aggregate.rollout.lastError.find("回滚"));
    EXPECT_EQ(3U, harness.published.size());
    EXPECT_EQ("failed", targetOf(aggregate, first)->status);
}

// 钉住：两波金丝雀第二波失败时，第一波已通过目标也一起回滚，整单落到 rolled_back
TEST(FirmwareRolloutService, TwoWaveCanaryRollbackCoversPassedWave)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t first  = harness.createNode("rollout-two-wave-a");
    const std::int64_t second = harness.createNode("rollout-two-wave-b");
    harness.heartbeatAt(first, "0.9.0", later(base, 1));
    harness.heartbeatAt(second, "0.9.0", later(base, 1));
    const std::int64_t previous = harness.createFirmware("0.9.0");
    const std::int64_t target   = harness.createFirmware("3.0.1");
    const char        *policy   = R"({"waves":[{"size":1},{"size":1}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":30,"autoRollback":true})";
    const std::int64_t rolloutId = harness.createRollout(target, "两波回滚", policy, {first, second});
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());
    harness.completeCommand(0, "3.0.1", later(base, 6));
    harness.tick(later(base, 6));
    ASSERT_EQ(2U, harness.published.size());
    EXPECT_EQ(second, harness.published[1].nodeId);
    EXPECT_EQ("passed", harness.aggregate(rolloutId).waves[0].status);

    // 第二波命令迟迟不完成，观察结束时判失败并自动回滚；第一波不能停留在新固件
    harness.tick(later(base, 10));
    EXPECT_EQ("rolling_back", harness.aggregate(rolloutId).rollout.status);
    harness.tick(later(base, 11));
    ASSERT_EQ(4U, harness.published.size());
    EXPECT_EQ(previous, firmwareIdOf(harness.published[2]));
    EXPECT_EQ(previous, firmwareIdOf(harness.published[3]));

    harness.completeCommand(2, "0.9.0", later(base, 12));
    harness.completeCommand(3, "0.9.0", later(base, 12));
    harness.tick(later(base, 12));
    const auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("rolled_back", aggregate.rollout.status);
    EXPECT_EQ("rolled_back", aggregate.waves[0].status);
    EXPECT_EQ("rolled_back", aggregate.waves[1].status);
    EXPECT_EQ("rolled_back", targetOf(aggregate, first)->status);
    EXPECT_EQ("rolled_back", targetOf(aggregate, second)->status);
}

// 钉住：两波金丝雀在第二波失败时整单回滚，已通过波次一并回滚，后一波未派发目标落 skipped
TEST(FirmwareRolloutService, CanaryRollbackCoversWholeRolloutAndSkipsNeverDispatched)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t first  = harness.createNode("rollout-canary-a");
    const std::int64_t second = harness.createNode("rollout-canary-b");
    const std::int64_t third  = harness.createNode("rollout-canary-c");
    const std::int64_t fourth = harness.createNode("rollout-canary-d");
    for (const std::int64_t nodeId: {first, second, third, fourth})
    {
        harness.heartbeatAt(nodeId, "0.9.0", later(base, 1));
    }
    const std::int64_t previous = harness.createFirmware("0.9.0");
    const std::int64_t target   = harness.createFirmware("3.0.0");
    const char        *policy   = R"({"waves":[{"size":1},{"size":2},{"size":1}],"settleSeconds":2,"observeSeconds":4,
        "maxWaveSeconds":30,"autoRollback":true})";
    const std::int64_t rolloutId = harness.createRollout(target, "两波金丝雀", policy, {first, second, third, fourth});
    EXPECT_EQ(previous, targetOf(harness.aggregate(rolloutId), first)->previousFirmwareId);

    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());
    EXPECT_EQ(first, harness.published[0].nodeId);
    harness.completeCommand(0, "3.0.0", later(base, 6));
    harness.tick(later(base, 6));
    ASSERT_EQ(3U, harness.published.size());
    EXPECT_EQ(second, harness.published[1].nodeId);
    EXPECT_EQ(third, harness.published[2].nodeId);
    auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("passed", aggregate.waves[0].status);
    EXPECT_EQ("observing", aggregate.waves[1].status);
    EXPECT_EQ("pending", targetOf(aggregate, fourth)->status);

    // 第二波安装成功但出现新告警：整单进入回滚，第三波从未派发
    harness.completeCommand(1, "3.0.0", later(base, 10));
    harness.completeCommand(2, "3.0.0", later(base, 10));
    harness.raiseAlarm(second, "cpu.high", 301);
    harness.tick(later(base, 10));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("rolling_back", aggregate.rollout.status);
    EXPECT_EQ("failed", aggregate.waves[1].status);

    harness.tick(later(base, 11));
    ASSERT_EQ(6U, harness.published.size());
    for (std::size_t index = 3; index < 6; ++index)
    {
        EXPECT_EQ(previous, firmwareIdOf(harness.published[index]));
    }
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("rolling_back", aggregate.waves[0].status) << "已通过波次也必须回滚";
    EXPECT_EQ("rolling_back", aggregate.waves[1].status);
    EXPECT_EQ("rolling_back", targetOf(aggregate, first)->status);
    EXPECT_EQ("rolling_back", targetOf(aggregate, second)->status);
    EXPECT_EQ("rolling_back", targetOf(aggregate, third)->status);
    EXPECT_EQ("skipped", targetOf(aggregate, fourth)->status);

    harness.completeCommand(3, "0.9.0", later(base, 12));
    harness.completeCommand(4, "0.9.0", later(base, 12));
    harness.completeCommand(5, "0.9.0", later(base, 12));
    harness.tick(later(base, 12));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("rolled_back", aggregate.rollout.status);
    EXPECT_EQ("rolled_back", aggregate.waves[0].status);
    EXPECT_EQ("rolled_back", aggregate.waves[1].status);
    EXPECT_EQ("skipped", aggregate.waves[2].status);
    EXPECT_EQ("rolled_back", targetOf(aggregate, first)->status);
    EXPECT_EQ("rolled_back", targetOf(aggregate, second)->status);
    EXPECT_EQ("rolled_back", targetOf(aggregate, third)->status);
    EXPECT_EQ("skipped", targetOf(aggregate, fourth)->status);
}

// 钉住：回滚命令迟迟不确认时按回滚期限失败，不直接标记 rolled_back
TEST(FirmwareRolloutService, RollbackDeadlineFailsWithoutConfirmation)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-rollback-deadline");
    harness.heartbeatAt(nodeId, "1.0.0", later(base, 1));
    const std::int64_t previous  = harness.createFirmware("1.0.0");
    const std::int64_t target    = harness.createFirmware("2.8.3");
    const char        *policy    = R"({"waves":[{"size":1}],"settleSeconds":2,"observeSeconds":4,"maxWaveSeconds":10,"autoRollback":true})";
    const std::int64_t rolloutId = harness.createRollout(target, "回滚超时", policy, {nodeId});
    harness.tick(base);
    harness.completeCommand(0, "2.8.3", later(base, 2), false);
    harness.tick(later(base, 2));
    harness.tick(later(base, 3));
    ASSERT_EQ(2U, harness.published.size());
    EXPECT_EQ(previous, firmwareIdOf(harness.published[1]));
    EXPECT_EQ("rolling_back", harness.aggregate(rolloutId).rollout.status);

    harness.tick(later(base, 14));
    const auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("failed", aggregate.rollout.status);
    EXPECT_NE(std::string::npos, aggregate.rollout.lastError.find("期限"));
    EXPECT_EQ(2U, harness.published.size());
    ASSERT_EQ(1U, aggregate.targets.size());
    // 期限到达仍未确认的目标必须留下 rollback_failed 终局，不能悬在 rolling_back
    EXPECT_EQ("failed", aggregate.targets[0].status);
    EXPECT_NE(std::string::npos, aggregate.targets[0].healthJson.find("rollback_failed"));
}

// 钉住：命令发布失败时目标失败、命令取消、任务失败，不把发布卡在派发中
TEST(FirmwareRolloutService, PublishFailureFailsTarget)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-publish-fail");
    harness.heartbeatAt(nodeId, "0.9.0", later(base, 1));
    const std::int64_t firmwareId = harness.createFirmware("2.8.4");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "发布失败", kSinglePolicy, {nodeId});
    harness.failPublishes         = 1;
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());
    EXPECT_EQ("canceled", harness.commandStatus(harness.published[0].id));
    auto aggregate = harness.aggregate(rolloutId);
    ASSERT_NE(nullptr, targetOf(aggregate, nodeId));
    EXPECT_EQ("failed", targetOf(aggregate, nodeId)->status);
    const auto tasks = harness.m_runner->run(harness.m_firmware->tasks(harness.m_runner->loop(), firmwareId));
    ASSERT_EQ(1U, tasks.size());
    EXPECT_EQ("failed", tasks[0].status);

    harness.tick(later(base, 2));
    EXPECT_EQ("paused", harness.aggregate(rolloutId).rollout.status);
}

// 钉住：命令已绑定、目标未回写的重启窗口里复用 installing 任务，不重复建单或重发
TEST(FirmwareRolloutService, ReusesInstallingTaskWhenTargetWriteWasLost)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-idempotent-reuse");
    harness.heartbeatAt(nodeId, "0.9.0", later(base, 1));
    static_cast<void>(harness.createFirmware("0.9.0"));
    const std::int64_t firmwareId = harness.createFirmware("3.1.0");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "安装中重启", kSinglePolicy, {nodeId});
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());
    const auto         first        = harness.aggregate(rolloutId);
    const auto        *firstTarget  = targetOf(first, nodeId);
    ASSERT_NE(nullptr, firstTarget);
    const std::int64_t taskId    = firstTarget->taskId;
    const std::int64_t commandId = firstTarget->commandId;
    ASSERT_GT(taskId, 0);
    ASSERT_GT(commandId, 0);

    // 模拟 Agent 已进入安装、进程在目标回写前重启：任务为 installing，目标仍是待派发
    ASSERT_TRUE(harness.m_runner->run(harness.m_firmware->updateTaskById(harness.m_runner->loop(), taskId, "installing", 40, "安装中")));
    harness.executeSql("UPDATE biz_firmware_rollout_target SET status = 'pending', task_id = 0, command_id = 0, health_json = '{}' WHERE rollout_id = " +
                       std::to_string(rolloutId) + " AND node_id = " + std::to_string(nodeId));

    harness.tick(later(base, 1));
    EXPECT_EQ(1U, harness.published.size()) << "已有 installing 任务时不能再发布第二条命令";
    const auto repaired = harness.aggregate(rolloutId);
    const auto *repairedTarget = targetOf(repaired, nodeId);
    ASSERT_NE(nullptr, repairedTarget);
    EXPECT_EQ("installing", repairedTarget->status);
    EXPECT_EQ(taskId, repairedTarget->taskId);
    EXPECT_EQ(commandId, repairedTarget->commandId);
    const auto tasks = harness.m_runner->run(harness.m_firmware->tasks(harness.m_runner->loop(), firmwareId));
    EXPECT_EQ(1U, tasks.size());
}

// 钉住：Agent 已完成安装但目标回写丢失时，下一拍复用 installed 任务，不再建单或重发
TEST(FirmwareRolloutService, ReusesInstalledTaskWhenTargetWriteWasLost)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-idempotent-installed");
    harness.heartbeatAt(nodeId, "0.9.0", later(base, 1));
    static_cast<void>(harness.createFirmware("0.9.0"));
    const std::int64_t firmwareId = harness.createFirmware("3.1.1");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "安装完成回写丢失", kSinglePolicy, {nodeId});
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());
    const auto         first       = harness.aggregate(rolloutId);
    const auto        *firstTarget = targetOf(first, nodeId);
    ASSERT_NE(nullptr, firstTarget);
    const std::int64_t taskId    = firstTarget->taskId;
    const std::int64_t commandId = firstTarget->commandId;
    harness.completeCommand(0, "3.1.1", later(base, 2));

    harness.executeSql("UPDATE biz_firmware_rollout_target SET status = 'pending', task_id = 0, command_id = 0, health_json = '{}' WHERE rollout_id = " +
                       std::to_string(rolloutId) + " AND node_id = " + std::to_string(nodeId));
    harness.tick(later(base, 3));
    EXPECT_EQ(1U, harness.published.size()) << "已安装任务不能被重新下发";
    auto aggregate = harness.aggregate(rolloutId);
    ASSERT_NE(nullptr, targetOf(aggregate, nodeId));
    EXPECT_EQ(taskId, targetOf(aggregate, nodeId)->taskId);
    EXPECT_EQ(commandId, targetOf(aggregate, nodeId)->commandId);
    EXPECT_EQ(1U, harness.m_runner->run(harness.m_firmware->tasks(harness.m_runner->loop(), firmwareId)).size());

    harness.tick(later(base, 4));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("completed", aggregate.rollout.status);
    EXPECT_EQ("healthy", targetOf(aggregate, nodeId)->status);
}

// 钉住：绑定任务与命令失败时取消命令、任务与目标一起收尾
TEST(FirmwareRolloutService, BindFailureFailsTarget)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-bind-fail");
    harness.heartbeatAt(nodeId, "0.9.0", later(base, 1));
    const std::int64_t firmwareId = harness.createFirmware("2.8.5");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "绑定失败", kSinglePolicy, {nodeId});
    harness.failTaskBinds();
    harness.tick(base);
    EXPECT_TRUE(harness.published.empty());

    const auto commands = harness.m_runner->run(harness.m_commands->list(harness.m_runner->loop(), "", nodeId, EdgePulse::Common::PageParameters{1, 10}));
    ASSERT_EQ(1, commands.total);
    EXPECT_EQ("canceled", commands.rows[0].status);
    const auto tasks = harness.m_runner->run(harness.m_firmware->tasks(harness.m_runner->loop(), firmwareId));
    ASSERT_EQ(1U, tasks.size());
    EXPECT_EQ("failed", tasks[0].status);
    const auto aggregate = harness.aggregate(rolloutId);
    ASSERT_NE(nullptr, targetOf(aggregate, nodeId));
    EXPECT_EQ("failed", targetOf(aggregate, nodeId)->status);

    harness.tick(later(base, 2));
    EXPECT_EQ("paused", harness.aggregate(rolloutId).rollout.status);
}

// 钉住：安装完成后节点掉线立即判失败
TEST(FirmwareRolloutService, OfflineDuringInstallFailsGate)
{
    RolloutHarness     harness;
    const TimePoint    base       = fakeBase();
    const std::int64_t nodeId     = harness.createNode("rollout-offline-install");
    const std::int64_t firmwareId = harness.createFirmware("2.8.6");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "安装后离线", kSinglePolicy, {nodeId});
    harness.tick(base);
    harness.finishCommandWithoutTelemetry(0, "2.8.6", later(base, 2));
    harness.insertTelemetryAt(nodeId, later(base, 2), later(base, 2));
    harness.setNodeOffline(nodeId);
    harness.tick(later(base, 2));

    const auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("paused", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, nodeId));
    EXPECT_EQ("failed", targetOf(aggregate, nodeId)->status);
    EXPECT_NE(std::string::npos, targetOf(aggregate, nodeId)->healthJson.find("离线"));
}

// 钉住：安装尚未确认也需要评估节点状态；离线目标在观察窗口结束判失败，不再悬在 installing
TEST(FirmwareRolloutService, GateFailsOfflineBeforeInstallAtObserveEnd)
{
    RolloutHarness     harness;
    const TimePoint    base       = fakeBase();
    const std::int64_t nodeId     = harness.createNode("rollout-offline-before-install");
    const std::int64_t firmwareId = harness.createFirmware("3.2.0");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "安装前离线", kSinglePolicy, {nodeId});
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());

    harness.tick(later(base, 2));
    auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("running", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, nodeId));
    EXPECT_EQ("installing", targetOf(aggregate, nodeId)->status);
    EXPECT_NE(std::string::npos, targetOf(aggregate, nodeId)->healthJson.find("\"nodeOnline\":false")) << targetOf(aggregate, nodeId)->healthJson;

    harness.tick(later(base, 5));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("paused", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, nodeId));
    EXPECT_EQ("failed", targetOf(aggregate, nodeId)->status);
    EXPECT_NE(std::string::npos, targetOf(aggregate, nodeId)->healthJson.find("节点离线"));
}

// 钉住：节点在安装前被删除时门控立即给出失败，不等待命令或任务完成
TEST(FirmwareRolloutService, GateFailsDeletedNodeAfterSettle)
{
    RolloutHarness     harness;
    const TimePoint    base       = fakeBase();
    const std::int64_t nodeId     = harness.createNode("rollout-deleted-before-install");
    harness.heartbeatAt(nodeId, "0.9.0", later(base, 1));
    const std::int64_t firmwareId = harness.createFirmware("3.2.1");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "安装前删除", kSinglePolicy, {nodeId});
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());
    harness.executeSql("UPDATE biz_node SET deleted = 1 WHERE id = " + std::to_string(nodeId));

    harness.tick(later(base, 2));
    const auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("paused", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, nodeId));
    EXPECT_EQ("failed", targetOf(aggregate, nodeId)->status);
    EXPECT_NE(std::string::npos, targetOf(aggregate, nodeId)->healthJson.find("节点不存在"));
}

// 钉住：沉降窗口之前的遥测不能证明升级后健康，窗口结束后仍只有旧样本就判失败
TEST(FirmwareRolloutService, GateRejectsTelemetryCreatedBeforeSettle)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-telemetry-pre-settle");
    harness.heartbeatAt(nodeId, "0.9.0", later(base, 1));
    const std::int64_t firmwareId = harness.createFirmware("3.2.2");
    const char        *policy     = R"({"waves":[{"size":1}],"settleSeconds":2,"observeSeconds":6,"maxWaveSeconds":30})";
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "沉降前遥测", policy, {nodeId});
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());
    harness.finishCommandWithoutTelemetry(0, "3.2.2", later(base, 2));
    harness.insertTelemetryAt(nodeId, later(base, 1), later(base, 1));

    harness.tick(later(base, 2));
    auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("running", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, nodeId));
    EXPECT_EQ("installing", targetOf(aggregate, nodeId)->status);
    EXPECT_NE(std::string::npos, targetOf(aggregate, nodeId)->healthJson.find("沉降窗口"));

    harness.tick(later(base, 7));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("paused", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, nodeId));
    EXPECT_EQ("failed", targetOf(aggregate, nodeId)->status);
    EXPECT_NE(std::string::npos, targetOf(aggregate, nodeId)->healthJson.find("沉降窗口"));
}

// 钉住：接收与采集时间必须来自同一条遥测；旧样本的迟到接收时间不能借新样本的采集时间过关
TEST(FirmwareRolloutService, GateUsesSingleTelemetrySampleForBothTimestamps)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t nodeId = harness.createNode("rollout-telemetry-skew");
    harness.heartbeatAt(nodeId, "0.9.0", later(base, 1));
    const std::int64_t firmwareId = harness.createFirmware("3.2.3");
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "遥测时钟偏差", kSinglePolicy, {nodeId});
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());
    harness.finishCommandWithoutTelemetry(0, "3.2.3", later(base, 2));

    // 采样 A 的接收时间落在沉降窗口后，但采集时间比派发还早；采样 B 的采集时间新、接收时间旧
    harness.insertTelemetryAt(nodeId, later(base, -400), later(base, 2));
    harness.insertTelemetryAt(nodeId, later(base, 2), later(base, 1));

    harness.tick(later(base, 2));
    auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("running", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, nodeId));
    EXPECT_EQ("installing", targetOf(aggregate, nodeId)->status);
    EXPECT_NE(std::string::npos, targetOf(aggregate, nodeId)->healthJson.find("采集时间"));

    harness.tick(later(base, 5));
    aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("paused", aggregate.rollout.status);
    ASSERT_NE(nullptr, targetOf(aggregate, nodeId));
    EXPECT_EQ("failed", targetOf(aggregate, nodeId)->status);
    EXPECT_NE(std::string::npos, targetOf(aggregate, nodeId)->healthJson.find("采集时间"));
}

// 钉住：maxFailures 超出上限时即使成功率阈值放行也判失败
TEST(FirmwareRolloutService, MaxFailuresRejectsWave)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t first  = harness.createNode("rollout-maxfail-a");
    const std::int64_t second = harness.createNode("rollout-maxfail-b");
    const std::int64_t third  = harness.createNode("rollout-maxfail-c");
    harness.heartbeatAt(first, "0.9.0", later(base, 1));
    harness.heartbeatAt(second, "0.9.0", later(base, 1));
    harness.heartbeatAt(third, "0.9.0", later(base, 1));
    const std::int64_t firmwareId = harness.createFirmware("2.8.7");
    const char        *policy     = R"({"waves":[{"size":3}],"settleSeconds":2,"observeSeconds":2,"maxWaveSeconds":30,"maxFailures":1,
        "minWaveSuccessRatio":0,"minCumulativeSuccessRatio":0})";
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "失败上限", policy, {first, second, third});
    harness.tick(base);
    ASSERT_EQ(3U, harness.published.size());
    harness.completeCommand(0, "2.8.7", later(base, 2));
    harness.completeCommand(1, "2.8.7", later(base, 2), false);
    harness.completeCommand(2, "2.8.7", later(base, 2), false);
    harness.tick(later(base, 2));

    const auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("paused", aggregate.rollout.status);
    EXPECT_EQ("failed", aggregate.waves[0].status);
}

// 钉住：累计成功率跨波次计算，前一波成功不能抵消后一波失败
TEST(FirmwareRolloutService, CumulativeRatioRejectsLaterWaveFailure)
{
    RolloutHarness     harness;
    const TimePoint    base   = fakeBase();
    const std::int64_t first  = harness.createNode("rollout-cumulative-a");
    const std::int64_t second = harness.createNode("rollout-cumulative-b");
    harness.heartbeatAt(first, "0.9.0", later(base, 1));
    harness.heartbeatAt(second, "0.9.0", later(base, 1));
    const std::int64_t firmwareId = harness.createFirmware("2.8.8");
    const char        *policy     = R"({"waves":[{"size":1},{"size":1}],"settleSeconds":2,"observeSeconds":2,"maxWaveSeconds":30,
        "minWaveSuccessRatio":0,"minCumulativeSuccessRatio":1,"maxFailures":10})";
    const std::int64_t rolloutId  = harness.createRollout(firmwareId, "累计成功率", policy, {first, second});
    harness.tick(base);
    ASSERT_EQ(1U, harness.published.size());
    harness.completeCommand(0, "2.8.8", later(base, 2));
    harness.tick(later(base, 2));
    ASSERT_EQ(2U, harness.published.size());
    EXPECT_EQ(1, harness.aggregate(rolloutId).rollout.currentWave);

    harness.completeCommand(1, "2.8.8", later(base, 4), false);
    harness.tick(later(base, 4));
    const auto aggregate = harness.aggregate(rolloutId);
    EXPECT_EQ("paused", aggregate.rollout.status);
    EXPECT_EQ("failed", aggregate.waves[1].status);
}
