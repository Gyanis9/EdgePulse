#include "Core/Auth/SessionStore.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Exception/EdgePulseException.h"
#include "Base/Security/RandomToken.h"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Auth
{
    namespace
    {
        constexpr std::string_view kSessionPrefix = "edgepulse:session:";

        [[nodiscard]] std::string keyOf(const std::string_view token)
        {
            return std::string{kSessionPrefix} + std::string{token};
        }

        [[nodiscard]] std::string serialize(const SessionRecord &record)
        {
            AsynGyanis::Base::ConfigValue roles       = AsynGyanis::Base::ConfigValue::array();
            AsynGyanis::Base::ConfigValue permissions = AsynGyanis::Base::ConfigValue::array();
            for (const std::string &role: record.roles)
            {
                roles.push_back(role);
            }
            for (const std::string &permission: record.permissions)
            {
                permissions.push_back(permission);
            }
            return AsynGyanis::Base::ConfigValue::object({{"userId", record.userId},
                                                          {"username", record.username},
                                                          {"nickname", record.nickname},
                                                          {"departmentId", record.departmentId},
                                                          {"departmentName", record.departmentName},
                                                          {"roles", std::move(roles)},
                                                          {"permissions", std::move(permissions)}})
                    .dump();
        }

        [[nodiscard]] std::optional<SessionRecord> deserialize(const std::string_view text, const std::string_view token)
        {
            const std::optional<AsynGyanis::Base::ConfigValue> value = AsynGyanis::Base::parseConfigValue(text);
            if (!value.has_value() || !value->is_object())
            {
                return std::nullopt;
            }
            SessionRecord record;
            record.token          = std::string{token};
            record.userId         = value->value("userId", std::int64_t{0});
            record.username       = value->value("username", std::string{});
            record.nickname       = value->value("nickname", std::string{});
            record.departmentId   = value->value("departmentId", std::int64_t{0});
            record.departmentName = value->value("departmentName", std::string{});
            if (const auto roles = value->find("roles"); roles != value->end() && roles->is_array())
            {
                for (const AsynGyanis::Base::ConfigValue &role: *roles)
                {
                    if (role.is_string())
                    {
                        record.roles.push_back(role.get<std::string>());
                    }
                }
            }
            if (const auto permissions = value->find("permissions"); permissions != value->end() && permissions->is_array())
            {
                for (const AsynGyanis::Base::ConfigValue &permission: *permissions)
                {
                    if (permission.is_string())
                    {
                        record.permissions.push_back(permission.get<std::string>());
                    }
                }
            }
            // 身份快照缺关键字段时当作损坏，按未登录处理
            if (record.userId <= 0 || record.username.empty())
            {
                return std::nullopt;
            }
            return record;
        }
    } // namespace

    SessionStore::SessionStore(std::shared_ptr<Cache::KeyValueStore> store, const int ttlSeconds) : m_store(std::move(store)), m_ttlSeconds(ttlSeconds)
    {
    }

    AsynGyanis::Core::Task<SessionRecord> SessionStore::create(AsynGyanis::Core::EventLoop &loop, SessionRecord record)
    {
        record.token      = Common::randomTokenHex(32);
        const bool stored = co_await m_store->set(loop, keyOf(record.token), serialize(record), std::chrono::seconds{ttlSeconds()});
        if (!stored)
        {
            throw Common::EdgePulseException("会话写入失败：请检查会话存储是否可用");
        }
        co_return record;
    }

    AsynGyanis::Core::Task<std::optional<SessionRecord>> SessionStore::validateAndTouch(AsynGyanis::Core::EventLoop &loop, const std::string_view token)
    {
        if (token.empty())
        {
            co_return std::nullopt;
        }
        const std::optional<std::string> stored = co_await m_store->get(loop, keyOf(token));
        if (!stored.has_value())
        {
            co_return std::nullopt;
        }
        std::optional<SessionRecord> record = deserialize(*stored, token);
        if (!record.has_value())
        {
            static_cast<void>(co_await m_store->erase(loop, keyOf(token)));
            co_return std::nullopt;
        }
        // 滑动续期：每次有效访问都把截止时刻推到从现在起算
        const bool touched = co_await m_store->set(loop, keyOf(token), serialize(*record), std::chrono::seconds{ttlSeconds()});
        if (!touched)
        {
            co_return std::nullopt;
        }
        co_return record;
    }

    AsynGyanis::Core::Task<bool> SessionStore::remove(AsynGyanis::Core::EventLoop &loop, const std::string_view token)
    {
        if (token.empty())
        {
            co_return false;
        }
        co_return co_await m_store->erase(loop, keyOf(token));
    }

    int SessionStore::ttlSeconds() const noexcept
    {
        return m_ttlSeconds.load(std::memory_order_relaxed);
    }

    void SessionStore::setTtlSeconds(const int ttlSeconds) noexcept
    {
        if (ttlSeconds > 0)
        {
            m_ttlSeconds.store(ttlSeconds, std::memory_order_relaxed);
        }
    }
} // namespace EdgePulse::Auth
