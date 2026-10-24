#include "Executors/TelemetryPublisher.h"

#include "AgentClient.h"
#include "AgentState.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/EventLoop/EventLoop.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
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

        /**
         * @brief 一个已分拣好的补报批次
         */
        struct FlushBatch
        {
            AsynGyanis::Base::ConfigValue payloads = AsynGyanis::Base::ConfigValue::array(); ///< 可上报的样本数组
            std::vector<std::int64_t>     ids;                                               ///< 与 payloads 一一对应的队列行
            std::size_t                   takenRows{0};                                      ///< 本次从队列取出的行数
            bool                          corruptCleanupFailed{false};                       ///< 损坏行删除失败，本轮补报必须停止
        };

        /**
         * @brief 从队列取一批行并按可上报性分拣，损坏行当场批量删除
         * @param state 本地状态库
         * @return FlushBatch 可上报样本、对应行标识与损坏行清理结论
         * @note 只应在阻塞执行器的工作线程上调用
         */
        [[nodiscard]] FlushBatch takeFlushBatch(AgentState &state)
        {
            const std::vector<QueuedItem> rows = state.takeTelemetry(kTelemetryBatchLimit);
            FlushBatch                    taken;
            taken.takenRows = rows.size();

            std::vector<QueuedItem>   corruptRows;
            std::vector<std::int64_t> corruptIds;
            corruptRows.reserve(rows.size());
            corruptIds.reserve(rows.size());
            for (const QueuedItem &row: rows)
            {
                auto parsed = AsynGyanis::Base::parseConfigValue(row.payload);
                if (!parsed.has_value() || !isValidQueuedSample(*parsed))
                {
                    corruptRows.push_back(row);
                    corruptIds.push_back(row.id);
                    continue;
                }
                taken.payloads.push_back(std::move(*parsed));
                taken.ids.push_back(row.id);
            }

            if (!corruptIds.empty())
            {
                const auto dropped = state.ackTelemetryBatch(corruptIds);
                if (!dropped)
                {
                    LOG_ERROR_FMT("遥测队列中 {} 条损坏行删除失败，本轮补报停止：{}", corruptIds.size(), dropped.error());
                    taken.corruptCleanupFailed = true;
                    return taken;
                }
                for (const QueuedItem &row: corruptRows)
                {
                    LOG_ERROR_FMT("遥测队列第 {} 条正文已损坏，已从队列删除：{}", row.id, row.payload);
                }
            }
            return taken;
        }

        /**
         * @brief 在一个阻塞任务里批量确认已送达的队列行
         * @param state 本地状态库
         * @param loop 恢复协程用的事件循环
         * @param ids 待删除的队列行
         * @return 成功为空；任务提交失败或删除失败时返回中文原因
         */
        [[nodiscard]] AsynGyanis::Core::Task<std::expected<void, std::string>> ackTelemetryRows(AgentState &state, AsynGyanis::Core::EventLoop &loop, std::vector<std::int64_t> ids)
        {
            try
            {
                std::expected<void, std::string> acknowledged = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::expected<void, std::string>>(
                        loop, [state = &state, ids = std::move(ids)]() { return state->ackTelemetryBatch(ids); });
                co_return acknowledged;
            } catch (const std::exception &failure)
            {
                co_return std::unexpected("遥测确认任务失败：" + std::string{failure.what()});
            } catch (...)
            {
                co_return std::unexpected("遥测确认任务失败：未知异常");
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

    AsynGyanis::Core::Task<> TelemetryPublisher::flush(AsynGyanis::Core::EventLoop &loop, const std::int64_t nodeId)
    {
        if (nodeId <= 0)
        {
            LOG_ERROR("节点标识无效，暂停遥测补报");
            co_return;
        }

        for (;;)
        {
            FlushBatch taken;
            try
            {
                taken = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<FlushBatch>(loop, [state = &m_state] { return takeFlushBatch(*state); });
            } catch (const std::exception &failure)
            {
                LOG_ERROR_FMT("遥测补报的队列读取任务失败，本轮停止：{}", failure.what());
                co_return;
            } catch (...)
            {
                LOG_ERROR("遥测补报的队列读取任务失败：未知异常，本轮停止");
                co_return;
            }
            if (taken.corruptCleanupFailed)
            {
                co_return;
            }
            if (taken.takenRows == 0)
            {
                co_return;
            }
            if (taken.ids.empty())
            {
                // 整批都是损坏行且已删除，接着取下一批
                continue;
            }

            const std::size_t batchSize = taken.ids.size();
            const auto        sent      = co_await m_client.sendTelemetry(nodeId, taken.payloads);
            if (sent.has_value())
            {
                const auto acknowledged = co_await ackTelemetryRows(m_state, loop, std::move(taken.ids));
                if (!acknowledged)
                {
                    LOG_ERROR_FMT("遥测补报已送达，但 {} 条队列行确认失败，下一轮会重复上报：{}", batchSize, acknowledged.error());
                    co_return;
                }
                continue;
            }

            const TelemetryFailure &failure = sent.error();
            if (failure.retryable)
            {
                LOG_WARN_FMT("遥测补报失败，{} 条样本保留在离线队列等待重试：{}", batchSize, failure.message);
                co_return;
            }
            LOG_ERROR_FMT("遥测补报被服务端以 HTTP {} 拒绝，重试不会改变结论，已删除 {} 条队列样本：{}", failure.httpStatus, batchSize, failure.message);
            const auto acknowledged = co_await ackTelemetryRows(m_state, loop, std::move(taken.ids));
            if (!acknowledged)
            {
                LOG_ERROR_FMT("被终态拒绝的 {} 条队列行删除失败，下一轮会再次补报：{}", batchSize, acknowledged.error());
                co_return;
            }
        }
    }
} // namespace EdgePulse::Agent
