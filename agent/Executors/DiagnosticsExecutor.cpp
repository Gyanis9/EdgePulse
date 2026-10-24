#include "Executors/DiagnosticsExecutor.h"

#include "AgentState.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Platform/FileSystem/FileSystem.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/EventLoop/EventLoop.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

namespace EdgePulse::Agent
{
    namespace
    {
        constexpr std::size_t kMaximumDiagnosticsBytes = 256U * 1024U; ///< 诊断结果正文上限

        /**
         * @brief 阻塞阶段收集到的原始材料
         */
        struct DiagnosticsInputs
        {
            std::string metricsJson; ///< 指标提供者返回的 JSON 文本
            std::string logTail;     ///< 日志尾文本
        };

        /**
         * @brief 丢弃非法 UTF-8 字节与截断的多字节序列
         * @param text 原始文本
         * @return std::string 可交给 JSON 序列化的文本
         */
        [[nodiscard]] std::string sanitizeUtf8(const std::string &text)
        {
            std::string sanitized;
            sanitized.reserve(text.size());
            std::size_t index = 0;
            while (index < text.size())
            {
                const auto lead = static_cast<unsigned char>(text[index]);
                if (lead < 0x80)
                {
                    sanitized.push_back(static_cast<char>(lead));
                    ++index;
                    continue;
                }
                std::size_t length = 0;
                if (lead >= 0xC2 && lead <= 0xDF)
                {
                    length = 2;
                } else if (lead >= 0xE0 && lead <= 0xEF)
                {
                    length = 3;
                } else if (lead >= 0xF0 && lead <= 0xF4)
                {
                    length = 4;
                }
                if (length == 0 || index + length > text.size())
                {
                    ++index;
                    continue;
                }
                bool valid = true;
                for (std::size_t offset = 1; offset < length; ++offset)
                {
                    const auto continuation = static_cast<unsigned char>(text[index + offset]);
                    if ((continuation & 0xC0U) != 0x80U)
                    {
                        valid = false;
                        break;
                    }
                }
                if (!valid)
                {
                    ++index;
                    continue;
                }
                sanitized.append(text, index, length);
                index += length;
            }
            return sanitized;
        }

        /**
         * @brief 读取日志文件末尾的一段
         * @param path 日志路径
         * @param maximumBytes 最多读取的字节数
         * @return 成功返回末尾文本；文件不可读时返回中文原因
         */
        [[nodiscard]] std::expected<std::string, std::string> readLogTail(const std::filesystem::path &path, const std::size_t maximumBytes)
        {
            if (path.empty() || maximumBytes == 0)
            {
                return std::string{};
            }
            std::ifstream stream(path, std::ios::binary);
            if (!stream.is_open())
            {
                return std::unexpected("无法打开日志文件 " + AsynGyanis::Platform::FileSystem::utf8FromPath(path));
            }
            stream.seekg(0, std::ios::end);
            const std::streamoff size = stream.tellg();
            if (size < 0)
            {
                return std::unexpected("无法读取日志文件大小 " + AsynGyanis::Platform::FileSystem::utf8FromPath(path));
            }
            const auto fileSize = static_cast<std::size_t>(size);
            const auto take     = std::min(fileSize, maximumBytes);
            stream.seekg(static_cast<std::streamoff>(fileSize - take), std::ios::beg);
            std::string tail(take, '\0');
            stream.read(tail.data(), static_cast<std::streamsize>(take));
            tail.resize(static_cast<std::size_t>(stream.gcount()));
            return sanitizeUtf8(tail);
        }

        /**
         * @brief 取状态库里的期望配置版本
         * @param state 本地状态库
         * @return std::int64_t 可解析的非负版本；缺失或格式不对时为 0
         */
        [[nodiscard]] std::int64_t desiredConfigVersion(AgentState &state)
        {
            const auto text = state.get("desired_config_version");
            if (!text.has_value() || text->empty())
            {
                return 0;
            }
            std::int64_t value  = 0;
            const auto   parsed = std::from_chars(text->data(), text->data() + text->size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size() || value < 0)
            {
                return 0;
            }
            return value;
        }
    } // namespace

