#include "AgentApplication.h"

#include "Platform/Platform.h"

#include "Base/Log/LogLevel.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/ConsoleSink.h"
#include "Base/Log/Sinks/RollingFileSink.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace EdgePulse::Agent
{
    namespace
    {
        /**
         * @brief 确保目录存在
         * @param directory 目标目录
         * @param error 失败时写入中文原因
         * @return bool 创建成功或已存在返回 true
         */
        [[nodiscard]] bool ensureDirectory(const std::filesystem::path &directory, std::string &error)
        {
            std::error_code filesystemError;
            std::filesystem::create_directories(directory, filesystemError);
            if (filesystemError)
            {
                error = "无法创建目录 " + directory.string() + "：" + filesystemError.message();
                return false;
            }
            return true;
        }

        /**
         * @brief 装配根日志器：控制台加按大小滚动的文件输出
         * @param configuration 已校验的配置
         * @throws std::exception 文件 Sink 打不开日志路径时由构造抛出，交给 run() 的边界捕获并拒绝启动
         * @details Agent 配置里的日志段是部署形态（文件、等级、大小与备份份数），与引擎日志加载器
         *          要求的树形段落不同，因此直接复用引擎的 Sink 与等级工具完成装配。
         */
        void installLogging(const AgentConfiguration &configuration)
        {
            auto &rootLogger = AsynGyanis::Base::LoggerRegistry::instance().getRootLogger();
            rootLogger.setLevel(AsynGyanis::Base::logLevelFromString(configuration.logLevel));
            rootLogger.addSink(std::make_shared<AsynGyanis::Base::ConsoleSink>(false));

            std::filesystem::path logDirectory = configuration.logFile.parent_path();
            if (logDirectory.empty())
            {
                logDirectory = std::filesystem::current_path();
            }
            const auto maximumSizeBytes = static_cast<std::size_t>(configuration.logMaxSizeMb) * 1024U * 1024U;
            rootLogger.addSink(std::make_shared<AsynGyanis::Base::RollingFileSink>(
                    configuration.logFile.filename(), logDirectory, AsynGyanis::Base::RollingPolicy::Size, maximumSizeBytes,
                    static_cast<std::size_t>(configuration.logMaxBackup), true));
        }
    } // namespace

    AgentApplication::AgentApplication(AgentConfiguration configuration) : m_configuration(std::move(configuration))
    {
    }

    int AgentApplication::run()
    {
        try
        {
            std::string error;
            if (!ensureDirectory(m_configuration.stateDirectory, error))
            {
                std::cerr << error << '\n';
                return 1;
            }
            if (!m_configuration.logFile.parent_path().empty() && !ensureDirectory(m_configuration.logFile.parent_path(), error))
            {
                std::cerr << error << '\n';
                return 1;
            }

            installLogging(m_configuration);
            LOG_INFO_FMT("Agent 已启动，节点名 {}", m_configuration.name);
            return 0;
        } catch (const std::exception &failure)
        {
            std::cerr << "Agent 启动失败：" << failure.what() << '\n';
            return 1;
        }
    }
} // namespace EdgePulse::Agent
