/**
 * @file AgentClient.h
 * @brief Agent 到服务端的注册通道
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AgentConfiguration.h"
#include "AgentIdentity.h"

#include "Core/Coroutine/Task.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace AsynGyanis::Core
{
    class EventLoop;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    class HttpClient;
} // namespace AsynGyanis::Net

namespace EdgePulse::Agent
{
    /**
     * @brief 一次失败的注册，带重试与授权分类
     */
    struct RegistrationFailure
    {
        std::string message;                      ///< 面向运维的中文原因，不含令牌
        bool        retryable{false};             ///< 是否值得按退避重试
        bool        authorizationRejected{false}; ///< 服务端 403，表示令牌或指纹被拒
    };

    /**
     * @brief 持有事件循环与 TLS 身份的注册客户端
     * @details 请求走带连接池的 HttpClient；https 地址会带上客户端证书，明文地址只用于显式允许的测试配置。
     */
    class AgentClient
    {
    public:
        /**
         * @brief 装配注册地址与 TLS 客户端
         * @param loop 请求所属事件循环
         * @param configuration 已校验的配置
         * @param identity 已加载的证书身份
         * @note TLS 上下文或客户端证书装载失败不会抛出，而是在第一次注册时返回不可重试的中文原因
         */
        AgentClient(AsynGyanis::Core::EventLoop &loop, const AgentConfiguration &configuration, AgentIdentity identity);

        ~AgentClient();

        AgentClient(const AgentClient &) = delete;

        AgentClient &operator=(const AgentClient &) = delete;

        /**
         * @brief 向服务端注册本节点
         * @param enrollmentToken 一次性注册令牌；空字符串表示不发送该字段
         * @param agentVersion 当前 Agent 版本
         * @return 成功返回服务端分配的节点 ID；失败返回中文原因与重试分类
         * @note 令牌只进入请求正文，不写入日志、状态库或错误文本
         */
        AsynGyanis::Core::Task<std::expected<std::int64_t, RegistrationFailure>> registerAgent(std::string enrollmentToken, std::string agentVersion);

    private:
        AsynGyanis::Core::EventLoop                 &m_loop;                ///< 请求所属事件循环
        AgentConfiguration                           m_configuration;       ///< 本次进程使用的配置副本
        AgentIdentity                                m_identity;            ///< 证书指纹与到期时间
        std::unique_ptr<AsynGyanis::Net::HttpClient> m_client;              ///< 带连接池的 HTTP 客户端
        std::string                                  m_registerUrl;         ///< 注册接口的绝对地址
        std::string                                  m_initializationError; ///< 客户端装配失败原因，空表示可用
        std::chrono::milliseconds                    m_requestTimeout;      ///< 单次注册请求的整体时限
    };
} // namespace EdgePulse::Agent