    DiagnosticsExecutor::DiagnosticsExecutor(AgentState &state, AgentConfiguration configuration, std::string agentVersion, MetricsProvider metricsProvider) :
        m_state(state), m_configuration(std::move(configuration)), m_agentVersion(std::move(agentVersion)), m_metricsProvider(std::move(metricsProvider))
    {
    }

    bool DiagnosticsExecutor::supports(const std::string_view type) const
    {
        return type == "diagnostics.collect";
    }

    AsynGyanis::Core::Task<CommandResult> DiagnosticsExecutor::execute(AsynGyanis::Core::EventLoop &loop, const AgentCommand &command)
    {
        static_cast<void>(command);

        DiagnosticsInputs inputs;
        try
        {
            inputs = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<DiagnosticsInputs>(
                    loop,
                    [provider = m_metricsProvider, logFile = m_configuration.logFile, tailBytes = m_configuration.diagnosticsLogTailBytes]()
                    {
                        DiagnosticsInputs collected;
                        if (!provider)
                        {
                            LOG_WARN("未配置诊断指标提供者，metrics 返回空数组");
                        } else
                        {
                            try
                            {
                                collected.metricsJson = provider();
                            } catch (const std::exception &failure)
                            {
                                LOG_WARN_FMT("诊断指标采集抛出异常，metrics 返回空数组：{}", failure.what());
                            } catch (...)
                            {
                                LOG_WARN("诊断指标采集抛出未知异常，metrics 返回空数组");
                            }
                        }

                        const auto tail = readLogTail(logFile, tailBytes);
                        if (!tail.has_value())
                        {
                            LOG_WARN_FMT("{}；logTail 返回空文本", tail.error());
                        } else
                        {
                            collected.logTail = *tail;
                        }
                        return collected;
                    });
        } catch (const std::exception &failure)
        {
            co_return CommandResult{false, "{}", "诊断采集任务提交失败：" + std::string{failure.what()}};
        }

        AsynGyanis::Base::ConfigValue metrics = AsynGyanis::Base::ConfigValue::array();
        if (!inputs.metricsJson.empty())
        {
            const auto parsed = AsynGyanis::Base::parseConfigValue(inputs.metricsJson);
            if (parsed.has_value() && parsed->is_array())
            {
                metrics = *parsed;
            } else
            {
                LOG_WARN("诊断指标提供者返回的不是 JSON 数组，metrics 返回空数组");
            }
        }

        AsynGyanis::Base::ConfigValue diagnostics = AsynGyanis::Base::ConfigValue::object({
                {"agentVersion", m_agentVersion},
                {"nodeName", m_configuration.name},
                {"configVersion", desiredConfigVersion(m_state)},
                {"queueDepth", AsynGyanis::Base::ConfigValue::object({{"telemetry", m_state.telemetryQueueDepth()}, {"results", m_state.resultQueueDepth()}})},
                {"metrics", metrics},
                {"logTail", inputs.logTail},
        });

        auto serialized = AsynGyanis::Base::serializeConfigValue(diagnostics);
        while (serialized.has_value() && serialized->size() > kMaximumDiagnosticsBytes && !diagnostics["logTail"].get_ref<const std::string &>().empty())
        {
            std::string tail = diagnostics["logTail"].get<std::string>();
            if (tail.size() <= 1)
            {
                tail.clear();
            } else
            {
                tail.erase(0, tail.size() / 2);
            }
            diagnostics["logTail"] = std::move(tail);
            serialized             = AsynGyanis::Base::serializeConfigValue(diagnostics);
        }
        if (serialized.has_value() && serialized->size() > kMaximumDiagnosticsBytes)
        {
            LOG_WARN("诊断结果超过 256 KiB，metrics 已清空");
            diagnostics["metrics"] = AsynGyanis::Base::ConfigValue::array();
            serialized             = AsynGyanis::Base::serializeConfigValue(diagnostics);
        }
        if (!serialized.has_value() || serialized->size() > kMaximumDiagnosticsBytes)
        {
            co_return CommandResult{false, "{}", "诊断结果无法序列化为不超过 256 KiB 的 JSON"};
        }

        co_return CommandResult{true, *serialized, "诊断采集完成"};
    }
} // namespace EdgePulse::Agent
