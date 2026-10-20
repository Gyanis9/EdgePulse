/**
 * @file HttpClientHarness.h
 * @brief 回环 HTTP 测试夹具
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Config/ConfigValue.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    class HttpServer;
    class Router;
} // namespace AsynGyanis::Net

namespace EdgePulse::TestSupport
{
    /**
     * @brief 一次回环 HTTP 调用的结果
     */
    struct HttpResult
    {
        bool                                             transportOk{false}; ///< 是否拿到了完整响应
        int                                              status{0};          ///< HTTP 状态码
        std::string                                      body;               ///< 响应正文
        std::vector<std::pair<std::string, std::string>> headers;            ///< 原始响应头
        std::string                                      error;              ///< 传输失败原因

        /**
         * @brief 按名字取响应头，大小写不敏感
         */
        [[nodiscard]] std::optional<std::string> header(std::string_view name) const;

        /**
         * @brief 把正文解析成 JSON；语法不合法时为空
         */
        [[nodiscard]] std::optional<AsynGyanis::Base::ConfigValue> json() const;
    };

    /**
     * @brief 自持事件循环与真实 HttpServer 的回环夹具
     *
     * @details 服务端与客户端跑在同一条事件循环上：服务端在构造时绑定端口 0，
     *          客户端请求经 scheduleRemote 投递到循环线程，测试线程只等待 future。
     */
    class ServerHarness
    {
    public:
        /**
         * @brief 起一台监听 127.0.0.1:0 的服务器
         * @param configure 路由注册回调，在 start() 之前执行
         * @throws std::runtime_error 端口未能在时限内就绪
         */
        explicit ServerHarness(const std::function<void(AsynGyanis::Net::Router &, AsynGyanis::Core::EventLoop &)> &configure);

        ~ServerHarness();

        ServerHarness(const ServerHarness &)            = delete;
        ServerHarness &operator=(const ServerHarness &) = delete;

        /// 内核分配的监听端口
        [[nodiscard]] std::uint16_t port() const noexcept;

        /// 服务端所属事件循环
        [[nodiscard]] AsynGyanis::Core::EventLoop &loop() noexcept;

        /**
         * @brief 发一次请求
         * @param method HTTP 方法
         * @param path 以 / 开头的路径
         * @param body 正文
         * @param contentType 媒体类型，正文非空时使用
         * @param headers 附加头
         * @return HttpResult 结果；超时或传输失败时 transportOk 为 false
         */
        HttpResult send(std::string method, std::string path, std::string body = {}, std::string contentType = {}, std::vector<std::pair<std::string, std::string>> headers = {});

        /// GET 便捷入口
        /// @param path 以 / 开头的路径
        /// @param headers 附加头
        /// @return HttpResult 结果
        HttpResult get(std::string path, std::vector<std::pair<std::string, std::string>> headers = {});

        /// POST 便捷入口
        /// @param path 以 / 开头的路径
        /// @param body 正文
        /// @param contentType 媒体类型
        /// @param headers 附加头
        /// @return HttpResult 结果
        HttpResult post(std::string path, std::string body, std::string contentType = "application/json", std::vector<std::pair<std::string, std::string>> headers = {});

    private:
        AsynGyanis::Core::EventLoop                  m_loop;            ///< 服务端与客户端共用的循环
        std::unique_ptr<AsynGyanis::Net::HttpServer> m_server;          ///< 回环 HTTP 服务器
        std::optional<AsynGyanis::Core::Task<>>      m_acceptTask;      ///< 接受协程，必须活到循环退出
        std::optional<std::jthread>                  m_loopThread;      ///< 驱动循环的线程
        std::vector<AsynGyanis::Core::Task<>>        m_pendingRequests; ///< 未确认完成的任务帧，活到夹具析构
        std::uint16_t                                m_port{0};         ///< 内核分配的端口
    };
} // namespace EdgePulse::TestSupport
