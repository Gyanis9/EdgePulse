#include "Core/Device/NodeGroupRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace EdgePulse::Device
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        [[nodiscard]] NodeGroupRecord readGroup(const DatabaseResult &result)
        {
            NodeGroupRecord group;
            group.id         = Sql::integer(result, "id");
            group.parentId   = Sql::integer(result, "parent_id");
            group.name       = Sql::text(result, "name");
            group.sort       = static_cast<int>(Sql::integer(result, "sort"));
            group.remark     = Sql::text(result, "remark");
            group.nodeCount  = Sql::integer(result, "node_count");
            group.createTime = Sql::dateTimeText(result, "created_at");
            return group;
        }
    } // namespace

    NodeGroupRepository::NodeGroupRepository(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<std::vector<NodeGroupRecord>> NodeGroupRepository::tree(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await m_database->withConnection<std::vector<NodeGroupRecord>>(
                loop,
                [](DatabaseConnection &connection) -> std::vector<NodeGroupRecord>
                {
                    const auto rows = Sql::executeOrThrow(connection, "SELECT g.id, g.parent_id, g.name, g.sort, g.remark, g.created_at, "
                                                                      "COALESCE((SELECT COUNT(*) FROM biz_node n WHERE n.group_id = g.id AND n.deleted = 0), 0) AS node_count "
                                                                      "FROM biz_node_group g WHERE g.deleted = 0 ORDER BY g.sort, g.id");
                    std::unordered_map<std::int64_t, NodeGroupRecord>           groups;
                    std::unordered_map<std::int64_t, std::vector<std::int64_t>> children;
                    while (rows->next())
                    {
                        NodeGroupRecord    group = readGroup(*rows);
                        const std::int64_t id    = group.id;
                        children[group.parentId].push_back(id);
                        groups.emplace(id, std::move(group));
                    }
                    std::function<NodeGroupRecord(std::int64_t)> build = [&](const std::int64_t id) -> NodeGroupRecord
                    {
                        NodeGroupRecord group = groups.at(id);
                        if (const auto iterator = children.find(id); iterator != children.end())
                        {
                            for (const std::int64_t childId: iterator->second)
                            {
                                group.children.push_back(build(childId));
                            }
                        }
                        return group;
                    };
                    std::vector<NodeGroupRecord> roots;
                    if (const auto iterator = children.find(0); iterator != children.end())
                    {
                        for (const std::int64_t id: iterator->second)
                        {
                            roots.push_back(build(id));
                        }
                    }
                    return roots;
                });
    }

    AsynGyanis::Core::Task<bool> NodeGroupRepository::exists(AsynGyanis::Core::EventLoop &loop, const std::int64_t groupId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [groupId](DatabaseConnection &connection) -> bool
                                                            {
                                                                const std::array<DatabaseValue, 1> parameters{groupId};
                                                                const auto                         rows = Sql::executeOrThrow(
                                                                        connection, "SELECT COUNT(*) AS total FROM biz_node_group WHERE id = ? AND deleted = 0", parameters);
                                                                return rows->next() && Sql::integer(*rows, "total") > 0;
                                                            });
    }

    AsynGyanis::Core::Task<std::int64_t> NodeGroupRepository::create(AsynGyanis::Core::EventLoop &loop, const std::int64_t parentId, std::string name, const int sort,
                                                                     std::string remark, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [parentId, name = std::move(name), sort, remark = std::move(remark), operatorId](DatabaseConnection &connection) -> std::int64_t
                {
                    const std::int64_t                  id  = Common::nextIdentifier();
                    const std::string                   now = Common::nowDateTimeUtc();
                    const std::array<DatabaseValue, 10> parameters{id, parentId, name, static_cast<std::int64_t>(sort), remark, now, now, operatorId, operatorId, std::int64_t{0}};
                    Sql::executeOrThrowVoid(connection,
                                            "INSERT INTO biz_node_group (id, parent_id, name, sort, remark, created_at, updated_at, created_by, updated_by, deleted) "
                                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                            parameters);
                    return id;
                });
    }

    AsynGyanis::Core::Task<bool> NodeGroupRepository::update(AsynGyanis::Core::EventLoop &loop, const std::int64_t groupId, std::optional<std::int64_t> parentId,
                                                             std::optional<std::string> name, std::optional<int> sort, std::optional<std::string> remark,
                                                             const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [groupId, parentId, name = std::move(name), sort, remark = std::move(remark), operatorId](DatabaseConnection &connection) -> bool
                {
                    std::string                sql = "UPDATE biz_node_group SET updated_at = ?, updated_by = ?";
                    std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), operatorId};
                    if (parentId.has_value())
                    {
                        sql += ", parent_id = ?";
                        parameters.emplace_back(*parentId);
                    }
                    if (name.has_value())
                    {
                        sql += ", name = ?";
                        parameters.emplace_back(*name);
                    }
                    if (sort.has_value())
                    {
                        sql += ", sort = ?";
                        parameters.emplace_back(static_cast<std::int64_t>(*sort));
                    }
                    if (remark.has_value())
                    {
                        sql += ", remark = ?";
                        parameters.emplace_back(*remark);
                    }
                    sql += " WHERE id = ? AND deleted = 0";
                    parameters.emplace_back(groupId);
                    const auto rows = Sql::executeOrThrow(connection, sql, parameters);
                    return rows->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<bool> NodeGroupRepository::remove(AsynGyanis::Core::EventLoop &loop, const std::int64_t groupId)
    {
        co_return co_await m_database->withConnection<bool>(
                loop,
                [groupId](DatabaseConnection &connection) -> bool
                {
                    const std::array<DatabaseValue, 2> parameters{Common::nowDateTimeUtc(), groupId};
                    const auto rows = Sql::executeOrThrow(connection, "UPDATE biz_node_group SET deleted = 1, updated_at = ? WHERE id = ? AND deleted = 0", parameters);
                    return rows->affectedRowCount() > 0;
                });
    }

    AsynGyanis::Core::Task<bool> NodeGroupRepository::isInUse(AsynGyanis::Core::EventLoop &loop, const std::int64_t groupId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [groupId](DatabaseConnection &connection) -> bool
                                                            {
                                                                const std::array<DatabaseValue, 1> parameters{groupId};
                                                                const auto                         rows =
                                                                        Sql::executeOrThrow(connection,
                                                                                            "SELECT (SELECT COUNT(*) FROM biz_node_group WHERE parent_id = ? AND deleted = 0) + "
                                                                                            "(SELECT COUNT(*) FROM biz_node WHERE group_id = ? AND deleted = 0) AS total",
                                                                                            std::array<DatabaseValue, 2>{groupId, groupId});
                                                                return rows->next() && Sql::integer(*rows, "total") > 0;
                                                            });
    }
} // namespace EdgePulse::Device
