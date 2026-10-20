/**
 * @file TenantRepository.h
 * @brief 租户注册表与租户初始化
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Api/Pagination.h"
#include "Base/Database/Database.h"

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::System
{
    /**
     * @brief 租户行
     */
    struct TenantRecord
    {
        std::int64_t id{0};      ///< 主键
        std::string  code;       ///< 租户编码
        std::string  name;       ///< 租户名称
        int          status{1};  ///< 1 启用、0 停用
        std::string  remark;     ///< 备注
        std::string  createTime; ///< 创建时间
    };

    /**
     * @brief 新建租户的输入
     */
    struct TenantProvisionRequest
    {
        std::string code;              ///< 租户编码
        std::string name;              ///< 租户名称
        std::string remark;            ///< 备注
        std::string adminUsername;     ///< 租户管理员登录名
        std::string adminNickname;     ///< 租户管理员显示名
        std::string adminPasswordHash; ///< 已计算的 PBKDF2 字符串
    };

    /**
     * @brief 新建租户的产物
     */
    struct TenantProvisionResult
    {
        std::int64_t tenantId{0};    ///< 租户
        std::int64_t adminUserId{0}; ///< 租户管理员
    };

    /**
     * @brief 租户仓储
     */
    class TenantRepository
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         */
        explicit TenantRepository(std::shared_ptr<Common::Database> database);

        /**
         * @brief 分页查询租户
         * @param loop 恢复协程用的事件循环
         * @param keyword 关键字
         * @param status 空、0、1
         * @param page 分页
         * @return Common::PageRows<TenantRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<TenantRecord>> list(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status, const Common::PageParameters &page);

        /**
         * @brief 按编码查租户
         * @param loop 恢复协程用的事件循环
         * @param code 租户编码
         * @return std::optional<TenantRecord> 租户
         */
        AsynGyanis::Core::Task<std::optional<TenantRecord>> findByCode(AsynGyanis::Core::EventLoop &loop, std::string code);

        /**
         * @brief 按主键查租户
         * @param loop 恢复协程用的事件循环
         * @param id 租户标识
         * @return std::optional<TenantRecord> 租户
         */
        AsynGyanis::Core::Task<std::optional<TenantRecord>> findById(AsynGyanis::Core::EventLoop &loop, std::int64_t id);

        /**
         * @brief 新建租户并创建租户管理员账号
         * @param loop 恢复协程用的事件循环
         * @param request 输入
         * @param operatorId 操作人
         * @return 成功返回租户与管理员标识；失败返回可读原因
         */
        AsynGyanis::Core::Task<std::expected<TenantProvisionResult, std::string>> provision(AsynGyanis::Core::EventLoop &loop, TenantProvisionRequest request,
                                                                                            std::int64_t operatorId);

        /**
         * @brief 修改租户名称、状态与备注
         * @param loop 恢复协程用的事件循环
         * @param id 租户
         * @param name 新名称，空保持
         * @param status 1/0；负值保持
         * @param remark 备注
         * @return bool 是否命中
         */
        AsynGyanis::Core::Task<bool> update(AsynGyanis::Core::EventLoop &loop, std::int64_t id, std::string name, int status, std::string remark);

        /**
         * @brief 编码是否已存在
         * @param loop 恢复协程用的事件循环
         * @param code 编码
         * @return bool 是否存在
         */
        AsynGyanis::Core::Task<bool> codeExists(AsynGyanis::Core::EventLoop &loop, std::string code);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
    };
} // namespace EdgePulse::System
