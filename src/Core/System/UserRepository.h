/**
 * @file UserRepository.h
 * @brief 用户、角色与权限码仓储
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Api/Pagination.h"
#include "Base/Database/Database.h"
#include "Core/System/SystemModels.h"
#include "Core/System/TenantScope.h"

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
     * @brief 认证与列表共用的用户行
     */
    struct UserRecord
    {
        std::int64_t              id{0};                ///< 主键
        std::int64_t              tenantId{1};          ///< 归属租户
        std::string               tenantCode;           ///< 租户编码
        std::string               tenantName;           ///< 租户名称
        bool                      platformAdmin{false}; ///< 是否平台管理员
        std::string               username;             ///< 登录名
        std::string               passwordHash;         ///< PBKDF2 字符串
        std::string               nickname;             ///< 显示名
        std::string               phone;                ///< 手机号
        std::string               email;                ///< 邮箱
        std::int64_t              departmentId{0};      ///< 部门标识
        std::string               departmentName;       ///< 部门名
        int                       status{0};            ///< 1 启用、0 停用
        bool                      mfaEnabled{false};    ///< 是否已启用两步验证
        int                       failedLoginCount{0};  ///< 当前失败窗口内的连续失败次数
        std::string               lockedUntil;          ///< 锁定截止时刻，空表示未锁定
        std::string               lastLoginTime;        ///< 最近登录时间
        std::string               createTime;           ///< 创建时间
        std::string               roleNames;            ///< 角色名，逗号分隔
        std::string               postNames;            ///< 岗位名，逗号分隔
        std::vector<std::int64_t> roleIds;              ///< 角色标识
        std::vector<std::int64_t> postIds;              ///< 岗位标识
    };

    /**
     * @brief 用户仓储：认证读取与管理域 CRUD
     */
    class UserRepository
    {
    public:
        /**
         * @brief 用数据库封装构造
         * @param database 进程级数据库
         */
        explicit UserRepository(std::shared_ptr<Common::Database> database, TenantScope tenant = {});

        /**
         * @brief 按租户范围派生一份仓储视图
         * @param tenant 租户范围
         * @return std::shared_ptr<UserRepository> 共享同一数据库的视图
         * @note 视图只影响查询过滤与插入归属，不复制连接
         */
        [[nodiscard]] std::shared_ptr<UserRepository> scoped(TenantScope tenant) const;

        /**
         * @brief 按登录名查未删除用户
         * @param loop 恢复协程用的事件循环
         * @param username 登录名
         * @return std::optional<UserRecord> 用户行
         */
        AsynGyanis::Core::Task<std::optional<UserRecord>> findByUsername(AsynGyanis::Core::EventLoop &loop, std::string username);

        /**
         * @brief 按邮箱查未删除用户
         * @param loop 恢复协程用的事件循环
         * @param email 邮箱
         * @return std::optional<UserRecord> 用户行
         */
        AsynGyanis::Core::Task<std::optional<UserRecord>> findByEmail(AsynGyanis::Core::EventLoop &loop, std::string email);

        /**
         * @brief 按主键查未删除用户
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return std::optional<UserRecord> 用户行
         */
        AsynGyanis::Core::Task<std::optional<UserRecord>> findById(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 分页查询用户
         * @param loop 恢复协程用的事件循环
         * @param query 过滤与数据范围
         * @param page 分页参数
         * @return Common::PageRows<UserRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<UserRecord>> list(AsynGyanis::Core::EventLoop &loop, const UserQuery &query, const Common::PageParameters &page);

        /**
         * @brief 导出用户（最多一万行）
         * @param loop 恢复协程用的事件循环
         * @param query 过滤与数据范围
         * @return std::vector<UserRecord> 行
         */
        AsynGyanis::Core::Task<std::vector<UserRecord>> exportAll(AsynGyanis::Core::EventLoop &loop, const UserQuery &query);

        /**
         * @brief 用户名是否已被占用
         * @param loop 恢复协程用的事件循环
         * @param username 登录名
         * @param excludeUserId 排除的用户，0 表示不排除
         * @return bool 是否占用
         */
        AsynGyanis::Core::Task<bool> usernameExists(AsynGyanis::Core::EventLoop &loop, std::string username, std::int64_t excludeUserId = 0);

        /**
         * @brief 创建用户并写入角色与岗位
         * @param loop 恢复协程用的事件循环
         * @param write 用户字段
         * @param passwordHash 已计算的 PBKDF2 字符串
         * @param operatorId 操作人
         * @return std::int64_t 新用户标识
         */
        AsynGyanis::Core::Task<std::int64_t> create(AsynGyanis::Core::EventLoop &loop, const UserWrite &write, std::string passwordHash, std::int64_t operatorId);

        /**
         * @brief 更新用户；未给出的字段保持原值
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @param write 待更新字段
         * @param operatorId 操作人
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> update(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, const UserWrite &write, std::int64_t operatorId);

        /**
         * @brief 软删除用户并清理角色岗位关系
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> remove(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 启用或停用
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @param status 1 启用、0 停用
         * @param operatorId 操作人
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> updateStatus(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, int status, std::int64_t operatorId);

        /**
         * @brief 重置口令
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @param passwordHash 新 PBKDF2 字符串
         * @param operatorId 操作人
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> resetPassword(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, std::string passwordHash, std::int64_t operatorId);

        /**
         * @brief 记录一次口令校验失败，必要时锁定账号
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @param windowSeconds 失败计数的滑动窗口，单位秒
         * @param maximumFailures 触发锁定的失败次数
         * @param lockSeconds 锁定时长，单位秒
         * @return 锁定截止时刻在将来时返回该时刻；未触发锁定或锁定已过期返回空
         * @note 计数递增由单条条件 UPDATE 完成，并发失败不会各自读到旧值再覆盖
         * @note 用户不存在、已删除或不在当前租户范围内时返回空，调用方仍按统一 401 处理
         * @note 达到阈值时只在锁定为空或已过期时补写截止时刻，不覆盖其它请求刚写入的新锁定
         */
        AsynGyanis::Core::Task<std::optional<std::string>> recordLoginFailure(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, int windowSeconds, int maximumFailures,
                                                                              int lockSeconds);

        /**
         * @brief 清空账号的失败计数与锁定
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> clearLoginFailures(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 取用户角色标识
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return std::vector<std::int64_t> 角色标识
         */
        AsynGyanis::Core::Task<std::vector<std::int64_t>> roleIdsFor(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 取用户岗位标识
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return std::vector<std::int64_t> 岗位标识
         */
        AsynGyanis::Core::Task<std::vector<std::int64_t>> postIdsFor(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 取全部用户与角色关联，供权限关系图
         * @param loop 恢复协程用的事件循环
         * @param limit 最多返回的关联数
         * @return std::vector<std::pair<std::int64_t, std::int64_t>> [用户, 角色] 列表
         */
        AsynGyanis::Core::Task<std::vector<std::pair<std::int64_t, std::int64_t>>> userRolePairs(AsynGyanis::Core::EventLoop &loop, int limit);

        /**
         * @brief 取用户拥有的权限码
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return std::vector<std::string> 去重权限码
         */
        AsynGyanis::Core::Task<std::vector<std::string>> permissionsFor(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 取用户启用的角色码
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @return std::vector<std::string> 角色码
         */
        AsynGyanis::Core::Task<std::vector<std::string>> rolesFor(AsynGyanis::Core::EventLoop &loop, std::int64_t userId);

        /**
         * @brief 更新密码哈希
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @param passwordHash 新 PBKDF2 字符串
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> updatePassword(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, std::string passwordHash);

        /**
         * @brief 记录一次成功登录
         * @param loop 恢复协程用的事件循环
         * @param userId 用户标识
         * @param loginTime SQL DATETIME 文本
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> touchLastLogin(AsynGyanis::Core::EventLoop &loop, std::int64_t userId, std::string loginTime);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
        TenantScope                       m_tenant;   ///< 租户范围
    };
} // namespace EdgePulse::System
