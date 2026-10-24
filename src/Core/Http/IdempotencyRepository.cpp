#include "Core/Http/IdempotencyRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Exception/EdgePulseException.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace EdgePulse::Web
{
    namespace
    {
        /**
         * @brief 回读已有占位所需的列
         */
        constexpr const char *kExistingColumns = "request_hash, status, response_status, response_trace_id, response_content_type, response_body";

        /**
         * @brief 把回读的已完成行转成重放结果
         * @param result 指向当前行
         * @return IdempotencyReplay 状态码与响应内容
         */
        [[nodiscard]] IdempotencyReplay readReplay(const AsynGyanis::Database::DatabaseResult &result)
        {
            IdempotencyReplay replay;
            replay.status      = static_cast<int>(Sql::integer(result, "response_status"));
            replay.traceId     = Sql::text(result, "response_trace_id");
            replay.contentType = Sql::text(result, "response_content_type");
            replay.body        = Sql::text(result, "response_body");
            return replay;
        }
    } // namespace

    IdempotencyRepository::IdempotencyRepository(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<IdempotencyBeginResult> IdempotencyRepository::begin(AsynGyanis::Core::EventLoop &loop, std::string tokenHash, std::string keyHash,
                                                                                std::string requestHash, std::string method, std::string path, std::string expiresAt)
    {
        co_return co_await m_database->withConnection<IdempotencyBeginResult>(
                loop,
                [tokenHash = std::move(tokenHash), keyHash = std::move(keyHash), requestHash = std::move(requestHash), method = std::move(method), path = std::move(path),
                 expiresAt = std::move(expiresAt)](AsynGyanis::Database::DatabaseConnection &connection) -> IdempotencyBeginResult
                {
                    const std::string now = Common::nowDateTimeUtc();
                    // 过期行先清掉：同一个键在 TTL 之后必须能重新开始，而不是被旧占位挡住
                    const std::array<AsynGyanis::Database::DatabaseValue, 3> expiredParameters{tokenHash, keyHash, now};
                    Sql::executeOrThrowVoid(connection, "DELETE FROM sys_idempotency WHERE token_hash = ? AND key_hash = ? AND expires_at <= ?", expiredParameters);

                    std::string insertError;
                    try
                    {
                        const std::array<AsynGyanis::Database::DatabaseValue, 9> insertParameters{Common::nextIdentifier(),   tokenHash, keyHash,  requestHash, method, path,
                                                                                                  std::string{"in_progress"}, now,       expiresAt};
                        Sql::executeOrThrowVoid(connection,
                                                "INSERT INTO sys_idempotency (id, token_hash, key_hash, request_hash, method, path, status, created_at, expires_at) "
                                                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                                insertParameters);
                        return IdempotencyBeginResult{IdempotencyState::Started};
                    } catch (const Common::DatabaseException &failure)
                    {
                        // 唯一约束是并发请求之间的仲裁者：插入被拒后只能读回现场再判定
                        insertError = failure.what();
                    }

                    const std::array<AsynGyanis::Database::DatabaseValue, 2> existingParameters{tokenHash, keyHash};
                    const auto                                               existing = Sql::executeOrThrow(
                            connection, std::string{"SELECT "} + kExistingColumns + " FROM sys_idempotency WHERE token_hash = ? AND key_hash = ? LIMIT 1", existingParameters);
                    if (!existing->next())
                    {
                        // 并发的 abort 可能刚删掉占位：没有可判定的现场，按插入失败上抛
                        throw Common::DatabaseException(insertError.empty() ? "幂等键占位写入失败且未读到已有记录" : insertError);
                    }
                    if (Sql::text(*existing, "request_hash") != requestHash)
                    {
                        return IdempotencyBeginResult{IdempotencyState::Conflict};
                    }
                    const std::string status = Sql::text(*existing, "status");
                    if (status == "completed")
                    {
                        const IdempotencyReplay replay = readReplay(*existing);
                        if (replay.status <= 0)
                        {
                            // 完成态缺少状态码说明记录不完整，宁可报冲突也不重放一条无法解释的响应
                            return IdempotencyBeginResult{IdempotencyState::Conflict};
                        }
                        return IdempotencyBeginResult{IdempotencyState::Replay, replay};
                    }
                    if (status == "in_progress")
                    {
                        return IdempotencyBeginResult{IdempotencyState::InProgress};
                    }
                    // 未知状态不猜语义：按冲突处理，让调用方换一个幂等键
                    return IdempotencyBeginResult{IdempotencyState::Conflict};
                });
    }

    AsynGyanis::Core::Task<> IdempotencyRepository::complete(AsynGyanis::Core::EventLoop &loop, std::string tokenHash, std::string keyHash, const int status, std::string traceId,
                                                             std::string contentType, std::string body)
    {
        co_await m_database->withConnection<std::size_t>(
                loop,
                [tokenHash = std::move(tokenHash), keyHash = std::move(keyHash), status, traceId = std::move(traceId), contentType = std::move(contentType),
                 body = std::move(body)](AsynGyanis::Database::DatabaseConnection &connection) -> std::size_t
                {
                    const std::array<AsynGyanis::Database::DatabaseValue, 7> parameters{static_cast<std::int64_t>(status), traceId, contentType, body, tokenHash, keyHash,
                                                                                        std::string{"in_progress"}};
                    const auto result = Sql::executeOrThrow(connection,
                                                            "UPDATE sys_idempotency SET status = 'completed', response_status = ?, response_trace_id = ?, "
                                                            "response_content_type = ?, response_body = ? WHERE token_hash = ? AND key_hash = ? AND status = ?",
                                                            parameters);
                    return result->affectedRowCount();
                });
        co_return;
    }

    AsynGyanis::Core::Task<> IdempotencyRepository::abort(AsynGyanis::Core::EventLoop &loop, std::string tokenHash, std::string keyHash)
    {
        co_await m_database->withConnection<std::size_t>(
                loop,
                [tokenHash = std::move(tokenHash), keyHash = std::move(keyHash)](AsynGyanis::Database::DatabaseConnection &connection) -> std::size_t
                {
                    const std::array<AsynGyanis::Database::DatabaseValue, 3> parameters{tokenHash, keyHash, std::string{"in_progress"}};
                    const auto result = Sql::executeOrThrow(connection, "DELETE FROM sys_idempotency WHERE token_hash = ? AND key_hash = ? AND status = ?", parameters);
                    return result->affectedRowCount();
                });
        co_return;
    }
} // namespace EdgePulse::Web
