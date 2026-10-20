/**
 * @file ReverseProxy.h
 * @brief 边缘反向代理：把配置前缀下的请求转发到上游 HTTP 服务
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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
}

namespace EdgePulse::Web
{
    /**
     * @brief 反向代理配置
     */
    struct ReverseProxyConfiguration
    {
        std::string  prefix{"/edge"};                              ///< 对外路径前缀
        std::string  upstream;                                     ///< 上游基地址，如 http://127.0.0.1:9000
        bool         stripPrefix{true};                            ///< 转发时去掉前缀
        bool         requireSession{true};                         ///< 是否要求 EdgePulse 会话
        std::string  permission;                                   ///< 需要的权限码；空表示只要求登录
        std::int64_t timeoutMilliseconds{15000};                   ///< 单次转发总时限
        std::size_t  maximumRequestBodyBytes{8U * 1024U * 1024U};  ///< 请求正文上限
        std::size_t  maximumResponseBodyBytes{8U * 1024U * 1024U}; ///< 上游响应正文上限
    };

    /**
     * @brief 注册反向代理路由
     * @param router 公开路由器
     * @param loop 服务器所属事件循环
     * @param configuration 配置
     * @param auth 认证服务；requireSession 为假时可为空
     * @note 必须在站点兜底路由之前注册，否则通配路由会吞掉代理前缀
     */
    void registerReverseProxy(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, ReverseProxyConfiguration configuration, Auth::AuthService *auth);
} // namespace EdgePulse::Web
