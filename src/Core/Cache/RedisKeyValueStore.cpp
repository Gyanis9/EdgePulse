#include "Core/Cache/RedisKeyValueStore.h"

#include "Base/Exception/EdgePulseException.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Redis/RedisConnection.h"

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Cache
{
    RedisKeyValueStore::RedisKeyValueStore(std::string host, const std::uint16_t port, std::string password, const int database) :
        m_host(std::move(host)), m_port(port), m_password(std::move(password)), m_database(database)
    {
        AsynGyanis::Database::ConnectionConfig configuration = AsynGyanis::Database::ConnectionConfig::redisDefault();
        configuration.host                                   = m_host;
        configuration.port                                   = m_port;
        configuration.password                               = m_password;
        configuration.database                               = std::to_string(m_database);
        m_connection                                         = std::make_unique<AsynGyanis::Database::RedisConnection>(configuration);
        if (!m_connection->connect())
        {
            throw Common::DatabaseException("连接 Redis 失败：" + m_host + ":" + std::to_string(m_port) + "：" + m_connection->lastError());
        }
    }

    RedisKeyValueStore::~RedisKeyValueStore() = default;

    std::unique_ptr<AsynGyanis::Database::DatabaseResult> RedisKeyValueStore::executeLocked(const std::string_view command, const std::vector<std::string> &arguments)
    {
        if (!m_connection || !m_connection->isConnected())
        {
            throw Common::DatabaseException("Redis 连接不可用，缓存操作未执行");
        }
        std::vector<AsynGyanis::Database::DatabaseValue> parameters;
        parameters.reserve(arguments.size());
        for (const std::string &argument: arguments)
        {
            parameters.emplace_back(argument);
        }
        std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = m_connection->execute(command, std::span<const AsynGyanis::Database::DatabaseValue>(parameters));
        if (!result)
        {
            throw Common::DatabaseException("Redis 命令 " + std::string{command} + " 失败：" + m_connection->lastError());
        }
        return result;
    }

    AsynGyanis::Core::Task<std::optional<std::string>> RedisKeyValueStore::get(AsynGyanis::Core::EventLoop &loop, std::string key)
    {
        co_return co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::optional<std::string>>(
                loop, std::function<std::optional<std::string>()>(
                              [this, key = std::move(key)]() -> std::optional<std::string>
                              {
                                  std::lock_guard<std::mutex> lock(m_mutex);
                                  const auto                  result = executeLocked("GET", {key});
                                  if (!result->next())
                                  {
                                      return std::nullopt;
                                  }
                                  const AsynGyanis::Database::DatabaseValue value = result->getValue(0);
                                  if (const auto *text = std::get_if<std::string>(&value); text != nullptr)
                                  {
                                      return *text;
                                  }
                                  return std::nullopt;
                              }));
    }

    AsynGyanis::Core::Task<bool> RedisKeyValueStore::set(AsynGyanis::Core::EventLoop &loop, std::string key, std::string value, const std::chrono::seconds ttl)
    {
        co_return co_await AsynGyanis::Core::AsyncExecutor::shared().submit<bool>(loop, std::function<bool()>(
                                                                                                [this, key = std::move(key), value = std::move(value), ttl]() -> bool
                                                                                                {
                                                                                                    if (ttl.count() <= 0)
                                                                                                    {
                                                                                                        throw Common::DatabaseException("Redis 写入的过期时间必须是正数");
                                                                                                    }
                                                                                                    std::lock_guard<std::mutex> lock(m_mutex);
                                                                                                    static_cast<void>(
                                                                                                            executeLocked("SET", {key, value, "EX", std::to_string(ttl.count())}));
                                                                                                    return true;
                                                                                                }));
    }

    AsynGyanis::Core::Task<bool> RedisKeyValueStore::erase(AsynGyanis::Core::EventLoop &loop, std::string key)
    {
        co_return co_await AsynGyanis::Core::AsyncExecutor::shared().submit<bool>(loop, std::function<bool()>(
                                                                                                [this, key = std::move(key)]() -> bool
                                                                                                {
                                                                                                    std::lock_guard<std::mutex> lock(m_mutex);
                                                                                                    static_cast<void>(executeLocked("DEL", {key}));
                                                                                                    return true;
                                                                                                }));
    }

    AsynGyanis::Core::Task<std::int64_t> RedisKeyValueStore::increment(AsynGyanis::Core::EventLoop &loop, std::string key, const std::chrono::seconds ttl)
    {
        co_return co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::int64_t>(
                loop, std::function<std::int64_t()>(
                              [this, key = std::move(key), ttl]() -> std::int64_t
                              {
                                  if (ttl.count() <= 0)
                                  {
                                      throw Common::DatabaseException("Redis 计数的过期时间必须是正数");
                                  }
                                  std::lock_guard<std::mutex> lock(m_mutex);
                                  const auto                  increaseResult = executeLocked("INCR", {key});
                                  if (!increaseResult->next())
                                  {
                                      throw Common::DatabaseException("Redis INCR 没有返回计数");
                                  }
                                  const AsynGyanis::Database::DatabaseValue value  = increaseResult->getValue(0);
                                  const auto                               *number = std::get_if<std::int64_t>(&value);
                                  if (number == nullptr)
                                  {
                                      throw Common::DatabaseException("Redis INCR 的返回值不是整数");
                                  }
                                  // 只在键刚创建时设置过期：固定窗口的截止时刻不能被后续自增推后
                                  if (*number == 1)
                                  {
                                      static_cast<void>(executeLocked("EXPIRE", {key, std::to_string(ttl.count())}));
                                  }
                                  return *number;
                              }));
    }

    std::string_view RedisKeyValueStore::backendName() const noexcept
    {
        return "redis";
    }
} // namespace EdgePulse::Cache
