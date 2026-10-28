/**
 * @file SystemController.h
 * @brief 系统管理域 HTTP 路由
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

namespace AsynGyanis::Core
{
    class EventLoop;
}
namespace AsynGyanis::Net
{
    class Router;
}

namespace EdgePulse::Auth
{
    class AuthService;
    class DataScopeService;
    class MfaService;
} // namespace EdgePulse::Auth
namespace EdgePulse::System
{
    class DictionaryRepository;
    class TenantRepository;
    class MenuRepository;
    class OrgRepository;
    class RoleRepository;
    class UserRepository;
} // namespace EdgePulse::System

namespace EdgePulse::System
{
    /**
     * @brief 系统管理路由依赖的服务集合
     * @note 所有指针必须活到服务器收口
     */
    struct SystemDependencies
    {
        Auth::AuthService      *auth{nullptr};         ///< 认证服务
        Auth::DataScopeService *dataScopes{nullptr};   ///< 数据范围
        Auth::MfaService       *mfa{nullptr};          ///< 两步验证，用于管理员重置
        UserRepository         *users{nullptr};        ///< 用户
        RoleRepository         *roles{nullptr};        ///< 角色
        MenuRepository         *menus{nullptr};        ///< 菜单
        OrgRepository          *org{nullptr};          ///< 部门与岗位
        DictionaryRepository   *dictionaries{nullptr}; ///< 字典与参数
        TenantRepository       *tenants{nullptr};      ///< 租户
    };

    /**
     * @brief 注册 /api/system 下的全部管理路由
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 服务集合
     */
    void registerSystemRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const SystemDependencies &dependencies);
} // namespace EdgePulse::System
