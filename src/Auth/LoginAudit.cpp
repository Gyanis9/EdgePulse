#include "Auth/LoginAudit.h"

#include "Common/IdGenerator.h"
#include "Common/TimeFormat.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"

#include <memory>
#include <array>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>

namespace EdgePulse::Auth
{
    LoginAudit::LoginAudit(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<bool> LoginAudit::record(AsynGyanis::Core::EventLoop &loop, LoginAuditEntry entry)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [entry = std::move(entry)](AsynGyanis::Database::DatabaseConnection &connection) -> bool
                {
                    const std::int64_t                                          status = entry.success ? 1 : 0;
                    const std::array<AsynGyanis::Database::DatabaseValue, 7>    parameters{Common::nextIdentifier(), entry.username, entry.ip, status, entry.message, entry.traceId,
                                                                                           Common::nowIso8601Utc()};
                    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result =
                            connection.execute("INSERT INTO sys_login_log (id, username, ip, status, message, trace_id, login_time) VALUES (?, ?, ?, ?, ?, ?, ?)", parameters);
                    // 审计写不进去是数据库故障，不把“没写”伪装成成功
                    if (!result)
                    {
                        throw Common::DatabaseException("写登录审计失败：" + connection.lastError());
                    }
                    return true;
                });
    }
} // namespace EdgePulse::Auth
