#include "Core/System/MenuRepository.h"

#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"

#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace EdgePulse::System
{
    namespace
    {
        /**
         * @brief 读一列文本；NULL 或非文本时交回空串
         */
        [[nodiscard]] std::string readText(const AsynGyanis::Database::DatabaseValue &value)
        {
            if (const auto *text = std::get_if<std::string>(&value); text != nullptr)
            {
                return *text;
            }
            return {};
        }

        /**
         * @brief 读一列整数；NULL 或非整数时交回 0
         */
        [[nodiscard]] std::int64_t readInteger(const AsynGyanis::Database::DatabaseValue &value)
        {
            if (const auto *number = std::get_if<std::int64_t>(&value); number != nullptr)
            {
                return *number;
            }
            return 0;
        }
    } // namespace

    MenuRepository::MenuRepository(std::shared_ptr<Common::Database> database) : m_database(std::move(database))
    {
    }

    AsynGyanis::Core::Task<std::vector<MenuNode>> MenuRepository::treeForUser(AsynGyanis::Core::EventLoop &loop, const std::int64_t userId)
    {
        co_return co_await m_database->withConnection<std::vector<MenuNode>>(
                loop,
                [userId](AsynGyanis::Database::DatabaseConnection &connection) -> std::vector<MenuNode>
                {
                    const std::array<AsynGyanis::Database::DatabaseValue, 1>    parameters{userId};
                    const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute(
                            "SELECT DISTINCT m.id, m.parent_id, m.menu_name, m.menu_type, m.icon, m.path, m.component, m.permission, m.sort, m.visible, m.status "
                            "FROM sys_menu m "
                            "JOIN sys_role_menu rm ON rm.menu_id = m.id "
                            "JOIN sys_user_role ur ON ur.role_id = rm.role_id "
                            "WHERE ur.user_id = ? AND m.menu_type IN ('dir', 'menu') AND m.visible = 1 AND m.status = 1 "
                            "ORDER BY m.sort, m.id",
                            parameters);
                    if (!result)
                    {
                        throw Common::DatabaseException("查询菜单失败：" + connection.lastError());
                    }

                    std::unordered_map<std::int64_t, MenuNode>                  nodesById;
                    std::unordered_map<std::int64_t, std::vector<std::int64_t>> childrenByParent;
                    while (result->next())
                    {
                        MenuNode node;
                        node.id                   = readInteger(result->getValue("id"));
                        node.parentId             = readInteger(result->getValue("parent_id"));
                        node.name                 = readText(result->getValue("menu_name"));
                        node.type                 = readText(result->getValue("menu_type"));
                        node.icon                 = readText(result->getValue("icon"));
                        node.path                 = readText(result->getValue("path"));
                        node.component            = readText(result->getValue("component"));
                        node.permission           = readText(result->getValue("permission"));
                        node.sort                 = static_cast<int>(readInteger(result->getValue("sort")));
                        node.visible              = static_cast<int>(readInteger(result->getValue("visible")));
                        node.status               = static_cast<int>(readInteger(result->getValue("status")));
                        const std::int64_t nodeId = node.id;
                        childrenByParent[node.parentId].push_back(nodeId);
                        nodesById.emplace(nodeId, std::move(node));
                    }

                    // 递归组装：查询已按 sort、id 排序，子节点顺序随之稳定
                    std::function<MenuNode(std::int64_t)> buildNode = [&](const std::int64_t nodeId) -> MenuNode
                    {
                        MenuNode node = nodesById.at(nodeId);
                        if (const auto children = childrenByParent.find(nodeId); children != childrenByParent.end())
                        {
                            for (const std::int64_t childId: children->second)
                            {
                                node.children.push_back(buildNode(childId));
                            }
                        }
                        return node;
                    };

                    std::vector<MenuNode> roots;
                    if (const auto rootChildren = childrenByParent.find(0); rootChildren != childrenByParent.end())
                    {
                        for (const std::int64_t rootId: rootChildren->second)
                        {
                            roots.push_back(buildNode(rootId));
                        }
                    }
                    return roots;
                });
    }
} // namespace EdgePulse::System
