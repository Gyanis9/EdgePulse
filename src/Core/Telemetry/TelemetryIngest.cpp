#include "Core/Telemetry/TelemetryIngest.h"

#include "Core/Alarm/AlarmService.h"
#include "Core/Device/NodeRepository.h"
#include "Core/Telemetry/TelemetryRepository.h"

#include <utility>

namespace EdgePulse::Telemetry
{
    AsynGyanis::Core::Task<TelemetryIngestOutcome> ingestTelemetry(AsynGyanis::Core::EventLoop &loop, Device::NodeRepository &nodes, TelemetryRepository &telemetry,
                                                                   Alarm::AlarmService &alarms, const std::int64_t nodeId, std::vector<TelemetrySample> samples)
    {
        TelemetryIngestOutcome outcome;
        const auto             node = co_await nodes.scoped({1, true})->findById(loop, nodeId);
        outcome.nodeExists          = node.has_value();
        if (!outcome.nodeExists)
        {
            co_return outcome;
        }
        const System::TenantScope tenant{node->tenantId, false};
        outcome.stored = co_await telemetry.scoped(tenant)->insertBatch(loop, nodeId, samples);
        if (!outcome.stored)
        {
            co_return outcome;
        }
        outcome.alarms = co_await alarms.scoped(tenant)->evaluate(loop, nodeId, samples);
        co_return outcome;
    }
} // namespace EdgePulse::Telemetry
