/**
 * @file OidcController.h
 * @brief OIDC 登录路由
 * @author Gyanis
 * @date 2026-10-21
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
    class OidcService;

    /**
     * @brief 注册启用查询、授权跳转、回调与一次性 code 交换路由
     * @param router 公开路由器
     * @param loop 服务器所属事件循环
     * @param oidc OIDC 服务
     */
    void registerOidcRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, OidcService &oidc);
} // namespace EdgePulse::Auth
