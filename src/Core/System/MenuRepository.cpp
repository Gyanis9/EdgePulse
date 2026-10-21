#include "Core/System/MenuRepository.h"

#include "Base/Database/SqlExecution.h"
#include "Base/Database/SqlHelpers.h"
#include "Base/Runtime/IdGenerator.h"
#include "Base/Runtime/TimeFormat.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace EdgePulse::System
{
    namespace
    {
        using AsynGyanis::Database::DatabaseConnection;
        using AsynGyanis::Database::DatabaseResult;
        using AsynGyanis::Database::DatabaseValue;

        [[nodiscard]] MenuNode readMenu(const DatabaseResult &result)
        {
            MenuNode node;
            node.id         = Sql::integer(result, "id");
            node.parentId   = Sql::integer(result, "parent_id");
            node.name       = Sql::text(result, "menu_name");
            node.type       = Sql::text(result, "menu_type");
            node.icon       = Sql::text(result, "icon");
            node.path       = Sql::text(result, "path");
            node.component  = Sql::text(result, "component");
            node.permission = Sql::text(result, "permission");
            node.sort       = static_cast<int>(Sql::integer(result, "sort"));
            node.visible    = static_cast<int>(Sql::integer(result, "visible"));
            node.status     = static_cast<int>(Sql::integer(result, "status"));
            return node;
        }

        constexpr std::string_view kSelectMenu = "SELECT id, parent_id, menu_name, menu_type, icon, path, component, permission, sort, visible, status FROM sys_menu ";

        [[nodiscard]] std::vector<MenuNode> buildTree(std::vector<MenuNode> nodes)
        {
            std::unordered_map<std::int64_t, MenuNode>                  byId;
            std::unordered_map<std::int64_t, std::vector<std::int64_t>> childrenByParent;
            for (MenuNode &node: nodes)
            {
                const std::int64_t id = node.id;
                childrenByParent[node.parentId].push_back(id);
                byId.emplace(id, std::move(node));
            }
            std::function<MenuNode(std::int64_t)> build = [&](const std::int64_t id) -> MenuNode
            {
                MenuNode node = byId.at(id);
                if (const auto children = childrenByParent.find(id); children != childrenByParent.end())
                {
                    for (const std::int64_t childId: children->second)
                    {
                        node.children.push_back(build(childId));
                    }
                }
                return node;
            };
            std::vector<MenuNode> roots;
            if (const auto rootChildren = childrenByParent.find(0); rootChildren != childrenByParent.end())
            {
                for (const std::int64_t id: rootChildren->second)
                {
                    roots.push_back(build(id));
                }
            }
            return roots;
        }

        [[nodiscard]] bool matches(const MenuNode &node, const std::string &keyword)
        {
            return keyword.empty() || node.name.find(keyword) != std::string::npos || node.path.find(keyword) != std::string::npos ||
                   node.permission.find(keyword) != std::string::npos || node.component.find(keyword) != std::string::npos;
        }

        /**
         * @brief 过滤管理树：命中节点连同后代保留，未命中但后代命中时保留祖先
         */
        [[nodiscard]] bool filterMenu(MenuNode &node, const std::string &keyword, const std::string &status)
        {
            const bool            statusOk = status.empty() || std::to_string(node.status) == status;
            std::vector<MenuNode> keptChildren;
            for (MenuNode &child: node.children)
            {
                if (filterMenu(child, keyword, status))
                {
                    keptChildren.push_back(std::move(child));
                }
            }
            node.children     = std::move(keptChildren);
            const bool selfOk = statusOk && matches(node, keyword);
            return selfOk || !node.children.empty();
        }
    } // namespace

    MenuRepository::MenuRepository(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<std::vector<MenuNode>> MenuRepository::treeForUser(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        co_return co_await m_database->withConnection<std::vector<MenuNode>>(
                loop,
                [userId](DatabaseConnection &connection) -> std::vector<MenuNode>
                {
                    const std::array<DatabaseValue, 1> parameters{userId};
                    const auto            rows = Sql::executeOrThrow(connection,
                                                                     "SELECT DISTINCT m.id, m.parent_id, m.menu_name, m.menu_type, m.icon, m.path, m.component, m.permission, "
                                                                     "m.sort, m.visible, m.status FROM sys_menu m "
                                                                     "JOIN sys_role_menu rm ON rm.menu_id = m.id "
                                                                     "JOIN sys_user_role ur ON ur.role_id = rm.role_id "
                                                                     "WHERE ur.user_id = ? AND m.menu_type IN ('dir', 'menu') AND m.visible = 1 AND m.status = 1 "
                                                                     "ORDER BY m.sort, m.id",
                                                                     parameters);
                    std::vector<MenuNode> nodes;
                    while (rows->next())
                    {
                        nodes.push_back(readMenu(*rows));
                    }
                    return buildTree(std::move(nodes));
                });
    }

    AsynGyanis::Core::Task<std::vector<MenuNode>> MenuRepository::tree(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status)
    {
        co_return co_await m_database->withConnection<std::vector<MenuNode>>(
                loop,
                [keyword = std::move(keyword), status = std::move(status)](DatabaseConnection &connection) -> std::vector<MenuNode>
                {
                    const auto            rows = Sql::executeOrThrow(connection, std::string{kSelectMenu} + "ORDER BY sort, id");
                    std::vector<MenuNode> nodes;
                    while (rows->next())
                    {
                        nodes.push_back(readMenu(*rows));
                    }
                    std::vector<MenuNode> roots = buildTree(std::move(nodes));
                    roots.erase(std::remove_if(roots.begin(), roots.end(), [&](MenuNode &node) { return !filterMenu(node, keyword, status); }), roots.end());
                    return roots;
                });
    }

    AsynGyanis::Core::Task<std::optional<MenuNode>> MenuRepository::findById(AsynGyanis::Core::EventLoop &loop, const std::int64_t menuId)
    {
        co_return co_await m_database->withConnection<std::optional<MenuNode>>(loop,
                                                                               [menuId](DatabaseConnection &connection) -> std::optional<MenuNode>
                                                                               {
                                                                                   const std::array<DatabaseValue, 1> parameters{menuId};
                                                                                   const auto                         rows =
                                                                                           Sql::executeOrThrow(connection, std::string{kSelectMenu} + "WHERE id = ?", parameters);
                                                                                   if (!rows->next())
                                                                                   {
                                                                                       return std::nullopt;
                                                                                   }
                                                                                   return readMenu(*rows);
                                                                               });
    }

    AsynGyanis::Core::Task<std::int64_t> MenuRepository::create(AsynGyanis::Core::EventLoop &loop, const MenuWrite &write, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<std::int64_t>(
                loop,
                [write, operatorId](DatabaseConnection &connection) -> std::int64_t
                {
                    const std::int64_t                  id  = Common::nextIdentifier();
                    const std::string                   now = Common::nowDateTimeUtc();
                    const std::array<DatabaseValue, 14> parameters{id,
                                                                   write.parentId.value_or(0),
                                                                   write.name.value_or(std::string{}),
                                                                   write.type.value_or(std::string{"menu"}),
                                                                   write.icon.value_or(std::string{}),
                                                                   write.path.value_or(std::string{}),
                                                                   write.component.value_or(std::string{}),
                                                                   write.permission.value_or(std::string{}),
                                                                   static_cast<std::int64_t>(write.sort.value_or(0)),
                                                                   static_cast<std::int64_t>(write.visible.value_or(1)),
                                                                   static_cast<std::int64_t>(write.status.value_or(1)),
                                                                   now,
                                                                   now,
                                                                   operatorId};
                    Sql::executeOrThrowVoid(connection,
                                            "INSERT INTO sys_menu (id, parent_id, menu_name, menu_type, icon, path, component, permission, sort, visible, status, "
                                            "created_at, updated_at, created_by) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                            parameters);
                    return id;
                });
    }

    AsynGyanis::Core::Task<bool> MenuRepository::update(AsynGyanis::Core::EventLoop &loop, const std::int64_t menuId, const MenuWrite &write, const std::int64_t operatorId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [menuId, write, operatorId](DatabaseConnection &connection) -> bool
                                                            {
                                                                std::string                sql = "UPDATE sys_menu SET updated_at = ?, updated_by = ?";
                                                                std::vector<DatabaseValue> parameters{Common::nowDateTimeUtc(), operatorId};
                                                                const auto                 appendText = [&](const std::optional<std::string> &value, const char *column)
                                                                {
                                                                    if (value.has_value())
                                                                    {
                                                                        sql += ", ";
                                                                        sql += column;
                                                                        sql += " = ?";
                                                                        parameters.emplace_back(*value);
                                                                    }
                                                                };
                                                                appendText(write.name, "menu_name");
                                                                appendText(write.type, "menu_type");
                                                                appendText(write.icon, "icon");
                                                                appendText(write.path, "path");
                                                                appendText(write.component, "component");
                                                                appendText(write.permission, "permission");
                                                                if (write.parentId.has_value())
                                                                {
                                                                    sql += ", parent_id = ?";
                                                                    parameters.emplace_back(*write.parentId);
                                                                }
                                                                if (write.sort.has_value())
                                                                {
                                                                    sql += ", sort = ?";
                                                                    parameters.emplace_back(static_cast<std::int64_t>(*write.sort));
                                                                }
                                                                if (write.visible.has_value())
                                                                {
                                                                    sql += ", visible = ?";
                                                                    parameters.emplace_back(static_cast<std::int64_t>(*write.visible));
                                                                }
                                                                if (write.status.has_value())
                                                                {
                                                                    sql += ", status = ?";
                                                                    parameters.emplace_back(static_cast<std::int64_t>(*write.status));
                                                                }
                                                                sql += " WHERE id = ?";
                                                                parameters.emplace_back(menuId);
                                                                const auto rows = Sql::executeOrThrow(connection, sql, parameters);
                                                                return rows->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<bool> MenuRepository::remove(AsynGyanis::Core::EventLoop &loop, const std::int64_t menuId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [menuId](DatabaseConnection &connection) -> bool
                                                            {
                                                                const std::array<DatabaseValue, 1> parameters{menuId};
                                                                Sql::executeOrThrowVoid(connection, "DELETE FROM sys_role_menu WHERE menu_id = ?", parameters);
                                                                const auto rows = Sql::executeOrThrow(connection, "DELETE FROM sys_menu WHERE id = ?", parameters);
                                                                return rows->affectedRowCount() > 0;
                                                            });
    }

    AsynGyanis::Core::Task<bool> MenuRepository::hasChildren(AsynGyanis::Core::EventLoop &loop, const std::int64_t menuId)
    {
        co_return co_await m_database->withConnection<bool>(loop,
                                                            [menuId](DatabaseConnection &connection) -> bool
                                                            {
                                                                const std::array<DatabaseValue, 1> parameters{menuId};
                                                                const auto                         rows =
                                                                        Sql::executeOrThrow(connection, "SELECT COUNT(*) AS total FROM sys_menu WHERE parent_id = ?", parameters);
                                                                return rows->next() && Sql::integer(*rows, "total") > 0;
                                                            });
    }
    AsynGyanis::Core::Task<std::vector<std::string>> MenuRepository::permissionsFor(AsynGyanis::Core::EventLoop &loop, std::vector<std::int64_t> menuIds)
    {
        co_return co_await m_database->withConnection<std::vector<std::string>>(loop,
                                                                                [menuIds = std::move(menuIds)](DatabaseConnection &connection) -> std::vector<std::string>
                                                                                {
                                                                                    if (menuIds.empty())
                                                                                    {
                                                                                        return {};
                                                                                    }
                                                                                    std::string                sql = "SELECT permission FROM sys_menu WHERE id IN (";
                                                                                    std::vector<DatabaseValue> parameters;
                                                                                    for (std::size_t index = 0; index < menuIds.size(); ++index)
                                                                                    {
                                                                                        sql += index == 0 ? "?" : ", ?";
                                                                                        parameters.emplace_back(menuIds[index]);
                                                                                    }
                                                                                    sql += ") AND permission <> ''";
                                                                                    const auto               rows = Sql::executeOrThrow(connection, sql, parameters);
                                                                                    std::vector<std::string> result;
                                                                                    while (rows->next())
                                                                                    {
                                                                                        result.push_back(Sql::text(*rows, "permission"));
                                                                                    }
                                                                                    return result;
                                                                                });
    }
} // namespace EdgePulse::System
