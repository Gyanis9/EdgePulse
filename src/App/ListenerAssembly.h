/**
 * @file ListenerAssembly.h
 * @brief HTTP 路由装配入口
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

namespace AsynGyanis::Net
{
    class Router;
}

namespace EdgePulse::App
{
    /**
     * @brief 注册健康检查接口
     * @param router 目标路由器
     */
    void registerHealthRoutes(AsynGyanis::Net::Router &router);

    /**
     * @brief 注册兜底路由：未命中的路径回统一 JSON 404
     * @param router 目标路由器
     * @note 静态站点接入后由静态站点接管非 /api 路径，本函数只负责 API 的 404 包络
     */
    void registerFallbackRoute(AsynGyanis::Net::Router &router);
} // namespace EdgePulse::App
