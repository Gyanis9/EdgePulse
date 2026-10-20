/**
 * @file UserRepository.h
 * @brief 用户、角色与权限码的只读/更新仓储
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Common/Database.h"

#include <Core/Coroutine/Task.h>
#include <Core/EventLoop/EventLoop.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace EdgePulse::System
{
    /**
     * @brief 认证所需的用户行
     */
    struct UserRecord
    {
        std::int64_t id{0};           ///< 主键
        std::string  username;        ///< 登录名
        std::string  passwordHash;    ///< PBKDF2 字符串
        std::string  nickname;        ///< 显示名
        std::string  phone;           ///< 手机号
        std::string  email;           ///< 邮箱
        std::int64_t departmentId{0}; ///< 部门标识，P0 仅回显
        int          status{0};       ///< 1 启用、0 停用
        std::string  lastLoginTime;   ///< 最近登录时间文本，可空
    };

    /**
     * @brief 用户与权限查询
     */
    class UserRepository
    {
    public:
        /**
         * @brief 用数据库封装构造
         * @param database 进程级数据库
         */
        explicit UserRepository(std::shared_ptr<Common::Database> database);

        /**
         * @brief 按登录名查未删除用户
         * @param loop 恢复协程用的事件循环
         * @param username 登录名
         * @return 找到时返回用户；不存在时为空
         */
        AsynGyanis::Core::Task<std::optional<UserRecord>> findByUsername(AsynGyanis::Core::EventLoop &loop, std::string username);

        /**
         * @brief 按主键查未删除用户
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return 找到时返回用户；不存在时为空
         */
        AsynGyanis::Core::Task<std::optional<UserRecord>> findById(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 取用户拥有的权限码
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return 去重后的权限码，按字典序
         */
        AsynGyanis::Core::Task<std::vector<std::string>> permissionsFor(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 取用户启用的角色码
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return 角色码列表
         */
        AsynGyanis::Core::Task<std::vector<std::string>> rolesFor(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 更新密码哈希
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @param passwordHash 新的 PBKDF2 字符串
         * @return 是否命中一行
         */
        AsynGyanis::Core::Task<bool> updatePassword(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, std::string passwordHash);

        /**
         * @brief 记录一次成功登录
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @param loginTime 登录时刻的 UTC 文本
         * @return 是否命中一行
         */
        AsynGyanis::Core::Task<bool> touchLastLogin(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, std::string loginTime);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
    };
} // namespace EdgePulse::System
