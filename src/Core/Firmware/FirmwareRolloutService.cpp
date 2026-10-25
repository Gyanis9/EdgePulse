#include "Core/Firmware/FirmwareRolloutService.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Exception/EdgePulseException.h"
#include "Base/Log/LogMacros.h"
#include "Base/Runtime/TimeFormat.h"
#include "Core/Alarm/AlarmRepository.h"
#include "Core/Command/CommandRepository.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Firmware/FirmwareRepository.h"
#include "Core/Firmware/FirmwareSigningPolicy.h"
#include "Core/Telemetry/TelemetryRepository.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace EdgePulse::Firmware
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;
        using Clock     = std::chrono::system_clock;
        using TimePoint = Clock::time_point;

        constexpr int kTelemetryClockSkewSeconds = 300;

        [[nodiscard]] TimePoint addSeconds(const TimePoint point, const int seconds)
        {
            return point + std::chrono::seconds{seconds};
        }

        [[nodiscard]] std::tm utcTime(const TimePoint point)
        {
            const std::time_t seconds = Clock::to_time_t(point);
            std::tm           value{};
#ifdef _WIN32
            gmtime_s(&value, &seconds);
#else
            gmtime_r(&seconds, &value);
#endif
            return value;
        }

        [[nodiscard]] std::optional<TimePoint> parseSqlTime(const std::string &text)
        {
            // 数据库时间统一为 YYYY-MM-DD HH:MM:SS，按定长逐位解析，避免 sscanf 的跨平台告警
            if (text.size() != 19 || text[4] != '-' || text[7] != '-' || text[10] != ' ' || text[13] != ':' || text[16] != ':')
            {
                return std::nullopt;
            }
            const auto digit = [](const char character) -> std::optional<int>
            {
                if (character < '0' || character > '9')
                {
                    return std::nullopt;
                }
                return character - '0';
            };
            const auto number = [&digit, &text](const std::size_t offset, const std::size_t length) -> std::optional<int>
            {
                int value = 0;
                for (std::size_t index = 0; index < length; ++index)
                {
                    const std::optional<int> current = digit(text[offset + index]);
                    if (!current.has_value())
                    {
                        return std::nullopt;
                    }
                    value = value * 10 + *current;
                }
                return value;
            };
            const std::optional<int> year   = number(0, 4);
            const std::optional<int> month  = number(5, 2);
            const std::optional<int> day    = number(8, 2);
            const std::optional<int> hour   = number(11, 2);
            const std::optional<int> minute = number(14, 2);
            const std::optional<int> second = number(17, 2);
            if (!year.has_value() || !month.has_value() || !day.has_value() || !hour.has_value() || !minute.has_value() || !second.has_value())
            {
                return std::nullopt;
            }
            if (*year < 1970 || *month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59 || *second > 60)
            {
                return std::nullopt;
            }
            std::tm value{};
            value.tm_year = *year - 1900;
            value.tm_mon  = *month - 1;
            value.tm_mday = *day;
            value.tm_hour = *hour;
            value.tm_min  = *minute;
            value.tm_sec  = *second;
#ifdef _WIN32
            const std::time_t epoch = _mkgmtime(&value);
#else
            const std::time_t epoch = timegm(&value);
#endif
            if (epoch < 0)
            {
                return std::nullopt;
            }
            return Clock::from_time_t(epoch);
        }

        [[nodiscard]] std::string sqlTime(const TimePoint point)
        {
            return Common::formatDateTimeUtc(point);
        }

        [[nodiscard]] bool signatureRequired(const std::shared_ptr<FirmwareSigningPolicy> &policy)
        {
            return policy != nullptr && policy->signatureRequired();
        }

        [[nodiscard]] std::string downloadPayload(const FirmwareRecord &record, const std::int64_t nodeId)
        {
            return ConfigValue::object({{"firmwareId", record.id},
                                        {"name", record.name},
                                        {"version", record.version},
                                        {"sha256", record.sha256},
                                        {"sizeBytes", record.sizeBytes},
                                        {"signature", record.signature},
                                        {"signingKeyId", record.signingKeyId},
                                        {"signatureAlgorithm", record.signatureAlgorithm},
                                        {"downloadPath", "/api/agent/firmware/" + std::to_string(record.id) + "/download?nodeId=" + std::to_string(nodeId)}})
                    .dump();
        }

        [[nodiscard]] std::string joinIdentifiers(const std::vector<std::int64_t> &identifiers)
        {
            std::string joined;
            for (const std::int64_t identifier: identifiers)
            {
                if (!joined.empty())
                {
                    joined += "、";
                }
                joined += std::to_string(identifier);
            }
            return joined;
        }

        [[nodiscard]] std::string joinReasons(const std::vector<std::string> &reasons, const std::size_t maximumBytes)
        {
            std::string joined;
            for (const std::string &reason: reasons)
            {
                if (!joined.empty())
                {
                    joined += "；";
                }
                joined += reason;
                if (joined.size() >= maximumBytes)
                {
                    break;
                }
            }
            return joined;
        }

        /**
         * @brief 按字节上限截断 UTF-8 文本，不切断多字节字符
         */
        [[nodiscard]] std::string truncateUtf8(const std::string &text, const std::size_t maximumBytes)
        {
            if (text.size() <= maximumBytes)
            {
                return text;
            }
            std::size_t cut = maximumBytes;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U)
            {
                --cut;
            }
            return text.substr(0, cut);
        }

        [[nodiscard]] std::string trimCopy(const std::string_view text)
        {
            const auto begin = std::find_if_not(text.begin(), text.end(), [](const unsigned char character) { return std::isspace(character) != 0; });
            const auto end   = std::find_if_not(text.rbegin(), text.rend(), [](const unsigned char character) { return std::isspace(character) != 0; }).base();
            return begin < end ? std::string{begin, end} : std::string{};
        }

        [[nodiscard]] bool commandActive(const std::string &status)
        {
            return status == "pending" || status == "running";
        }

        [[nodiscard]] bool taskActive(const std::string &status)
        {
            return status == "pending" || status == "downloading" || status == "installing";
        }

        [[nodiscard]] std::string healthField(const std::string &json, const std::string &key)
        {
            const std::optional<ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(json);
            if (!parsed.has_value() || !parsed->is_object() || !parsed->contains(key))
            {
                return {};
            }
            const std::optional<std::string> value = AsynGyanis::Base::configValueAs<std::string>(parsed->at(key));
            return value.value_or(std::string{});
        }

        /**
         * @brief 回滚流程是否为该目标留下过决定
         * @note 目标状态 rolling_back 带着命令与任务表示已派发；rolled_back 表示已确认；
         *       failed 且门控为 rollback_unavailable 或 rollback_failed 表示派发阶段已给出终局结论
         */
        [[nodiscard]] bool rollbackDecided(const FirmwareRolloutTargetRecord &target)
        {
            if (target.status == "rolling_back")
            {
                return target.commandId > 0 && target.taskId > 0;
            }
            if (target.status == "rolled_back" || target.status == "skipped")
            {
                return true;
            }
            if (target.status != "failed")
            {
                return false;
            }
            const std::string gate = healthField(target.healthJson, "gate");
            return gate == "rollback_unavailable" || gate == "rollback_failed";
        }

        [[nodiscard]] bool commandTargetsFirmware(const std::string &payload, const std::int64_t firmwareId)
        {
            const std::optional<ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(payload);
            if (!parsed.has_value() || !parsed->is_object() || !parsed->contains("firmwareId"))
            {
                return false;
            }
            return AsynGyanis::Base::configValueAs<std::int64_t>(parsed->at("firmwareId")).value_or(0) == firmwareId;
        }

        [[nodiscard]] std::string healthIdentity(const std::string &json)
        {
            const std::optional<ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(json);
            if (!parsed.has_value() || !parsed->is_object())
            {
                return {};
            }
            std::string identity;
            if (parsed->contains("gate"))
            {
                const std::optional<std::string> gate = AsynGyanis::Base::configValueAs<std::string>(parsed->at("gate"));
                if (gate.has_value())
                {
                    identity += *gate;
                }
            }
            identity += '\n';
            if (parsed->contains("reason"))
            {
                const std::optional<std::string> reason = AsynGyanis::Base::configValueAs<std::string>(parsed->at("reason"));
                if (reason.has_value())
                {
                    identity += *reason;
                }
            }
            return identity;
        }

        [[nodiscard]] bool isTerminal(const std::string &status)
        {
            return status == "completed" || status == "rolled_back" || status == "rollback_unavailable" || status == "aborted" || status == "failed";
        }

        [[nodiscard]] bool containsStatus(const std::vector<std::string> &statuses, const std::string &status)
        {
            return std::find(statuses.begin(), statuses.end(), status) != statuses.end();
        }

        [[nodiscard]] bool isExempt(const FirmwareRolloutPolicy &policy, const std::string &metric)
        {
            return std::find(policy.exemptAlarmMetrics.begin(), policy.exemptAlarmMetrics.end(), metric) != policy.exemptAlarmMetrics.end();
        }

        [[nodiscard]] const FirmwareRolloutWaveRecord *waveAt(const FirmwareRolloutAggregate &aggregate, const std::size_t index)
        {
            for (const FirmwareRolloutWaveRecord &wave: aggregate.waves)
            {
                if (wave.waveIndex == static_cast<int>(index))
                {
                    return &wave;
                }
            }
            return nullptr;
        }

        [[nodiscard]] std::vector<FirmwareRolloutTargetRecord> targetsOf(const FirmwareRolloutAggregate &aggregate, const std::int64_t waveId)
        {
            std::vector<FirmwareRolloutTargetRecord> targets;
            for (const FirmwareRolloutTargetRecord &target: aggregate.targets)
            {
                if (target.waveId == waveId)
                {
                    targets.push_back(target);
                }
            }
            return targets;
        }

        [[nodiscard]] std::string healthText(const std::string &gate, const std::string &reason, const std::int64_t commandId, const std::string &commandStatus,
                                             const std::int64_t taskId, const std::string &taskStatus, const bool nodeOnline, const std::string &lastSeen,
                                             const std::string &installedVersion, const std::string &telemetryCreatedAt, const std::string &telemetryCollectedAt,
                                             const std::string &alarmMetric)
        {
            return ConfigValue::object({{"gate", gate},
                                        {"reason", reason},
                                        {"commandId", commandId},
                                        {"commandStatus", commandStatus},
                                        {"taskId", taskId},
                                        {"taskStatus", taskStatus},
                                        {"nodeOnline", nodeOnline},
                                        {"lastSeen", lastSeen},
                                        {"installedVersion", installedVersion},
                                        {"telemetryCreatedAt", telemetryCreatedAt},
                                        {"telemetryCollectedAt", telemetryCollectedAt},
                                        {"alarmMetric", alarmMetric}})
                    .dump();
        }
    } // namespace

    FirmwareRolloutService::FirmwareRolloutService(FirmwareRolloutDependencies dependencies) :
        m_firmware(std::move(dependencies.firmware)), m_rollouts(std::move(dependencies.rollouts)), m_commands(std::move(dependencies.commands)),
        m_nodes(std::move(dependencies.nodes)), m_telemetry(std::move(dependencies.telemetry)), m_alarms(std::move(dependencies.alarms)),
        m_publisher(std::move(dependencies.publisher)), m_signingPolicy(std::move(dependencies.signingPolicy)),
        m_heartbeatSeconds(std::max(1, dependencies.heartbeatIntervalSeconds)), m_dueLimit(std::max<std::size_t>(1, dependencies.dueLimit)),
        m_commandTimeout(std::max(1, dependencies.commandTimeoutSeconds))
    {
        if (m_firmware == nullptr || m_rollouts == nullptr || m_commands == nullptr || m_nodes == nullptr || m_telemetry == nullptr || m_alarms == nullptr)
        {
            throw std::invalid_argument("灰度服务缺少必需的仓储依赖");
        }
        if (!m_publisher)
        {
            throw std::invalid_argument("灰度服务缺少命令发布口");
        }
        const System::TenantScope allTenants{0, true};
        m_allRollouts  = m_rollouts->scoped(allTenants);
        m_allFirmware  = m_firmware->scoped(allTenants);
        m_allCommands  = m_commands->scoped(allTenants);
        m_allNodes     = m_nodes->scoped(allTenants);
        m_allTelemetry = m_telemetry->scoped(allTenants);
        m_allAlarms    = m_alarms->scoped(allTenants);
    }

    AsynGyanis::Core::Task<FirmwareRolloutResult<std::int64_t>> FirmwareRolloutService::create(AsynGyanis::Core::EventLoop &loop, const System::TenantScope scope,
                                                                                               const std::int64_t actorId, const std::int64_t firmwareId, std::string name,
                                                                                               std::string policyJson, std::vector<std::int64_t> nodeIds)
    {
        if (name.empty())
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::InvalidInput, "发布名称不能为空"});
        }
        if (name.size() > 128)
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::InvalidInput, "发布名称不能超过 128 字节"});
        }
        if (nodeIds.empty() || nodeIds.size() > 1000)
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::InvalidInput, "nodeIds 必须是 1 到 1000 个节点"});
        }
        std::vector<std::int64_t> seen;
        seen.reserve(nodeIds.size());
        for (const std::int64_t nodeId: nodeIds)
        {
            if (nodeId <= 0)
            {
                co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::InvalidInput, "目标节点标识必须为正整数"});
            }
            if (std::find(seen.begin(), seen.end(), nodeId) != seen.end())
            {
                co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::InvalidInput, "目标节点不能重复：" + std::to_string(nodeId)});
            }
            seen.push_back(nodeId);
        }
        const std::expected<FirmwareRolloutPolicy, std::string> parsed = FirmwareRolloutPolicy::parse(policyJson, m_heartbeatSeconds);
        if (!parsed.has_value())
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::InvalidInput, parsed.error()});
        }
        const FirmwareRolloutPolicy &policy = *parsed;

        const auto scopedFirmware = m_firmware->scoped(scope);
        const auto scopedNodes    = m_nodes->scoped(scope);
        const auto target         = co_await scopedFirmware->find(loop, firmwareId);
        if (!target.has_value())
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::NotFound, "固件不存在"});
        }
        if (signatureRequired(m_signingPolicy) && target->signature.empty())
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::InvalidInput, "该固件未签名，当前部署要求签名固件"});
        }

        std::vector<Device::NodeRecord> nodes;
        nodes.reserve(nodeIds.size());
        std::vector<std::int64_t> invalid;
        for (const std::int64_t nodeId: nodeIds)
        {
            const auto node = co_await scopedNodes->findById(loop, nodeId);
            if (!node.has_value() || node->tenantId != target->tenantId)
            {
                invalid.push_back(nodeId);
                continue;
            }
            nodes.push_back(*node);
        }
        if (!invalid.empty())
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::InvalidInput, "目标节点不存在、不可见或不属于固件所在租户：" + joinIdentifiers(invalid)});
        }

        const std::expected<std::vector<std::vector<std::int64_t>>, std::string> planned = planFirmwareRolloutWaves(policy, nodeIds);
        if (!planned.has_value())
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::InvalidInput, planned.error()});
        }

        std::vector<std::int64_t> previousFirmwareIds;
        previousFirmwareIds.reserve(nodes.size());
        for (const Device::NodeRecord &node: nodes)
        {
            previousFirmwareIds.push_back(co_await resolvePreviousFirmware(loop, *target, node));
        }

        FirmwareRolloutDraft draft;
        draft.rollout.firmwareId = firmwareId;
        draft.rollout.name       = std::move(name);
        draft.rollout.policyJson = policy.toJson();
        draft.rollout.status     = "scheduled";
        draft.rollout.createdBy  = actorId;
        draft.waves.reserve(planned->size());
        for (std::size_t index = 0; index < planned->size(); ++index)
        {
            FirmwareRolloutWaveDraft wave;
            wave.wave.waveIndex = static_cast<int>(index);
            wave.wave.status    = "pending";
            for (const std::int64_t nodeId: (*planned)[index])
            {
                const auto                  position = std::find(nodeIds.begin(), nodeIds.end(), nodeId);
                const std::size_t           offset   = static_cast<std::size_t>(std::distance(nodeIds.begin(), position));
                FirmwareRolloutTargetRecord record;
                record.nodeId             = nodeId;
                record.previousFirmwareId = offset < previousFirmwareIds.size() ? previousFirmwareIds[offset] : 0;
                record.status             = "pending";
                wave.targets.push_back(record);
            }
            draft.waves.push_back(std::move(wave));
        }
        const std::int64_t rolloutId = co_await m_rollouts->scoped(scope)->create(loop, std::move(draft));
        co_return rolloutId;
    }

    AsynGyanis::Core::Task<std::optional<FirmwareRolloutAggregate>> FirmwareRolloutService::find(AsynGyanis::Core::EventLoop &loop, const System::TenantScope scope,
                                                                                                 const std::int64_t rolloutId, const FirmwareRolloutTargetScope &targetScope)
    {
        co_return co_await m_rollouts->scoped(scope)->find(loop, rolloutId, targetScope);
    }

    AsynGyanis::Core::Task<Common::PageRows<FirmwareRolloutRecord>> FirmwareRolloutService::list(AsynGyanis::Core::EventLoop &loop, const System::TenantScope scope,
                                                                                                 const std::int64_t firmwareId, const Common::PageParameters &page,
                                                                                                 const FirmwareRolloutTargetScope &targetScope)
    {
        co_return co_await m_rollouts->scoped(scope)->list(loop, firmwareId, page, targetScope);
    }

    AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> FirmwareRolloutService::transition(AsynGyanis::Core::EventLoop &loop, const System::TenantScope scope,
                                                                                           const std::int64_t rolloutId, const std::vector<std::string> &allowed,
                                                                                           std::string status, std::string pauseReason, std::string lastError)
    {
        const auto repo      = m_rollouts->scoped(scope);
        const auto aggregate = co_await repo->find(loop, rolloutId);
        if (!aggregate.has_value())
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::NotFound, "灰度发布不存在"});
        }
        if (aggregate->rollout.status == status)
        {
            co_return true;
        }
        if (!containsStatus(allowed, aggregate->rollout.status))
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::Conflict, "当前状态 " + aggregate->rollout.status + " 不允许该操作"});
        }
        const bool updated = co_await repo->updateStatus(loop, rolloutId, aggregate->rollout.version, std::move(status), -1, std::move(pauseReason), std::move(lastError));
        if (!updated)
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::Conflict, "灰度发布状态已被其他操作修改，请刷新后重试"});
        }
        co_return true;
    }

    AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> FirmwareRolloutService::pause(AsynGyanis::Core::EventLoop &loop, const System::TenantScope scope,
                                                                                      const std::int64_t rolloutId, std::string reason)
    {
        reason = trimCopy(reason);
        if (reason.empty())
        {
            reason = "人工暂停";
        }
        // 在写库前按列上限截断，门控快照与暂停原因都不会把超长正文带进状态机
        reason = truncateUtf8(reason, 240);
        co_return co_await transition(loop, scope, rolloutId, {"scheduled", "preparing", "running", "paused"}, "paused", std::move(reason));
    }

    AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> FirmwareRolloutService::resume(AsynGyanis::Core::EventLoop &loop, const System::TenantScope scope,
                                                                                       const std::int64_t rolloutId, const TimePoint now)
    {
        const auto repo      = m_rollouts->scoped(scope);
        const auto aggregate = co_await repo->find(loop, rolloutId);
        if (!aggregate.has_value())
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::NotFound, "灰度发布不存在"});
        }
        if (aggregate->rollout.status == "running")
        {
            co_return true;
        }
        if (aggregate->rollout.status != "paused")
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::Conflict, "只有已暂停的灰度发布可以恢复"});
        }
        int maxWave = 1800;
        if (const std::expected<FirmwareRolloutPolicy, std::string> policy = FirmwareRolloutPolicy::parse(aggregate->rollout.policyJson, m_heartbeatSeconds); policy.has_value())
        {
            maxWave = policy->maxWaveSeconds;
        }
        // 先提交 running；波次观察窗在头 CAS 成功之后再重开，失败的恢复请求不会留下提前写入的窗口
        const bool updated = co_await repo->updateStatus(loop, rolloutId, aggregate->rollout.version, "running");
        if (!updated)
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::Conflict, "灰度发布状态已被其他操作修改，请刷新后重试"});
        }
        if (const FirmwareRolloutWaveRecord *wave = waveAt(*aggregate, static_cast<std::size_t>(std::max(0, aggregate->rollout.currentWave)));
            wave != nullptr && !wave->dispatchedAt.empty())
        {
            // 暂停期间的心跳会超过原观察窗口；恢复时以当前时刻重启门控起点，已绑定的命令不重发
            static_cast<void>(co_await repo->setWave(loop, wave->id, "observing", sqlTime(now), sqlTime(addSeconds(now, maxWave))));
        }
        co_return true;
    }

    AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> FirmwareRolloutService::promote(AsynGyanis::Core::EventLoop &loop, const System::TenantScope scope,
                                                                                        const std::int64_t rolloutId, const TimePoint now)
    {
        co_return co_await promoteInternal(loop, scope, rolloutId, "人工提升", false, now);
    }

    AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> FirmwareRolloutService::forcePromote(AsynGyanis::Core::EventLoop &loop, const System::TenantScope scope,
                                                                                             const std::int64_t rolloutId, std::string reason, const TimePoint now)
    {
        reason = trimCopy(reason);
        if (reason.empty())
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::InvalidInput, "强制提升必须提供 reason"});
        }
        // 原因会进入门控快照与 last_error，先按 480 字节上限截断再动状态
        reason = truncateUtf8(reason, 480);
        co_return co_await promoteInternal(loop, scope, rolloutId, std::move(reason), true, now);
    }

    AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> FirmwareRolloutService::promoteInternal(AsynGyanis::Core::EventLoop &loop, const System::TenantScope scope,
                                                                                                const std::int64_t rolloutId, std::string reason, const bool forced,
                                                                                                const TimePoint now)
    {
        const auto repo      = m_rollouts->scoped(scope);
        const auto aggregate = co_await repo->find(loop, rolloutId);
        if (!aggregate.has_value())
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::NotFound, "灰度发布不存在"});
        }
        if (isTerminal(aggregate->rollout.status))
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::Conflict, "灰度发布已结束，不能提升"});
        }
        if (aggregate->rollout.status != "running" && aggregate->rollout.status != "paused")
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::Conflict, "只有运行中或已暂停的灰度发布可以提升"});
        }
        if (aggregate->waves.empty())
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::Conflict, "灰度发布没有可提升的波次"});
        }
        const std::size_t                waveIndex = static_cast<std::size_t>(std::clamp(aggregate->rollout.currentWave, 0, static_cast<int>(aggregate->waves.size()) - 1));
        const FirmwareRolloutWaveRecord *wave      = waveAt(*aggregate, waveIndex);
        if (wave == nullptr)
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::Internal, "灰度波次不存在"});
        }
        // GCC 13 在协程里对 co_await 表达式中的临时 std::string 会触发编译器 ICE；所有传给仓储的
        // 字符串参数都先落成具名局部变量，不把临时对象留在挂起点上。
        const std::vector<FirmwareRolloutTargetRecord> targets = targetsOf(*aggregate, wave->id);
        const std::string                              skipReason = forced ? truncateUtf8("强制提升跳过：" + reason, 200) : "人工提升跳过";
        const std::string passedStatus = "passed";
        const std::string emptyTimestamp;
        const std::string gateTimestamp = sqlTime(now);
        const std::string gateJson      = ConfigValue::object({{"passed", true}, {"forced", forced}, {"reason", reason}, {"at", gateTimestamp}}).dump();
        const std::string nextStatus    = waveIndex + 1 >= aggregate->waves.size() ? "completed" : "running";
        const std::string pauseReason;
        const std::string lastError = forced ? reason : "";
        // 先提交发布头，再写波次与目标；头 CAS 失败时不能留下已经通过的波次或已取消的目标
        const bool updated = co_await repo->updateStatus(loop, rolloutId, aggregate->rollout.version, nextStatus, nextStatus == "completed" ? -1 : static_cast<int>(waveIndex + 1),
                                                         pauseReason, lastError);
        if (!updated)
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::Conflict, "灰度发布状态已被其他操作修改，请刷新后重试"});
        }
        static_cast<void>(co_await repo->setWave(loop, wave->id, passedStatus, emptyTimestamp, emptyTimestamp, gateTimestamp));
        static_cast<void>(co_await repo->appendGate(loop, wave->id, gateJson));
        for (const FirmwareRolloutTargetRecord &target: targets)
        {
            if (target.status == "healthy" || target.status == "failed" || target.status == "rolled_back")
            {
                continue;
            }
            co_await cancelTarget(loop, target, skipReason);
        }
        co_return true;
    }

    AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> FirmwareRolloutService::abort(AsynGyanis::Core::EventLoop &loop, const System::TenantScope scope,
                                                                                      const std::int64_t rolloutId, const TimePoint now)
    {
        const auto repo      = m_rollouts->scoped(scope);
        const auto aggregate = co_await repo->find(loop, rolloutId);
        if (!aggregate.has_value())
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::NotFound, "灰度发布不存在"});
        }
        if (aggregate->rollout.status == "aborted")
        {
            co_return true;
        }
        if (isTerminal(aggregate->rollout.status))
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::Conflict, "灰度发布已结束，不能中止"});
        }
        // 先提交 aborted，再做逐项收尾；头 CAS 失败时不能把波次和目标提前改成中止态
        const bool updated = co_await repo->updateStatus(loop, rolloutId, aggregate->rollout.version, "aborted", -1, "", "人工中止");
        if (!updated)
        {
            co_return std::unexpected(FirmwareRolloutError{FirmwareRolloutErrorKind::Conflict, "灰度发布状态已被其他操作修改，请刷新后重试"});
        }
        for (const FirmwareRolloutWaveRecord &wave: aggregate->waves)
        {
            if (wave.status == "passed" || wave.status == "failed" || wave.status == "skipped" || wave.status == "rolled_back")
            {
                continue;
            }
            static_cast<void>(co_await repo->setWave(loop, wave.id, "skipped", {}, {}, sqlTime(now)));
        }
        for (const FirmwareRolloutTargetRecord &target: aggregate->targets)
        {
            if (target.status == "healthy" || target.status == "rolled_back" || target.status == "skipped")
            {
                continue;
            }
            if (target.status == "failed")
            {
                co_await cancelInFlight(loop, target, "发布已中止，目标未完成");
                continue;
            }
            co_await cancelTarget(loop, target, "发布已中止，目标未完成");
        }
        co_return true;
    }

    AsynGyanis::Core::Task<FirmwareRolloutResult<bool>> FirmwareRolloutService::rollback(AsynGyanis::Core::EventLoop &loop, const System::TenantScope scope,
                                                                                         const std::int64_t rolloutId)
    {
        co_return co_await transition(loop, scope, rolloutId, {"running", "paused", "failed", "rolling_back"}, "rolling_back", "", "人工回滚");
    }

    AsynGyanis::Core::Task<> FirmwareRolloutService::tick(AsynGyanis::Core::EventLoop &loop, const TimePoint now)
    {
        std::vector<FirmwareRolloutRecord> due;
        try
        {
            due = co_await m_allRollouts->listDueRollouts(loop, m_dueLimit);
        } catch (const std::exception &failure)
        {
            LOG_WARN_FMT("灰度发布扫描失败：{}", failure.what());
            co_return;
        }
        for (const FirmwareRolloutRecord &record: due)
        {
            try
            {
                co_await tickOne(loop, record.id, now);
            } catch (const std::exception &failure)
            {
                LOG_WARN_FMT("灰度发布 {} 推进失败：{}", record.id, failure.what());
            }
        }
        co_return;
    }

    AsynGyanis::Core::Task<> FirmwareRolloutService::tickOne(AsynGyanis::Core::EventLoop &loop, const std::int64_t rolloutId, const TimePoint now)
    {
        const std::size_t guardLimit = 64;
        for (std::size_t guard = 0; guard < guardLimit; ++guard)
        {
            const auto aggregate = co_await m_allRollouts->find(loop, rolloutId);
            if (!aggregate.has_value())
            {
                co_return;
            }
            const std::string status = aggregate->rollout.status;
            if (isTerminal(status) || status == "paused" || status == "draft")
            {
                co_return;
            }
            const std::expected<FirmwareRolloutPolicy, std::string> policy = FirmwareRolloutPolicy::parse(aggregate->rollout.policyJson, m_heartbeatSeconds);
            if (!policy.has_value())
            {
                static_cast<void>(co_await m_allRollouts->updateStatus(loop, rolloutId, aggregate->rollout.version, "paused", -1, "灰度策略不合法", policy.error()));
                co_return;
            }
            if (aggregate->waves.empty())
            {
                static_cast<void>(co_await m_allRollouts->updateStatus(loop, rolloutId, aggregate->rollout.version, "failed", -1, "", "灰度发布没有波次"));
                co_return;
            }
            const std::size_t                waveIndex = static_cast<std::size_t>(std::clamp(aggregate->rollout.currentWave, 0, static_cast<int>(aggregate->waves.size()) - 1));
            const FirmwareRolloutWaveRecord *wave      = waveAt(*aggregate, waveIndex);
            if (wave == nullptr)
            {
                static_cast<void>(co_await m_allRollouts->updateStatus(loop, rolloutId, aggregate->rollout.version, "failed", -1, "", "灰度波次不存在"));
                co_return;
            }

            if (status == "rolling_back")
            {
                // 先补齐所有还没有回滚决定的目标，再评估已下发命令的目标；
                // 中途异常或进程重启后，下一拍仍会从缺少决定的目标继续
                co_await dispatchRollback(loop, *aggregate, *policy, now);
                const auto refreshed = co_await m_allRollouts->find(loop, rolloutId);
                if (!refreshed.has_value() || refreshed->rollout.status != "rolling_back")
                {
                    co_return;
                }
                co_await evaluateRollback(loop, *refreshed, *policy, now);
                co_return;
            }
            if ((status == "scheduled" || status == "preparing") && !policy->allowsTime(now))
            {
                co_return;
            }
            if (targetsOf(*aggregate, wave->id).empty())
            {
                // 小批量时累计百分比可能把后几波折叠成空波；按已通过处理并直接推进
                static_cast<void>(co_await m_allRollouts->setWave(loop, wave->id, "passed", {}, {}, sqlTime(now)));
                static_cast<void>(co_await m_allRollouts->appendGate(loop, wave->id, ConfigValue::object({{"passed", true}, {"skipped", true}, {"at", sqlTime(now)}}).dump()));
                const bool last  = waveIndex + 1 >= aggregate->waves.size();
                const bool moved = co_await m_allRollouts->updateStatus(loop, rolloutId, aggregate->rollout.version, last ? "completed" : "running",
                                                                        last ? -1 : static_cast<int>(waveIndex + 1));
                if (!moved)
                {
                    co_return;
                }
                if (last)
                {
                    co_return;
                }
                continue;
            }

            if (status == "scheduled" || status == "preparing" || status == "running")
            {
                // 维护窗口约束每一次新波次派发；已派发的波次继续观察与回滚
                if (wave->dispatchedAt.empty() && !policy->allowsTime(now))
                {
                    co_return;
                }
                co_await dispatchWave(loop, *aggregate, waveIndex, *policy, now);
            }
            if (status == "running")
            {
                const auto refreshed = co_await m_allRollouts->find(loop, rolloutId);
                if (!refreshed.has_value())
                {
                    co_return;
                }
                const FirmwareRolloutWaveRecord *current = waveAt(*refreshed, waveIndex);
                if (current == nullptr)
                {
                    co_return;
                }
                const std::vector<FirmwareRolloutTargetRecord> targets    = targetsOf(*refreshed, current->id);
                bool                                           allSettled = true;
                for (const FirmwareRolloutTargetRecord &target: targets)
                {
                    if (target.commandId <= 0 && target.status != "failed")
                    {
                        allSettled = false;
                        break;
                    }
                }
                if (!allSettled || current->dispatchedAt.empty())
                {
                    co_return;
                }
                const std::optional<TimePoint> gateStart = parseSqlTime(current->dispatchedAt);
                if (!gateStart.has_value())
                {
                    static_cast<void>(co_await m_allRollouts->updateStatus(loop, rolloutId, refreshed->rollout.version, "paused", -1, "波次派发时间不合法", "波次派发时间不合法"));
                    co_return;
                }
                if (now < addSeconds(*gateStart, policy->settleSeconds))
                {
                    co_return;
                }
                co_await gateWave(loop, *refreshed, waveIndex, *policy, now);
                const auto after = co_await m_allRollouts->find(loop, rolloutId);
                if (!after.has_value() || after->rollout.status != "running" || after->rollout.currentWave <= static_cast<int>(waveIndex))
                {
                    co_return;
                }
                continue;
            }
            co_return;
        }
        co_return;
    }

    AsynGyanis::Core::Task<> FirmwareRolloutService::dispatchWave(AsynGyanis::Core::EventLoop &loop, const FirmwareRolloutAggregate aggregate, const std::size_t waveIndex,
                                                                  const FirmwareRolloutPolicy &policy, const TimePoint now)
    {
        const FirmwareRolloutWaveRecord *wave = waveAt(aggregate, waveIndex);
        if (wave == nullptr)
        {
            co_return;
        }
        const auto targetFirmware = co_await m_allFirmware->find(loop, aggregate.rollout.firmwareId);
        if (!targetFirmware.has_value())
        {
            static_cast<void>(co_await m_allRollouts->updateStatus(loop, aggregate.rollout.id, aggregate.rollout.version, "paused", -1, "目标固件不存在", "目标固件不存在"));
            co_return;
        }
        std::vector<FirmwareRolloutTargetRecord> targets = targetsOf(aggregate, wave->id);
        if (targets.empty())
        {
            co_return;
        }
        const std::size_t cap    = policy.maxConcurrentPerWave > 0 ? static_cast<std::size_t>(policy.maxConcurrentPerWave) : std::numeric_limits<std::size_t>::max();
        std::size_t       active = 0;
        std::size_t       settled = 0;
        if (wave->dispatchedAt.empty() && wave->status != "dispatching")
        {
            // 先标记派发中；观察窗口从全部目标绑定完成时开始计算
            static_cast<void>(co_await m_allRollouts->setWave(loop, wave->id, "dispatching", {}, {}, {}));
        }
        for (const FirmwareRolloutTargetRecord &target: targets)
        {
            if (target.commandId > 0)
            {
                ++settled;
                // 并发上限针对仍在执行的命令；已结束的命令应交出名额给后续目标
                const auto command = co_await m_allCommands->find(loop, target.commandId);
                if (!command.has_value() || command->status == "pending" || command->status == "running")
                {
                    ++active;
                }
            } else if (target.status == "failed" || target.status == "skipped")
            {
                ++settled;
            }
        }
        const std::string claimHealth = ConfigValue::object({{"gate", "dispatching"}}).dump();
        const std::string staleBefore = sqlTime(addSeconds(now, -m_heartbeatSeconds));
        for (const FirmwareRolloutTargetRecord &target: targets)
        {
            if (target.commandId > 0 || target.status == "failed" || target.status == "skipped")
            {
                continue;
            }
            if (active >= cap)
            {
                break;
            }
            // 发布头版本、状态、当前波次与目标状态在同一事务里校验；抢不到派发权的目标留给下一拍
            const auto claim = co_await m_allRollouts->claimTarget(loop, aggregate.rollout.id, target.id, aggregate.rollout.version, static_cast<int>(waveIndex), target.status,
                                                                   target.taskId, target.commandId, false, claimHealth, staleBefore);
            if (claim == FirmwareRolloutClaimResult::RolloutChanged)
            {
                co_return;
            }
            if (claim == FirmwareRolloutClaimResult::TargetNotClaimable)
            {
                continue;
            }
            const auto node = co_await m_allNodes->findById(loop, target.nodeId);
            if (!node.has_value() || node->tenantId != aggregate.rollout.tenantId)
            {
                co_await failTarget(loop, target, "目标节点不存在、已删除或不属于发布租户", "dispatching");
                ++settled;
                continue;
            }
            if (signatureRequired(m_signingPolicy) && targetFirmware->signature.empty())
            {
                co_await failTarget(loop, target, "目标固件未签名，当前部署要求签名固件", "dispatching");
                ++settled;
                continue;
            }
            std::int64_t previousFirmwareId = target.previousFirmwareId;
            std::string  resolveFailure;
            try
            {
                previousFirmwareId = co_await resolvePreviousFirmware(loop, *targetFirmware, *node);
            } catch (const std::exception &failure)
            {
                resolveFailure = failure.what();
            }
            if (!resolveFailure.empty())
            {
                co_await failTarget(loop, target, "解析上一固件失败：" + truncateUtf8(resolveFailure, 200), "dispatching");
                ++settled;
                continue;
            }
            if (previousFirmwareId != target.previousFirmwareId)
            {
                FirmwareRolloutTargetUpdate previousUpdate;
                previousUpdate.previousFirmwareId = previousFirmwareId;
                static_cast<void>(co_await m_allRollouts->updateTarget(loop, target.id, previousUpdate, "dispatching"));
            }
            std::int64_t                          taskId = 0;
            std::optional<Command::CommandRecord> command;
            bool                                  createdCommand = false;
            std::string                           taskFailure;
            try
            {
                // 进程在命令已绑定、目标未回写之间重启时沿用原任务与命令，绝不再建第二份
                const std::optional<FirmwareTaskRecord> existing = co_await existingTask(loop, aggregate.rollout.firmwareId, aggregate.rollout.id, wave->id, target.nodeId);
                if (existing.has_value())
                {
                    taskId = existing->id;
                    if (existing->commandId > 0)
                    {
                        auto existingCommand = co_await m_allCommands->find(loop, existing->commandId);
                        if (!existingCommand.has_value() || existingCommand->nodeId != target.nodeId ||
                            !commandTargetsFirmware(existingCommand->payload, aggregate.rollout.firmwareId))
                        {
                            taskFailure = "已有任务绑定的命令与目标不符，拒绝重复下发";
                        } else
                        {
                            command = std::move(existingCommand);
                        }
                    }
                }
                if (taskFailure.empty() && !command.has_value())
                {
                    if (taskId == 0)
                    {
                        taskId = co_await m_allFirmware->createTask(loop, aggregate.rollout.firmwareId, target.nodeId, previousFirmwareId, aggregate.rollout.id, wave->id, 0);
                    }
                    if (taskId == 0)
                    {
                        const std::optional<FirmwareTaskRecord> raced = co_await existingTask(loop, aggregate.rollout.firmwareId, aggregate.rollout.id, wave->id, target.nodeId);
                        if (raced.has_value())
                        {
                            taskId = raced->id;
                            if (raced->commandId > 0)
                            {
                                auto existingCommand = co_await m_allCommands->find(loop, raced->commandId);
                                if (existingCommand.has_value() && existingCommand->nodeId == target.nodeId &&
                                    commandTargetsFirmware(existingCommand->payload, aggregate.rollout.firmwareId))
                                {
                                    command = std::move(existingCommand);
                                }
                            }
                        }
                    }
                    if (taskId == 0)
                    {
                        taskFailure = "无法创建固件任务";
                    } else if (!command.has_value())
                    {
                        auto created = co_await m_allCommands->create(loop, target.nodeId, "firmware.update", downloadPayload(*targetFirmware, target.nodeId),
                                                                      aggregate.rollout.createdBy, m_commandTimeout);
                        if (!created.has_value())
                        {
                            static_cast<void>(co_await m_allFirmware->updateTaskById(loop, taskId, "failed", 0, "创建固件命令失败"));
                            taskFailure = "创建固件命令失败";
                        } else
                        {
                            command        = std::move(created);
                            createdCommand = true;
                            const bool bound = co_await m_allFirmware->bindTaskCommand(loop, taskId, command->id);
                            if (!bound)
                            {
                                static_cast<void>(co_await m_allCommands->cancel(loop, command->id));
                                static_cast<void>(co_await m_allFirmware->updateTaskById(loop, taskId, "failed", 0, "绑定任务与命令失败"));
                                command.reset();
                                taskFailure = "绑定任务与命令失败";
                            }
                        }
                    }
                }
            } catch (const std::exception &failure)
            {
                taskFailure = failure.what();
            }
            if (!taskFailure.empty() && command.has_value())
            {
                // 绑定抛错时命令可能已落库，必须取消并关闭任务，不能继续发布
                static_cast<void>(co_await m_allCommands->cancel(loop, command->id));
                static_cast<void>(co_await m_allFirmware->updateTaskById(loop, taskId, "failed", 0, "绑定任务与命令失败"));
                command.reset();
            }
            if (!command.has_value())
            {
                if (taskId > 0)
                {
                    static_cast<void>(co_await m_allFirmware->updateTaskById(loop, taskId, "failed", 0, "创建固件任务或命令失败"));
                }
                co_await failTarget(loop, target, "创建固件任务失败：" + truncateUtf8(taskFailure.empty() ? "未知原因" : taskFailure, 200), "dispatching");
                ++settled;
                continue;
            }
            // 目标与命令的绑定必须在发布前落库；崩溃后重试会命中这条绑定而不是再建一份
            FirmwareRolloutTargetUpdate binding;
            binding.status     = "installing";
            binding.taskId     = taskId;
            binding.commandId  = command->id;
            binding.healthJson = ConfigValue::object({{"gate", "pending"}, {"commandId", command->id}, {"taskId", taskId}}).dump();
            if (!co_await m_allRollouts->updateTarget(loop, target.id, binding, "dispatching"))
            {
                if (createdCommand)
                {
                    static_cast<void>(co_await m_allCommands->cancel(loop, command->id));
                }
                static_cast<void>(co_await m_allFirmware->updateTaskById(loop, taskId, "failed", 0, "目标绑定失败"));
                // 目标已被人工动作改写时保留其终局状态；发布头变化则停止本波
                if (!co_await rolloutStillReady(loop, aggregate.rollout.id, aggregate.rollout.version, static_cast<int>(waveIndex), {"scheduled", "preparing", "running"}))
                {
                    co_return;
                }
                continue;
            }
            // 发布前再核对一次发布头：暂停、中止、提升或版本变化都不能把命令送出去
            if (!co_await rolloutStillReady(loop, aggregate.rollout.id, aggregate.rollout.version, static_cast<int>(waveIndex), {"scheduled", "preparing", "running"}))
            {
                if (createdCommand)
                {
                    static_cast<void>(co_await m_allCommands->cancel(loop, command->id));
                }
                static_cast<void>(co_await m_allFirmware->updateTaskById(loop, taskId, "failed", 0, "派发期间发布状态变化，取消未送达的命令"));
                // 命令尚未发布，目标回到待派发；已进入终局的目标不会被这里改回
                FirmwareRolloutTargetUpdate release;
                release.status     = "pending";
                release.taskId     = 0;
                release.commandId  = 0;
                release.healthJson = "{}";
                static_cast<void>(co_await m_allRollouts->releaseTarget(loop, target.id, "installing", release));
                co_return;
            }
            bool        publishFailed = false;
            std::string publishError;
            if (createdCommand)
            {
                try
                {
                    co_await m_publisher(*command);
                } catch (const std::exception &failure)
                {
                    publishFailed = true;
                    publishError  = failure.what();
                }
            }
            if (publishFailed)
            {
                static_cast<void>(co_await m_allCommands->cancel(loop, command->id));
                static_cast<void>(co_await m_allFirmware->updateTaskById(loop, taskId, "failed", 0, "发布固件命令失败"));
                co_await failTarget(loop, target, "发布固件命令失败", "installing");
                LOG_WARN_FMT("灰度发布 {} 命令 {} 发布失败：{}", aggregate.rollout.id, command->id, publishError);
                ++settled;
                continue;
            }
            // 发布后立刻再读一次：状态已变化时取消刚送达的命令，但不再改写目标终局
            if (createdCommand && !co_await rolloutStillReady(loop, aggregate.rollout.id, aggregate.rollout.version, static_cast<int>(waveIndex),
                                                              {"scheduled", "preparing", "running"}))
            {
                static_cast<void>(co_await m_allCommands->cancel(loop, command->id));
                static_cast<void>(co_await m_allFirmware->updateTaskById(loop, taskId, "failed", 0, "发布在派发期间暂停或中止"));
                co_return;
            }
            ++active;
            ++settled;
        }

        const auto refreshed = co_await m_allRollouts->find(loop, aggregate.rollout.id);
        if (!refreshed.has_value() || refreshed->rollout.version != aggregate.rollout.version || refreshed->rollout.currentWave != static_cast<int>(waveIndex) ||
            (refreshed->rollout.status != "scheduled" && refreshed->rollout.status != "preparing" && refreshed->rollout.status != "running"))
        {
            co_return;
        }
        const bool allSettled = settled >= targets.size();
        if (allSettled)
        {
            // 观察窗口已经确定时不再每拍重写波次行
            if (wave->status != "observing" || wave->dispatchedAt.empty() || wave->deadlineAt.empty())
            {
                const std::string              dispatchedAt = wave->dispatchedAt.empty() ? sqlTime(now) : wave->dispatchedAt;
                const std::optional<TimePoint> start        = parseSqlTime(dispatchedAt);
                const std::string              deadline     = start.has_value() ? sqlTime(addSeconds(*start, policy.maxWaveSeconds)) : std::string{};
                static_cast<void>(co_await m_allRollouts->setWave(loop, wave->id, "observing", dispatchedAt, deadline));
            }
        } else if (wave->status != "dispatching")
        {
            static_cast<void>(co_await m_allRollouts->setWave(loop, wave->id, "dispatching", {}, {}, {}));
        }
        if (refreshed->rollout.status != "running" || refreshed->rollout.currentWave != static_cast<int>(waveIndex))
        {
            static_cast<void>(co_await m_allRollouts->updateStatus(loop, aggregate.rollout.id, aggregate.rollout.version, "running", static_cast<int>(waveIndex)));
        }
        co_return;
    }

    AsynGyanis::Core::Task<> FirmwareRolloutService::gateWave(AsynGyanis::Core::EventLoop &loop, const FirmwareRolloutAggregate aggregate, const std::size_t waveIndex,
                                                              const FirmwareRolloutPolicy &policy, const TimePoint now)
    {
        const FirmwareRolloutWaveRecord *wave = waveAt(aggregate, waveIndex);
        if (wave == nullptr)
        {
            co_return;
        }
        const std::optional<TimePoint> gateStart = parseSqlTime(wave->dispatchedAt);
        if (!gateStart.has_value())
        {
            co_return;
        }
        const TimePoint settleEnd  = addSeconds(*gateStart, policy.settleSeconds);
        const TimePoint observeEnd = addSeconds(*gateStart, std::max(policy.observeSeconds, policy.settleSeconds));
        const TimePoint maxWaveEnd = addSeconds(*gateStart, policy.maxWaveSeconds);
        if (now < settleEnd)
        {
            co_return;
        }
        const auto targetFirmware = co_await m_allFirmware->find(loop, aggregate.rollout.firmwareId);
        if (!targetFirmware.has_value())
        {
            static_cast<void>(co_await m_allRollouts->updateStatus(loop, aggregate.rollout.id, aggregate.rollout.version, "paused", -1, "目标固件不存在", "目标固件不存在"));
            co_return;
        }

        const std::vector<FirmwareRolloutTargetRecord> targets       = targetsOf(aggregate, wave->id);
        std::size_t                                    healthy       = 0;
        std::size_t                                    failed        = 0;
        std::size_t                                    indeterminate = 0;
        std::vector<std::string>                       reasons;
        std::vector<TargetGateResult>                  gateResults;
        for (const FirmwareRolloutTargetRecord &target: targets)
        {
            if (target.commandId <= 0 && target.status == "failed")
            {
                ++failed;
                TargetGateResult skipped;
                skipped.targetId = target.id;
                skipped.nodeId   = target.nodeId;
                skipped.status   = "failed";
                skipped.failed   = true;
                skipped.reason   = "节点此前下发失败";
                gateResults.push_back(std::move(skipped));
                continue;
            }
            TargetGateResult result = co_await evaluateTarget(loop, aggregate.rollout, target, *targetFirmware, policy, *gateStart, settleEnd, observeEnd, now);
            co_await persistGate(loop, target, result);
            if (result.healthy)
            {
                ++healthy;
            } else if (result.failed)
            {
                ++failed;
                if (reasons.size() < 8 && !result.reason.empty())
                {
                    reasons.push_back("节点 " + std::to_string(result.nodeId) + "：" + result.reason);
                }
            } else
            {
                ++indeterminate;
            }
            gateResults.push_back(result);
        }
        std::size_t previousHealthy = 0;
        std::size_t previousTotal   = 0;
        std::unordered_set<std::int64_t> earlierWaveIds;
        for (const FirmwareRolloutWaveRecord &earlier: aggregate.waves)
        {
            if (earlier.waveIndex < static_cast<int>(waveIndex))
            {
                earlierWaveIds.insert(earlier.id);
            }
        }
        for (const FirmwareRolloutTargetRecord &target: aggregate.targets)
        {
            if (!earlierWaveIds.contains(target.waveId))
            {
                continue;
            }
            if (target.status == "healthy")
            {
                ++previousHealthy;
                ++previousTotal;
            } else if (target.status == "failed")
            {
                ++previousTotal;
            }
        }
        const std::size_t total                = targets.size();
        const double      waveRatio            = total == 0 ? 1.0 : static_cast<double>(healthy) / static_cast<double>(total);
        const double      cumulativeTotal      = static_cast<double>(previousTotal + total);
        const double      cumulativeRatio      = cumulativeTotal <= 0.0 ? 1.0 : static_cast<double>(previousHealthy + healthy) / cumulativeTotal;
        const bool        ratiosOk             = waveRatio + 1e-9 >= policy.minWaveSuccessRatio && cumulativeRatio + 1e-9 >= policy.minCumulativeSuccessRatio;
        const bool        failuresOk           = policy.maxFailures == 0 || failed <= static_cast<std::size_t>(policy.maxFailures);
        const bool        shouldPass           = ratiosOk && failuresOk;
        const double      optimisticWave       = total == 0 ? 1.0 : static_cast<double>(healthy + indeterminate) / static_cast<double>(total);
        const double      optimisticCumulative = cumulativeTotal <= 0.0 ? 1.0 : static_cast<double>(previousHealthy + healthy + indeterminate) / cumulativeTotal;
        const bool        cannotPass           = optimisticWave + 1e-9 < policy.minWaveSuccessRatio || optimisticCumulative + 1e-9 < policy.minCumulativeSuccessRatio ||
                                                 (policy.maxFailures > 0 && failed > static_cast<std::size_t>(policy.maxFailures));
        const bool        atFinal              = now >= observeEnd;
        const bool        atDeadline           = now >= maxWaveEnd;

        const std::string waveLabel  = "第 " + std::to_string(waveIndex + 1) + " 波";
        auto              finishWave = [&](const bool passed, const bool timeout) -> AsynGyanis::Core::Task<>
        {
            std::string reason;
            if (passed)
            {
                reason = waveLabel + "健康门控通过：" + std::to_string(healthy) + "/" + std::to_string(total);
            } else if (timeout)
            {
                reason = waveLabel + "超时：" + std::to_string(healthy) + "/" + std::to_string(total) + " 通过，" + std::to_string(failed) + " 失败";
            } else
            {
                reason = waveLabel + "健康门控未通过：" + std::to_string(healthy) + "/" + std::to_string(total) + " 通过，" + std::to_string(failed) + " 失败";
                if (!reasons.empty())
                {
                    for (std::size_t index = 0; index < reasons.size(); ++index)
                    {
                        reason += index == 0 ? "；" : "，";
                        reason += reasons[index];
                    }
                }
            }
            if (!passed)
            {
                for (const TargetGateResult &result: gateResults)
                {
                    if (result.healthy || result.failed)
                    {
                        continue;
                    }
                    FirmwareRolloutTargetUpdate update;
                    update.status      = "failed";
                    std::string detail = timeout ? "波次超时未完成" : "未达到成功率";
                    if (!result.reason.empty())
                    {
                        detail += "：" + result.reason;
                    }
                    update.healthJson = ConfigValue::object({{"gate", "failed"}, {"reason", detail}}).dump();
                    static_cast<void>(co_await m_allRollouts->updateTarget(loop, result.targetId, update));
                }
            } else
            {
                for (const TargetGateResult &result: gateResults)
                {
                    // 已判失败的目标保留逐项目健康原因，只把悬而未决的目标按未确认收尾
                    if (result.healthy || result.failed)
                    {
                        continue;
                    }
                    FirmwareRolloutTargetUpdate update;
                    update.status     = "failed";
                    update.healthJson = ConfigValue::object({{"gate", "failed"}, {"reason", "波次按成功率通过，该目标未在观察窗口内确认"}}).dump();
                    static_cast<void>(co_await m_allRollouts->updateTarget(loop, result.targetId, update));
                }
            }
            static_cast<void>(co_await m_allRollouts->setWave(loop, wave->id, passed ? "passed" : "failed", {}, {}, sqlTime(now)));
            static_cast<void>(co_await m_allRollouts->appendGate(
                    loop, wave->id,
                    ConfigValue::object(
                            {{"passed", passed}, {"timeout", timeout}, {"healthy", healthy}, {"failed", failed}, {"total", total}, {"reason", reason}, {"at", sqlTime(now)}})
                            .dump()));
            const bool last = waveIndex + 1 >= aggregate.waves.size();
            if (passed)
            {
                static_cast<void>(co_await m_allRollouts->updateStatus(loop, aggregate.rollout.id, aggregate.rollout.version, last ? "completed" : "running",
                                                                       last ? -1 : static_cast<int>(waveIndex + 1)));
                co_return;
            }
            if (policy.autoRollback)
            {
                static_cast<void>(co_await m_allRollouts->updateStatus(loop, aggregate.rollout.id, aggregate.rollout.version, "rolling_back", static_cast<int>(waveIndex), "",
                                                                       truncateUtf8(reason, 480)));
                co_return;
            }
            if (timeout && policy.onTimeout == "fail")
            {
                static_cast<void>(co_await m_allRollouts->updateStatus(loop, aggregate.rollout.id, aggregate.rollout.version, "failed", static_cast<int>(waveIndex), "",
                                                                       truncateUtf8(reason, 480)));
                co_return;
            }
            static_cast<void>(co_await m_allRollouts->updateStatus(loop, aggregate.rollout.id, aggregate.rollout.version, "paused", static_cast<int>(waveIndex),
                                                                   truncateUtf8(reason, 240), truncateUtf8(reason, 480)));
            co_return;
        };

        if (atFinal)
        {
            // 观察窗口结束必须给出结论：未确认的目标已在逐项评估里落成失败
            co_await finishWave(shouldPass, !shouldPass);
            co_return;
        }
        if (atDeadline)
        {
            co_await finishWave(false, true);
            co_return;
        }
        if (cannotPass)
        {
            co_await finishWave(false, false);
            co_return;
        }
        co_return;
    }

    AsynGyanis::Core::Task<FirmwareRolloutService::TargetGateResult>
    FirmwareRolloutService::evaluateTarget(AsynGyanis::Core::EventLoop &loop, const FirmwareRolloutRecord &rollout, const FirmwareRolloutTargetRecord &target,
                                           const FirmwareRecord &targetFirmware, const FirmwareRolloutPolicy &policy, const TimePoint gateStart, const TimePoint settleEnd,
                                           const TimePoint observeEnd, const TimePoint now)
    {
        TargetGateResult result;
        result.targetId = target.id;
        result.nodeId   = target.nodeId;

        std::string commandStatus;
        std::string taskStatus;
        bool        nodeOnline = false;
        std::string lastSeen;
        std::string installedVersion;
        std::string telemetryCreatedAt;
        std::string telemetryCollectedAt;
        std::string alarmMetric;
        std::string failure;
        std::string waiting;
        const bool  atObserveEnd = now >= observeEnd;

        // 先看命令与任务；安装确认只是证据之一，不再是一切证据的门槛
        if (target.commandId <= 0)
        {
            waiting = "等待命令下发";
        } else
        {
            const auto command = co_await m_allCommands->find(loop, target.commandId);
            if (!command.has_value())
            {
                failure = "关联命令不存在";
            } else
            {
                commandStatus = command->status;
                if (commandStatus == "failed" || commandStatus == "timeout" || commandStatus == "canceled")
                {
                    failure = "命令状态为 " + commandStatus;
                } else if (commandStatus != "succeeded")
                {
                    waiting = "命令尚未完成";
                }
            }
        }

        bool installConfirmed = false;
        if (failure.empty() && commandStatus == "succeeded")
        {
            std::optional<FirmwareTaskRecord> task;
            if (target.taskId > 0)
            {
                task = co_await m_allFirmware->findTask(loop, target.taskId);
            }
            if (!task.has_value())
            {
                failure = "关联任务不存在";
            } else
            {
                taskStatus = task->status;
                if (task->commandId != target.commandId)
                {
                    failure = "任务与命令不匹配";
                } else if (taskStatus == "failed")
                {
                    failure = "任务安装失败";
                } else if (taskStatus != "installed")
                {
                    waiting = "任务尚未确认安装";
                }
            }
        }
        installConfirmed = failure.empty() && commandStatus == "succeeded" && taskStatus == "installed";

        // 节点、遥测与告警证据对每个目标都评估，而不是只在安装确认后才看
        const auto node = co_await m_allNodes->findById(loop, target.nodeId);
        if (!node.has_value() || node->tenantId != rollout.tenantId)
        {
            if (failure.empty())
            {
                failure = "节点不存在或不属于发布租户";
            }
        } else
        {
            nodeOnline       = node->status == "online";
            lastSeen         = node->lastSeen;
            installedVersion = node->installedFirmwareVersion;
            if (failure.empty())
            {
                const std::optional<TimePoint> seen = parseSqlTime(node->lastSeen);
                if (!nodeOnline)
                {
                    if (installConfirmed || atObserveEnd)
                    {
                        failure = "节点离线";
                    } else
                    {
                        waiting = "节点离线";
                    }
                } else if (!seen.has_value() || *seen < settleEnd)
                {
                    if (installConfirmed || atObserveEnd)
                    {
                        failure = "最新心跳早于观察窗口";
                    } else
                    {
                        waiting = "心跳尚未覆盖观察窗口";
                    }
                }
            }
            if (failure.empty() && installConfirmed)
            {
                if (installedVersion.empty())
                {
                    if (policy.requireInstalledVersion)
                    {
                        if (atObserveEnd)
                        {
                            failure = "节点未上报已安装固件版本";
                        } else
                        {
                            waiting = "节点尚未上报已安装固件版本";
                        }
                    }
                } else if (installedVersion != targetFirmware.version)
                {
                    if (atObserveEnd)
                    {
                        failure = "已安装版本 " + installedVersion + " 与目标版本 " + targetFirmware.version + " 不一致";
                    } else
                    {
                        waiting = "已安装版本 " + installedVersion + " 与目标版本 " + targetFirmware.version + " 不一致";
                    }
                }
            }
        }
        if (failure.empty())
        {
            // 只用最新一条样本证明新鲜度；接收与采集时间必须来自同一条记录
            const auto sample = co_await m_allTelemetry->newestSample(loop, target.nodeId);
            if (!sample.has_value())
            {
                if (atObserveEnd)
                {
                    failure = "缺少新鲜遥测";
                } else
                {
                    waiting = "缺少新鲜遥测";
                }
            } else
            {
                telemetryCreatedAt                         = sample->createdAt;
                telemetryCollectedAt                       = sample->collectedAt;
                const std::optional<TimePoint> createdAt   = parseSqlTime(sample->createdAt);
                const std::optional<TimePoint> collectedAt = parseSqlTime(sample->collectedAt);
                if (!createdAt.has_value())
                {
                    failure = "遥测接收时间不合法";
                } else if (*createdAt < settleEnd)
                {
                    if (atObserveEnd)
                    {
                        failure = "遥测未覆盖沉降窗口";
                    } else
                    {
                        waiting = "遥测未覆盖沉降窗口";
                    }
                } else if (collectedAt.has_value() && *collectedAt < addSeconds(gateStart, -kTelemetryClockSkewSeconds))
                {
                    if (atObserveEnd)
                    {
                        failure = "遥测采集时间早于允许偏差";
                    } else
                    {
                        waiting = "遥测采集时间早于允许偏差";
                    }
                } else if (*createdAt < addSeconds(now, -policy.requiredFreshTelemetrySeconds))
                {
                    if (atObserveEnd)
                    {
                        failure = "遥测已过期";
                    } else
                    {
                        waiting = "遥测已过期";
                    }
                }
            }
        }
        if (failure.empty())
        {
            const auto alarms = co_await m_allAlarms->triggeredSince(loop, target.nodeId, sqlTime(gateStart), 200);
            for (const Alarm::AlarmRecord &alarm: alarms)
            {
                if (isExempt(policy, alarm.metric))
                {
                    continue;
                }
                alarmMetric = alarm.metric;
                failure     = "观察窗口内新增告警：" + alarm.metric;
                break;
            }
        }
        if (failure.empty() && !installConfirmed && atObserveEnd)
        {
            // 观察窗口结束时仍没有安装证据的目标必须落成失败，不能悬在 installing
            failure = waiting.empty() ? "观察窗口结束仍未确认安装" : waiting;
        }

        if (!failure.empty())
        {
            result.status = "failed";
            result.failed = true;
            result.reason = failure;
        } else if (installConfirmed && waiting.empty())
        {
            result.status  = "healthy";
            result.healthy = true;
        } else
        {
            result.status        = "installing";
            result.indeterminate = true;
            result.reason        = waiting.empty() ? "等待命令或任务完成" : waiting;
        }
        result.healthJson = healthText(result.status, result.reason, target.commandId, commandStatus, target.taskId, taskStatus, nodeOnline, lastSeen, installedVersion,
                                       telemetryCreatedAt, telemetryCollectedAt, alarmMetric);
        co_return result;
    }

    AsynGyanis::Core::Task<> FirmwareRolloutService::persistGate(AsynGyanis::Core::EventLoop &loop, const FirmwareRolloutTargetRecord &target, const TargetGateResult &result)
    {
        // 心跳与遥测时间戳每拍都会变化，状态和原因不变时不重复写库
        if (target.status == result.status && healthIdentity(target.healthJson) == healthIdentity(result.healthJson))
        {
            co_return;
        }
        FirmwareRolloutTargetUpdate update;
        update.status     = result.status;
        update.healthJson = result.healthJson;
        static_cast<void>(co_await m_allRollouts->updateTarget(loop, target.id, update));
        co_return;
    }

    AsynGyanis::Core::Task<> FirmwareRolloutService::dispatchRollback(AsynGyanis::Core::EventLoop &loop, const FirmwareRolloutAggregate aggregate,
                                                                      const FirmwareRolloutPolicy &policy, const TimePoint now)
    {
        const std::size_t                waveIndex = static_cast<std::size_t>(std::clamp(aggregate.rollout.currentWave, 0, static_cast<int>(aggregate.waves.size()) - 1));
        const FirmwareRolloutWaveRecord *wave      = waveAt(aggregate, waveIndex);
        if (wave == nullptr)
        {
            static_cast<void>(
                    co_await m_allRollouts->updateStatus(loop, aggregate.rollout.id, aggregate.rollout.version, "failed", aggregate.rollout.currentWave, "", "回滚波次不存在"));
            co_return;
        }
        // 回滚期限在进入回滚时确定；中断恢复后沿用原期限，避免每次补发都把期限向后推
        const std::string rollbackDeadline = (wave->status == "rolling_back" && !wave->deadlineAt.empty()) ? wave->deadlineAt : sqlTime(addSeconds(now, policy.maxWaveSeconds));
        if (wave->status != "rolling_back" || wave->deadlineAt.empty())
        {
            const std::string dispatchedAt = wave->dispatchedAt.empty() ? sqlTime(now) : wave->dispatchedAt;
            static_cast<void>(co_await m_allRollouts->setWave(loop, wave->id, "rolling_back", dispatchedAt, rollbackDeadline));
        }
        // 已通过的波次同样属于本次回滚；把它们的波次状态一并推进，聚合才不会头尾矛盾
        for (const FirmwareRolloutWaveRecord &earlier: aggregate.waves)
        {
            if (earlier.waveIndex < static_cast<int>(waveIndex) && (earlier.status == "passed" || earlier.status == "failed"))
            {
                static_cast<void>(co_await m_allRollouts->setWave(loop, earlier.id, "rolling_back", {}, rollbackDeadline));
            }
        }
        std::vector<std::string> unavailable;
        std::size_t              issued = 0;
        const std::string        claimHealth = ConfigValue::object({{"gate", "rollback_claiming"}}).dump();
        const std::string        staleBefore = sqlTime(addSeconds(now, -m_heartbeatSeconds));
        for (const FirmwareRolloutTargetRecord &target: aggregate.targets)
        {
            if (rollbackDecided(target))
            {
                continue;
            }
            const auto current = co_await m_allRollouts->find(loop, aggregate.rollout.id);
            if (!current.has_value() || current->rollout.version != aggregate.rollout.version || current->rollout.status != "rolling_back")
            {
                co_return;
            }
            co_await cancelInFlight(loop, target, "回滚前停止未完成的安装命令");
            const auto claim = co_await m_allRollouts->claimTarget(loop, aggregate.rollout.id, target.id, aggregate.rollout.version, static_cast<int>(waveIndex), target.status,
                                                                   target.taskId, target.commandId, true, claimHealth, staleBefore);
            if (claim == FirmwareRolloutClaimResult::RolloutChanged)
            {
                co_return;
            }
            if (claim == FirmwareRolloutClaimResult::TargetNotClaimable)
            {
                continue;
            }
            if (target.commandId <= 0)
            {
                // 从未下发过安装命令的目标没有可回滚的现场，落成跳过而不是“不可用”
                FirmwareRolloutTargetUpdate decision;
                decision.status     = "skipped";
                decision.healthJson = ConfigValue::object({{"gate", "rollback_skipped"}, {"reason", "节点 " + std::to_string(target.nodeId) + " 未下发安装命令，无需回滚"}}).dump();
                static_cast<void>(co_await m_allRollouts->updateTarget(loop, target.id, decision, "dispatching"));
                continue;
            }
            std::string reason;
            bool        unavailableDecision = false;
            if (target.previousFirmwareId <= 0)
            {
                reason              = "节点 " + std::to_string(target.nodeId) + " 缺少可回滚的历史版本";
                unavailableDecision = true;
            }
            std::optional<FirmwareRecord> previous;
            if (reason.empty())
            {
                previous = co_await m_allFirmware->find(loop, target.previousFirmwareId);
                if (!previous.has_value() || previous->tenantId != aggregate.rollout.tenantId)
                {
                    reason              = "节点 " + std::to_string(target.nodeId) + " 的历史固件不存在";
                    unavailableDecision = true;
                } else if (signatureRequired(m_signingPolicy) && previous->signature.empty())
                {
                    reason              = "节点 " + std::to_string(target.nodeId) + " 的历史固件未签名";
                    unavailableDecision = true;
                }
            }
            std::int64_t                          taskId = 0;
            std::optional<Command::CommandRecord> command;
            bool                                  reused         = false;
            bool                                  createdCommand = false;
            if (reason.empty())
            {
                std::string rollbackFailure;
                try
                {
                    // 进程在命令已绑定、目标未回写之间重启时沿用原任务与命令，不再重复建单
                    taskId = co_await existingRollbackTask(loop, target.previousFirmwareId, aggregate.rollout.id, target.waveId, target.nodeId);
                    if (taskId > 0)
                    {
                        const auto task = co_await m_allFirmware->findTask(loop, taskId);
                        if (task.has_value() && task->commandId > 0)
                        {
                            auto existingCommand = co_await m_allCommands->find(loop, task->commandId);
                            if (existingCommand.has_value() && existingCommand->nodeId == target.nodeId &&
                                commandTargetsFirmware(existingCommand->payload, target.previousFirmwareId))
                            {
                                command = std::move(existingCommand);
                                reused  = true;
                            } else
                            {
                                reason = "节点 " + std::to_string(target.nodeId) + " 已有任务绑定了不同的回滚命令";
                            }
                        }
                    }
                    if (reason.empty() && taskId == 0)
                    {
                        taskId = co_await m_allFirmware->createTask(loop, target.previousFirmwareId, target.nodeId, aggregate.rollout.firmwareId, aggregate.rollout.id,
                                                                    target.waveId, 0);
                        if (taskId == 0)
                        {
                            taskId = co_await existingRollbackTask(loop, target.previousFirmwareId, aggregate.rollout.id, target.waveId, target.nodeId);
                        }
                    }
                    if (reason.empty() && taskId == 0)
                    {
                        reason              = "节点 " + std::to_string(target.nodeId) + " 无法创建回滚任务";
                        unavailableDecision = true;
                    } else if (reason.empty() && !command.has_value())
                    {
                        command = co_await m_allCommands->create(loop, target.nodeId, "firmware.update", downloadPayload(*previous, target.nodeId), aggregate.rollout.createdBy,
                                                                 m_commandTimeout);
                        if (!command.has_value())
                        {
                            static_cast<void>(co_await m_allFirmware->updateTaskById(loop, taskId, "failed", 0, "创建回滚命令失败"));
                            reason              = "节点 " + std::to_string(target.nodeId) + " 无法创建回滚命令";
                            unavailableDecision = true;
                        } else
                        {
                            createdCommand = true;
                            if (!co_await m_allFirmware->bindTaskCommand(loop, taskId, command->id))
                            {
                                static_cast<void>(co_await m_allCommands->cancel(loop, command->id));
                                static_cast<void>(co_await m_allFirmware->updateTaskById(loop, taskId, "failed", 0, "绑定回滚任务与命令失败"));
                                command.reset();
                                reason              = "节点 " + std::to_string(target.nodeId) + " 无法绑定回滚命令";
                                unavailableDecision = true;
                            }
                        }
                    }
                } catch (const std::exception &failure)
                {
                    rollbackFailure = failure.what();
                }
                if (!rollbackFailure.empty())
                {
                    if (taskId > 0)
                    {
                        static_cast<void>(co_await m_allFirmware->updateTaskById(loop, taskId, "failed", 0, "创建回滚命令失败"));
                    }
                    if (command.has_value())
                    {
                        static_cast<void>(co_await m_allCommands->cancel(loop, command->id));
                    }
                    command.reset();
                    reason              = "节点 " + std::to_string(target.nodeId) + " 创建回滚命令失败：" + truncateUtf8(rollbackFailure, 160);
                    unavailableDecision = true;
                }
            }
            if (!reason.empty())
            {
                // 无法回滚同样要落成目标的终局决定，避免下一拍反复重试或提前收尾
                FirmwareRolloutTargetUpdate decision;
                decision.status     = "failed";
                decision.healthJson = ConfigValue::object({{"gate", unavailableDecision ? "rollback_unavailable" : "rollback_failed"}, {"reason", reason}}).dump();
                static_cast<void>(co_await m_allRollouts->updateTarget(loop, target.id, decision, "dispatching"));
                if (unavailableDecision)
                {
                    unavailable.push_back(reason);
                }
                continue;
            }
            // 回滚命令与目标绑定也必须早于发布，崩溃后的重试会命中这条绑定
            FirmwareRolloutTargetUpdate binding;
            binding.status     = "rolling_back";
            binding.taskId     = taskId;
            binding.commandId  = command->id;
            binding.healthJson = ConfigValue::object({{"gate", "rollback_pending"}, {"toFirmwareId", target.previousFirmwareId}, {"commandId", command->id}}).dump();
            bool        bindFailed = false;
            std::string bindFailure;
            try
            {
                bindFailed = !co_await m_allRollouts->updateTarget(loop, target.id, binding, "dispatching");
            } catch (const std::exception &failure)
            {
                bindFailed  = true;
                bindFailure = failure.what();
            }
            if (bindFailed)
            {
                if (createdCommand)
                {
                    static_cast<void>(co_await m_allCommands->cancel(loop, command->id));
                }
                static_cast<void>(co_await m_allFirmware->updateTaskById(loop, taskId, "failed", 0, "回滚目标绑定失败"));
                FirmwareRolloutTargetUpdate restore;
                restore.status     = target.status;
                restore.taskId     = target.taskId;
                restore.commandId  = target.commandId;
                restore.healthJson = target.healthJson;
                static_cast<void>(co_await m_allRollouts->releaseTarget(loop, target.id, "dispatching", restore));
                if (!co_await rolloutStillReady(loop, aggregate.rollout.id, aggregate.rollout.version, static_cast<int>(waveIndex), {"rolling_back"}))
                {
                    co_return;
                }
                LOG_WARN_FMT("灰度发布 {} 目标 {} 回滚绑定失败：{}", aggregate.rollout.id, target.nodeId,
                             bindFailure.empty() ? std::string{"目标状态已变化"} : truncateUtf8(bindFailure, 200));
                continue;
            }
            if (!co_await rolloutStillReady(loop, aggregate.rollout.id, aggregate.rollout.version, static_cast<int>(waveIndex), {"rolling_back"}))
            {
                if (createdCommand)
                {
                    static_cast<void>(co_await m_allCommands->cancel(loop, command->id));
                }
                static_cast<void>(co_await m_allFirmware->updateTaskById(loop, taskId, "failed", 0, "回滚在派发期间暂停或中止"));
                FirmwareRolloutTargetUpdate restore;
                restore.status     = target.status;
                restore.taskId     = target.taskId;
                restore.commandId  = target.commandId;
                restore.healthJson = target.healthJson;
                static_cast<void>(co_await m_allRollouts->releaseTarget(loop, target.id, "rolling_back", restore));
                co_return;
            }
            if (createdCommand)
            {
                bool        publishFailed = false;
                std::string publishError;
                try
                {
                    co_await m_publisher(*command);
                } catch (const std::exception &failure)
                {
                    publishFailed = true;
                    publishError  = failure.what();
                }
                if (publishFailed)
                {
                    static_cast<void>(co_await m_allCommands->cancel(loop, command->id));
                    static_cast<void>(co_await m_allFirmware->updateTaskById(loop, taskId, "failed", 0, "发布回滚命令失败"));
                    LOG_WARN_FMT("灰度发布 {} 回滚命令 {} 发布失败：{}", aggregate.rollout.id, command->id, publishError);
                    FirmwareRolloutTargetUpdate decision;
                    decision.status     = "failed";
                    decision.healthJson = ConfigValue::object({{"gate", "rollback_failed"}, {"reason", "节点 " + std::to_string(target.nodeId) + " 发布回滚命令失败"}}).dump();
                    static_cast<void>(co_await m_allRollouts->updateTarget(loop, target.id, decision, "rolling_back"));
                    continue;
                }
                if (!co_await rolloutStillReady(loop, aggregate.rollout.id, aggregate.rollout.version, static_cast<int>(waveIndex), {"rolling_back"}))
                {
                    static_cast<void>(co_await m_allCommands->cancel(loop, command->id));
                    static_cast<void>(co_await m_allFirmware->updateTaskById(loop, taskId, "failed", 0, "回滚在发布后暂停或中止"));
                    const auto latest = co_await m_allRollouts->find(loop, aggregate.rollout.id);
                    if (latest.has_value() && latest->rollout.status == "paused")
                    {
                        // 暂停只停后续动作；已经送达的命令取消，目标恢复原状，恢复后重新派发回滚
                        FirmwareRolloutTargetUpdate restore;
                        restore.status     = target.status;
                        restore.taskId     = target.taskId;
                        restore.commandId  = target.commandId;
                        restore.healthJson = target.healthJson;
                        static_cast<void>(co_await m_allRollouts->releaseTarget(loop, target.id, "rolling_back", restore));
                    } else
                    {
                        FirmwareRolloutTargetUpdate decision;
                        decision.status     = "failed";
                        decision.healthJson = ConfigValue::object({{"gate", "rollback_failed"}, {"reason", "回滚命令送达后发布中止或状态变化"}}).dump();
                        static_cast<void>(co_await m_allRollouts->updateTarget(loop, target.id, decision, "rolling_back"));
                    }
                    co_return;
                }
            } else if (reused)
            {
                // 已绑定的回滚命令不再重发；Agent 轮询会接走仍为 pending 的命令
                static_cast<void>(reused);
            }
            ++issued;
        }
        if (issued > 0 || !unavailable.empty())
        {
            static_cast<void>(co_await m_allRollouts->appendGate(
                    loop, wave->id, ConfigValue::object({{"rollback", true}, {"issued", issued}, {"unavailable", unavailable.size()}, {"at", sqlTime(now)}}).dump()));
        }
        co_return;
    }

    AsynGyanis::Core::Task<> FirmwareRolloutService::evaluateRollback(AsynGyanis::Core::EventLoop &loop, const FirmwareRolloutAggregate aggregate,
                                                                      const FirmwareRolloutPolicy &policy, const TimePoint now)
    {
        const std::size_t                waveIndex = static_cast<std::size_t>(std::clamp(aggregate.rollout.currentWave, 0, static_cast<int>(aggregate.waves.size()) - 1));
        const FirmwareRolloutWaveRecord *wave      = waveAt(aggregate, waveIndex);
        if (wave == nullptr)
        {
            static_cast<void>(
                    co_await m_allRollouts->updateStatus(loop, aggregate.rollout.id, aggregate.rollout.version, "failed", aggregate.rollout.currentWave, "", "回滚波次不存在"));
            co_return;
        }
        const std::optional<TimePoint> deadline         = parseSqlTime(wave->deadlineAt);
        const TimePoint                rollbackEnd      = deadline.has_value() ? *deadline : addSeconds(now, policy.maxWaveSeconds);
        std::size_t                    confirmed        = 0;
        std::size_t                    skipped          = 0;
        std::size_t                    waiting          = 0;
        std::size_t                    undecided        = 0;
        std::size_t                    unavailableCount = 0;
        std::vector<std::string>       failures;
        std::vector<std::string>       waitingReasons;
        std::vector<std::string>       unavailableReasons;
        for (const FirmwareRolloutTargetRecord &target: aggregate.targets)
        {
            if (target.status == "rolled_back")
            {
                ++confirmed;
                continue;
            }
            if (target.status == "skipped")
            {
                ++skipped;
                continue;
            }
            if (target.status == "failed")
            {
                const std::string gate = healthField(target.healthJson, "gate");
                if (gate == "rollback_failed")
                {
                    failures.push_back("节点 " + std::to_string(target.nodeId) + " 回滚失败");
                    continue;
                }
                if (gate == "rollback_unavailable")
                {
                    ++unavailableCount;
                    const std::string reason = healthField(target.healthJson, "reason");
                    unavailableReasons.push_back(reason.empty() ? "节点 " + std::to_string(target.nodeId) + " 回滚不可用" : reason);
                    continue;
                }
                // 门控失败等安装阶段结论还没有回滚决定，不能当作已处理
                ++undecided;
                continue;
            }
            if (target.status != "rolling_back" || target.commandId <= 0 || target.taskId <= 0)
            {
                ++undecided;
                continue;
            }
            std::string failure;
            std::string pendingReason;
            const auto  command = co_await m_allCommands->find(loop, target.commandId);
            if (!command.has_value())
            {
                failure = "节点 " + std::to_string(target.nodeId) + " 回滚命令不存在";
            } else if (command->status == "failed" || command->status == "timeout" || command->status == "canceled")
            {
                failure = "节点 " + std::to_string(target.nodeId) + " 回滚命令状态为 " + command->status;
            } else if (command->status != "succeeded")
            {
                pendingReason = "回滚命令尚未完成";
            } else
            {
                const auto task = co_await m_allFirmware->findTask(loop, target.taskId);
                if (!task.has_value())
                {
                    failure = "节点 " + std::to_string(target.nodeId) + " 回滚任务不存在";
                } else if (task->commandId != target.commandId)
                {
                    failure = "节点 " + std::to_string(target.nodeId) + " 回滚任务与命令不匹配";
                } else if (task->status == "failed")
                {
                    failure = "节点 " + std::to_string(target.nodeId) + " 回滚任务失败";
                } else if (task->status != "installed")
                {
                    pendingReason = "回滚任务尚未确认安装";
                } else
                {
                    const auto previous = co_await m_allFirmware->find(loop, target.previousFirmwareId);
                    const auto node     = co_await m_allNodes->findById(loop, target.nodeId);
                    if (!previous.has_value() || previous->tenantId != aggregate.rollout.tenantId)
                    {
                        failure = "节点 " + std::to_string(target.nodeId) + " 回滚固件不存在";
                    } else if (!node.has_value() || node->tenantId != aggregate.rollout.tenantId)
                    {
                        failure = "节点 " + std::to_string(target.nodeId) + " 不存在或不属于发布租户";
                    } else if (node->installedFirmwareVersion.empty())
                    {
                        pendingReason = "节点尚未上报回滚后的固件版本";
                    } else if (node->installedFirmwareVersion != previous->version)
                    {
                        pendingReason = "节点 " + std::to_string(target.nodeId) + " 已安装版本 " + node->installedFirmwareVersion + " 与回滚目标 " + previous->version + " 不一致";
                    } else
                    {
                        FirmwareRolloutTargetUpdate update;
                        update.status     = "rolled_back";
                        update.healthJson = ConfigValue::object({{"gate", "rolled_back"}, {"version", previous->version}, {"commandId", target.commandId}}).dump();
                        static_cast<void>(co_await m_allRollouts->updateTarget(loop, target.id, update, "rolling_back"));
                        ++confirmed;
                        continue;
                    }
                }
            }
            if (!failure.empty())
            {
                FirmwareRolloutTargetUpdate update;
                update.status     = "failed";
                update.healthJson = ConfigValue::object({{"gate", "rollback_failed"}, {"reason", failure}}).dump();
                static_cast<void>(co_await m_allRollouts->updateTarget(loop, target.id, update, "rolling_back"));
                failures.push_back(failure);
                continue;
            }
            ++waiting;
            if (!pendingReason.empty())
            {
                waitingReasons.push_back(pendingReason);
            }
        }
        const bool atDeadline = now >= rollbackEnd;
        if (atDeadline && (waiting > 0 || undecided > 0))
        {
            // 期限一到先给每个未决目标落终局，再写发布头；不能在目标还悬着时提前收尾
            for (const FirmwareRolloutTargetRecord &target: aggregate.targets)
            {
                if (target.status == "rolled_back" || target.status == "skipped")
                {
                    continue;
                }
                if (target.status == "failed")
                {
                    const std::string gate = healthField(target.healthJson, "gate");
                    if (gate == "rollback_failed" || gate == "rollback_unavailable")
                    {
                        continue;
                    }
                }
                if (target.commandId <= 0)
                {
                    FirmwareRolloutTargetUpdate decision;
                    decision.status     = "skipped";
                    decision.healthJson = ConfigValue::object({{"gate", "rollback_skipped"}, {"reason", "未下发安装命令，无需回滚"}}).dump();
                    static_cast<void>(co_await m_allRollouts->updateTarget(loop, target.id, decision, target.status));
                    ++skipped;
                    continue;
                }
                FirmwareRolloutTargetUpdate decision;
                decision.status     = "failed";
                decision.healthJson = ConfigValue::object({{"gate", "rollback_failed"}, {"reason", "回滚未在期限内确认"}}).dump();
                static_cast<void>(co_await m_allRollouts->updateTarget(loop, target.id, decision, target.status));
                failures.push_back("节点 " + std::to_string(target.nodeId) + " 回滚未在期限内确认");
            }
            waiting   = 0;
            undecided = 0;
        }

        auto finishWaves = [&](const std::string &status) -> AsynGyanis::Core::Task<>
        {
            for (const FirmwareRolloutWaveRecord &row: aggregate.waves)
            {
                if (row.status == "rolling_back")
                {
                    static_cast<void>(co_await m_allRollouts->setWave(loop, row.id, status, {}, {}, sqlTime(now)));
                }
            }
            for (const FirmwareRolloutWaveRecord &row: aggregate.waves)
            {
                if (row.waveIndex > static_cast<int>(waveIndex) && row.status == "pending")
                {
                    static_cast<void>(co_await m_allRollouts->setWave(loop, row.id, "skipped", {}, {}, sqlTime(now)));
                }
            }
            co_return;
        };

        if (!failures.empty() && waiting == 0 && undecided == 0)
        {
            co_await finishWaves("failed");
            static_cast<void>(co_await m_allRollouts->updateStatus(loop, aggregate.rollout.id, aggregate.rollout.version, "failed", aggregate.rollout.currentWave, "",
                                                                   "回滚失败：" + joinReasons(failures, 400)));
            co_return;
        }
        if (unavailableCount > 0 && waiting == 0 && undecided == 0)
        {
            const std::string message = joinReasons(unavailableReasons, 400);
            co_await finishWaves("failed");
            if (confirmed > 0)
            {
                static_cast<void>(co_await m_allRollouts->updateStatus(loop, aggregate.rollout.id, aggregate.rollout.version, "failed", aggregate.rollout.currentWave, "",
                                                                       "部分目标无法回滚：" + message));
            } else
            {
                static_cast<void>(co_await m_allRollouts->updateStatus(loop, aggregate.rollout.id, aggregate.rollout.version, "rollback_unavailable",
                                                                       aggregate.rollout.currentWave, "", message));
            }
            co_return;
        }
        // 只有每个目标都已确认回滚、已跳过或已有明确的回滚终局决定才允许收尾
        if (waiting == 0 && undecided == 0 && failures.empty())
        {
            co_await finishWaves("rolled_back");
            static_cast<void>(co_await m_allRollouts->updateStatus(loop, aggregate.rollout.id, aggregate.rollout.version, "rolled_back", aggregate.rollout.currentWave));
            co_return;
        }
        co_return;
    }

    AsynGyanis::Core::Task<std::int64_t> FirmwareRolloutService::resolvePreviousFirmware(AsynGyanis::Core::EventLoop &loop, const FirmwareRecord &firmware,
                                                                                         const Device::NodeRecord &node)
    {
        // 心跳里的已安装版本是节点当前事实，优先据此解析；只有未上报时才回落到任务历史
        if (!node.installedFirmwareVersion.empty())
        {
            const auto byVersion = co_await m_allFirmware->scoped(System::TenantScope{firmware.tenantId, false})->findByVersion(loop, node.installedFirmwareVersion);
            if (byVersion.has_value())
            {
                co_return byVersion->id == firmware.id ? 0 : byVersion->id;
            }
        }
        co_return co_await m_allFirmware->latestInstalledFirmware(loop, node.id, firmware.id);
    }

    AsynGyanis::Core::Task<bool> FirmwareRolloutService::failTarget(AsynGyanis::Core::EventLoop &loop, const FirmwareRolloutTargetRecord &target, std::string reason,
                                                                    std::string expectedStatus)
    {
        if (target.taskId > 0)
        {
            static_cast<void>(co_await m_allFirmware->updateTaskById(loop, target.taskId, "failed", 0, reason));
        }
        FirmwareRolloutTargetUpdate update;
        update.status     = "failed";
        update.healthJson = ConfigValue::object({{"gate", "failed"}, {"reason", reason}}).dump();
        co_return co_await m_allRollouts->updateTarget(loop, target.id, update, expectedStatus);
    }

    AsynGyanis::Core::Task<bool> FirmwareRolloutService::rolloutStillReady(AsynGyanis::Core::EventLoop &loop, const std::int64_t rolloutId, const std::int64_t expectedVersion,
                                                                           const int waveIndex, const std::vector<std::string> &allowedStatuses)
    {
        const auto current = co_await m_allRollouts->find(loop, rolloutId);
        co_return current.has_value() && current->rollout.version == expectedVersion && current->rollout.currentWave == waveIndex &&
                containsStatus(allowedStatuses, current->rollout.status);
    }

    AsynGyanis::Core::Task<> FirmwareRolloutService::cancelInFlight(AsynGyanis::Core::EventLoop &loop, const FirmwareRolloutTargetRecord &target, std::string reason)
    {
        if (target.commandId > 0)
        {
            const auto command = co_await m_allCommands->find(loop, target.commandId);
            if (command.has_value() && commandActive(command->status))
            {
                static_cast<void>(co_await m_allCommands->cancel(loop, target.commandId));
            }
        }
        if (target.taskId > 0)
        {
            const auto task = co_await m_allFirmware->findTask(loop, target.taskId);
            if (task.has_value() && taskActive(task->status))
            {
                static_cast<void>(co_await m_allFirmware->updateTaskById(loop, target.taskId, "failed", 0, reason));
            }
        }
        co_return;
    }

    AsynGyanis::Core::Task<> FirmwareRolloutService::cancelTarget(AsynGyanis::Core::EventLoop &loop, const FirmwareRolloutTargetRecord &target, std::string reason)
    {
        co_await cancelInFlight(loop, target, reason);
        FirmwareRolloutTargetUpdate update;
        update.status     = "failed";
        update.healthJson = ConfigValue::object({{"gate", "failed"}, {"reason", reason}}).dump();
        static_cast<void>(co_await m_allRollouts->updateTarget(loop, target.id, update));
        co_return;
    }

    AsynGyanis::Core::Task<std::optional<FirmwareTaskRecord>> FirmwareRolloutService::existingTask(AsynGyanis::Core::EventLoop &loop, const std::int64_t firmwareId,
                                                                                                    const std::int64_t rolloutId, const std::int64_t waveId,
                                                                                                    const std::int64_t nodeId)
    {
        const std::vector<FirmwareTaskRecord> tasks = co_await m_allFirmware->tasks(loop, firmwareId);
        std::optional<FirmwareTaskRecord>     matched;
        for (const FirmwareTaskRecord &task: tasks)
        {
            if (task.rolloutId != rolloutId || task.waveId != waveId || task.nodeId != nodeId)
            {
                continue;
            }
            // 安装中或已安装的任务同样可能停在“命令已绑定、目标未回写”的重启窗口里
            if (task.status != "pending" && task.status != "downloading" && task.status != "installing" && task.status != "installed")
            {
                continue;
            }
            if (!matched.has_value() || task.id > matched->id)
            {
                matched = task;
            }
        }
        co_return matched;
    }

    AsynGyanis::Core::Task<std::int64_t> FirmwareRolloutService::existingRollbackTask(AsynGyanis::Core::EventLoop &loop, const std::int64_t firmwareId,
                                                                                      const std::int64_t rolloutId, const std::int64_t waveId, const std::int64_t nodeId)
    {
        const std::vector<FirmwareTaskRecord> tasks   = co_await m_allFirmware->tasks(loop, firmwareId);
        std::int64_t                          matched = 0;
        for (const FirmwareTaskRecord &task: tasks)
        {
            if (task.rolloutId != rolloutId || task.waveId != waveId || task.nodeId != nodeId || task.commandId <= 0)
            {
                continue;
            }
            // 安装中或刚安装完的任务都可能停在“命令已绑定、目标未回写”的重启窗口里
            if (task.status != "pending" && task.status != "downloading" && task.status != "installing" && task.status != "installed")
            {
                continue;
            }
            matched = std::max(matched, task.id);
        }
        co_return matched;
    }
} // namespace EdgePulse::Firmware
