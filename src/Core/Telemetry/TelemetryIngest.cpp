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
        outcome.nodeExists = (co_await nodes.findById(loop, nodeId)).has_value();
        if (!outcome.nodeExists)
        {
            co_return outcome;
        }
        outcome.stored = co_await telemetry.insertBatch(loop, nodeId, samples);
        if (!outcome.stored)
        {
            co_return outcome;
        }
        outcome.alarms = co_await alarms.evaluate(loop, nodeId, samples);
        co_return outcome;
    }
} // namespace EdgePulse::Telemetry
