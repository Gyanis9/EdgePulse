#include "Core/Cache/RedisProbe.h"

#include "Base/Log/LogMacros.h"
#include "Database/Redis/RedisConnection.h"

#include <exception>
#include <string>

namespace EdgePulse::Cache
{
    namespace
    {
        /**
         * @brief 取连接最近一次失败原因；驱动没给文本时用固定占位
         * @param connection Redis 连接
         * @return std::string 原因文本
         */
        [[nodiscard]] std::string failureReason(const AsynGyanis::Database::RedisConnection &connection)
        {
            std::string reason = connection.lastError();
            return reason.empty() ? std::string{"无错误详情"} : reason;
        }
    } // namespace

    bool probeRedisConnection(AsynGyanis::Database::RedisConnection &connection, const std::string_view endpoint)
    {
        try
        {
            if (!connection.isConnected() && !connection.connect())
            {
                // 端点只写 host:port；口令留在连接配置里，不进入日志
                LOG_WARN_FMT("缓存探针无法建立 Redis 连接：endpoint={} error={}", endpoint, failureReason(connection));
                return false;
            }
            if (connection.probeAlive())
            {
                return true;
            }
            LOG_WARN_FMT("缓存探针 PING 失败，尝试重连：endpoint={} error={}", endpoint, failureReason(connection));
            // 只在这里重连一次：PING 失败通常意味着对端已丢弃这条连接，isConnected 看不出来
            connection.disconnect();
            if (!connection.connect())
            {
                LOG_WARN_FMT("缓存探针重连失败：endpoint={} error={}", endpoint, failureReason(connection));
                return false;
            }
            if (connection.probeAlive())
            {
                LOG_INFO_FMT("缓存探针重连后恢复：endpoint={}", endpoint);
                return true;
            }
            LOG_WARN_FMT("缓存探针重连后 PING 仍失败：endpoint={} error={}", endpoint, failureReason(connection));
            return false;
        } catch (const std::exception &failure)
        {
            LOG_WARN_FMT("缓存探针异常：endpoint={} error={}", endpoint, failure.what());
            return false;
        }
    }
} // namespace EdgePulse::Cache
