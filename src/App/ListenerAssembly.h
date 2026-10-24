/**
 * @file ListenerAssembly.h
 * @brief HTTP 路由装配入口
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <filesystem>

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
     * @brief 注册 OpenAPI 3.1 契约端点
     * @param router 目标路由器
     * @param webRoot 前端构建产物目录，契约从 webRoot/openapi.json 读取
     * @details 文件在注册时一次读入内存，运行期不再访问磁盘；缺失时接口返回统一 JSON 404
     */
    void registerOpenApiRoute(AsynGyanis::Net::Router &router, const std::filesystem::path &webRoot);

    /**
     * @brief 注册站点兜底：非 API 的 GET 命中构建产物时发文件，否则回 index.html
     * @param router 目标路由器
     * @param webRoot 前端构建产物目录；缺少 index.html 时只回 API 的统一 404
     * @details 静态文件与 SPA 回退放在同一条兜底路由里，避免两条 any("*") 互相覆盖
     */
    void registerSiteFallback(AsynGyanis::Net::Router &router, const std::filesystem::path &webRoot);
} // namespace EdgePulse::App
