#include "Executors/TelemetryPublisher.h"

#include "AgentClient.h"
#include "AgentState.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Agent
{
    namespace
    {
        /**
         * @brief 判断队列里的 JSON 是否仍是一条可上报的样本
         * @param value 已解析的队列正文
         * @return true 表示 metric、value、unit 与 collectedAt 都满足服务端约束
         */
        [[nodiscard]] bool isValidQueuedSample(const AsynGyanis::Base::ConfigValue &value)
        {
            if (!value.is_object())
            {
                return false;
            }
            const auto metric = value.find("metric");
            if (metric == value.end() || !metric->is_string())
            {
                return false;
            }
            const std::string &metricName = metric->get_ref<const std::string &>();
            if (metricName.empty() || metricName.size() > kMaximumMetricNameLength)
            {
                return false;
            }
            const auto number = value.find("value");
            if (number == value.end() || !number->is_number() || !std::isfinite(number->get<double>()))
            {
                return false;
            }
            const auto unit = value.find("unit");
            if (unit == value.end() || !unit->is_string())
            {
                return false;
            }
            const auto collectedAt = value.find("collectedAt");
            return collectedAt != value.end() && collectedAt->is_string();
        }

        /**
         * @brief 把已解析的样本逐条写进离线队列
         * @param state 本地状态库
         * @param samples 已达服务端格式的样本对象数组
         */
        void enqueueSamples(AgentState &state, const AsynGyanis::Base::ConfigValue &samples)
        {
            for (const AsynGyanis::Base::ConfigValue &sample: samples)
            {
                state.enqueueTelemetry(sample.dump());
            }
        }
    } // namespace

    TelemetryPublisher::TelemetryPublisher(AgentClient &client, AgentState &state) : m_client(client), m_state(state)
    {
    }

    AsynGyanis::Core::Task<> TelemetryPublisher::publish(const std::int64_t nodeId, const std::vector<TelemetrySample> &samples)
    {
        if (nodeId <= 0)
        {
            LOG_ERROR("节点标识无效，跳过本轮遥测上报");
            co_return;
        }
        if (samples.empty())
        {
            co_return;
        }

        AsynGyanis::Base::ConfigValue batch = AsynGyanis::Base::ConfigValue::array();
        for (const TelemetrySample &sample: samples)
        {
            const auto payload = serializeTelemetrySample(sample);
            if (!payload.has_value())
            {
                LOG_ERROR_FMT("遥测样本 {} 的指标名或数值不合法，已跳过该样本", sample.metric);
                continue;
            }
            auto parsed = AsynGyanis::Base::parseConfigValue(*payload);
            if (!parsed.has_value())
            {
                LOG_ERROR_FMT("遥测样本 {} 序列化结果无法解析，已跳过该样本", sample.metric);
                continue;
            }
            batch.push_back(std::move(*parsed));
        }
        if (batch.empty())
        {
            co_return;
        }

        const auto sent = co_await m_client.sendTelemetry(nodeId, batch);
        if (sent.has_value())
        {
            co_return;
        }
        const TelemetryFailure &failure = sent.error();
        const auto              count   = static_cast<std::size_t>(batch.size());
        if (!failure.retryable)
        {
            LOG_ERROR_FMT("遥测批次被服务端以 HTTP {} 拒绝，重试不会改变结论，已丢弃 {} 条样本：{}", failure.httpStatus, count, failure.message);
            co_return;
        }
        enqueueSamples(m_state, batch);
        LOG_WARN_FMT("遥测上报失败，已把 {} 条样本写入离线队列等待补报：{}", count, failure.message);
    }

    AsynGyanis::Core::Task<> TelemetryPublisher::flush(const std::int64_t nodeId)
    {
        if (nodeId <= 0)
        {
            LOG_ERROR("节点标识无效，暂停遥测补报");
            co_return;
        }

        for (;;)
        {
            const std::vector<QueuedItem> rows = m_state.takeTelemetry(kTelemetryBatchLimit);
            if (rows.empty())
            {
                co_return;
            }

            AsynGyanis::Base::ConfigValue batch = AsynGyanis::Base::ConfigValue::array();
            std::vector<QueuedItem>       deliverable;
            deliverable.reserve(rows.size());
            for (const QueuedItem &row: rows)
            {
                const auto parsed = AsynGyanis::Base::parseConfigValue(row.payload);
                if (!parsed.has_value() || !isValidQueuedSample(*parsed))
                {
                    LOG_ERROR_FMT("遥测队列第 {} 条正文已损坏，已从队列删除：{}", row.id, row.payload);
                    m_state.ackTelemetry(row.id);
                    continue;
                }
                batch.push_back(*parsed);
                deliverable.push_back(row);
            }
            if (deliverable.empty())
            {
                continue;
            }

            const auto sent = co_await m_client.sendTelemetry(nodeId, batch);
            if (sent.has_value())
            {
                for (const QueuedItem &row: deliverable)
                {
                    m_state.ackTelemetry(row.id);
                }
                continue;
            }

            const TelemetryFailure &failure = sent.error();
            if (failure.retryable)
            {
                LOG_WARN_FMT("遥测补报失败，{} 条样本保留在离线队列等待重试：{}", deliverable.size(), failure.message);
                co_return;
            }
            LOG_ERROR_FMT("遥测补报被服务端以 HTTP {} 拒绝，重试不会改变结论，已删除 {} 条队列样本：{}", failure.httpStatus, deliverable.size(), failure.message);
            for (const QueuedItem &row: deliverable)
            {
                m_state.ackTelemetry(row.id);
            }
        }
    }
} // namespace EdgePulse::Agent
