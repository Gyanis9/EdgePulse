#include "Core/Http/ReadinessState.h"

#include "Base/Security/RandomToken.h"

#include <exception>
#include <fstream>
#include <system_error>

namespace EdgePulse::Web
{
    AsynGyanis::Net::ReadinessCheckResult evaluateReadiness(const ReadinessDependencies &dependencies)
    {
        if (!dependencies.databaseReady)
        {
            return {false, "数据库探针失败"};
        }
        if (!dependencies.serviceReady)
        {
            return {false, "服务正在停机或尚未就绪"};
        }
        if (!dependencies.cacheReady)
        {
            return {false, "缓存探针失败"};
        }
        if (!dependencies.realtimeHealthy)
        {
            return {false, "实时事件总线不可用"};
        }
        if (!dependencies.firmwareDirectoryReady)
        {
            return {false, "固件目录不可写"};
        }
        return {true, ""};
    }

    bool probeDirectoryWritable(const std::filesystem::path &directory)
    {
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error) || error)
        {
            return false;
        }
        std::filesystem::path probePath;
        try
        {
            probePath = directory / (".edgepulse-readyz-" + Common::randomTokenHex(8) + ".tmp");
        } catch (const std::exception &)
        {
            return false;
        }
        {
            std::ofstream probe(probePath, std::ios::binary | std::ios::trunc);
            if (!probe)
            {
                return false;
            }
            probe << "readyz";
            probe.close();
            if (!probe)
            {
                std::filesystem::remove(probePath, error);
                return false;
            }
        }
        std::filesystem::remove(probePath, error);
        return true;
    }

    void ReadinessState::setDatabaseReady(const bool ready) noexcept
    {
        m_databaseReady.store(ready, std::memory_order_release);
    }

    void ReadinessState::setServiceReady(const bool ready) noexcept
    {
        m_serviceReady.store(ready, std::memory_order_release);
    }

    void ReadinessState::setCacheReady(const bool ready) noexcept
    {
        m_cacheReady.store(ready, std::memory_order_release);
    }

    void ReadinessState::setRealtimeHealthy(const bool healthy) noexcept
    {
        m_realtimeHealthy.store(healthy, std::memory_order_release);
    }

    void ReadinessState::setFirmwareDirectoryReady(const bool ready) noexcept
    {
        m_firmwareDirectoryReady.store(ready, std::memory_order_release);
    }

    ReadinessDependencies ReadinessState::snapshot() const noexcept
    {
        ReadinessDependencies dependencies;
        dependencies.databaseReady          = m_databaseReady.load(std::memory_order_acquire);
        dependencies.serviceReady           = m_serviceReady.load(std::memory_order_acquire);
        dependencies.cacheReady             = m_cacheReady.load(std::memory_order_acquire);
        dependencies.realtimeHealthy        = m_realtimeHealthy.load(std::memory_order_acquire);
        dependencies.firmwareDirectoryReady = m_firmwareDirectoryReady.load(std::memory_order_acquire);
        return dependencies;
    }

    AsynGyanis::Net::ReadinessCheckResult ReadinessState::check() const
    {
        return evaluateReadiness(snapshot());
    }
} // namespace EdgePulse::Web
