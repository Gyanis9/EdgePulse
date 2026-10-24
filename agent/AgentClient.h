/**
 * @file AgentClient.h
 * @brief Agent 到服务端的注册、心跳与命令通道
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AgentConfiguration.h"
#include "AgentIdentity.h"

#include "Base/Config/ConfigValue.h"
#include "Core/Coroutine/Task.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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
        int         httpStatus{0};                ///< 失败响应的 HTTP 状态码；0 表示未拿到响应
    };

    /**
     * @brief 一次心跳的结论
     */
    struct HeartbeatReply
    {
        std::int64_t desiredConfigVersion{-1};    ///< 服务端期望的配置版本；-1 表示未给出可解析值
        int          heartbeatIntervalSeconds{0}; ///< 服务端要求的间隔；0 表示未给出可解析值
    };

    /**
     * @brief 持有事件循环与 TLS 身份的 Agent 客户端
     * @details 请求走带连接池的 HttpClient；https 地址会带上客户端证书，明文地址只用于显式允许的测试配置。
     */
    class AgentClient
    {
    public:
        /**
         * @brief 装配服务端地址与 TLS 客户端
         * @param loop 请求所属事件循环
         * @param configuration 已校验的配置
         * @param identity 已加载的证书身份
         * @note TLS 上下文或客户端证书装载失败不会抛出，而是在第一次请求时返回不可重试的中文原因
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

        /**
         * @brief 发送一次心跳
         * @param nodeId 服务端分配的节点 ID
         * @param agentVersion 当前 Agent 版本
         * @return 成功返回服务端下发的配置版本与心跳间隔；失败返回可重试的中文原因
         */
        AsynGyanis::Core::Task<std::expected<HeartbeatReply, std::string>> heartbeat(std::int64_t nodeId, std::string agentVersion);

        /**
         * @brief 拉取一条待执行命令
         * @param nodeId 节点 ID
         * @return 成功时 data 为 null 表示暂无命令，否则交回命令对象；失败返回中文原因
         */
        AsynGyanis::Core::Task<std::expected<std::optional<AsynGyanis::Base::ConfigValue>, std::string>> takeNextCommand(std::int64_t nodeId);

        /**
         * @brief 上报一条命令结果
         * @param commandId 命令 ID
         * @param nodeId 节点 ID
         * @param status 状态文本，取 succeeded 或 failed
         * @param result 结果正文
         * @param message 中文说明
         * @return 服务端返回 2xx 时成功；失败返回中文原因
         */
        AsynGyanis::Core::Task<std::expected<void, std::string>> reportCommandResult(std::int64_t commandId, std::int64_t nodeId, std::string_view status, std::string_view result,
                                                                                     std::string_view message);

    private:
        AsynGyanis::Core::EventLoop                 &m_loop;                ///< 请求所属事件循环
        AgentConfiguration                           m_configuration;       ///< 本次进程使用的配置副本
        AgentIdentity                                m_identity;            ///< 证书指纹与到期时间
        std::unique_ptr<AsynGyanis::Net::HttpClient> m_client;              ///< 带连接池的 HTTP 客户端
        std::string                                  m_baseUrl;             ///< 去掉结尾斜杠的服务端基地址
        std::string                                  m_registerUrl;         ///< 注册接口的绝对地址
        std::string                                  m_heartbeatUrl;        ///< 心跳接口的绝对地址
        std::string                                  m_commandsUrl;         ///< 命令接口的绝对地址前缀
        std::string                                  m_initializationError; ///< 客户端装配失败原因，空表示可用
        std::chrono::milliseconds                    m_requestTimeout;      ///< 单次注册请求的整体时限
    };
} // namespace EdgePulse::Agent
