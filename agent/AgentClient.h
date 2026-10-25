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
#include "Net/Http/Client/HttpClient.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
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
     * @brief 一次命令结果上报的处置分类
     */
    enum class ResultPostDisposition
    {
        Accepted,  ///< 服务端已接受，命令收口
        Retryable, ///< 传输失败、408、425、429 或 5xx，稍后重试
        Terminal   ///< 其余 4xx，服务端不会再接受该结果
    };

    /**
     * @brief 一次命令结果上报的结论
     */
    struct ResultPostOutcome
    {
        ResultPostDisposition disposition{ResultPostDisposition::Retryable}; ///< 处置分类
        int                   httpStatus{0};                                 ///< HTTP 状态码；0 表示没有拿到响应
        std::string           message;                                       ///< 中文原因，供日志使用
    };

    /**
     * @brief 一次固件下载的结论
     */
    struct DownloadResult
    {
        int         httpStatus{0}; ///< 响应状态码；只有 200 表示正文已完整交出
        std::string message;       ///< 服务端说明或固定的中文文案
    };

    /**
     * @brief 一次遥测上报的失败，带重试分类
     */
    struct TelemetryFailure
    {
        std::string message;          ///< 中文原因
        bool        retryable{false}; ///< 传输失败、408、425、429 与 5xx 为 true
        int         httpStatus{0};    ///< HTTP 状态码；0 表示没有拿到响应
    };

    /**
     * @brief 服务端当前下发的配置
     */
    struct AgentConfigSnapshot
    {
        std::int64_t                  version{0}; ///< 期望配置版本；0 表示从未下发
        AsynGyanis::Base::ConfigValue content;    ///< 配置正文，JSON 对象
        std::string                   hash;       ///< 服务端给出的摘要
    };

    /**
     * @brief 服务端签发的一张新证书
     */
    struct RenewedCertificate
    {
        std::string certificatePem; ///< 证书 PEM，可能含中间证书
        std::string fingerprint;    ///< 叶证书 SHA-256 指纹，64 位小写十六进制
        std::string notBefore;      ///< 生效时间，服务端给出的 UTC 文本
        std::string notAfter;       ///< 到期时间，服务端给出的 UTC 文本
        std::string serial;         ///< 序列号，小写十六进制
    };

    /**
     * @brief 一次证书续期请求的失败，带重试与授权分类
     */
    struct CertificateRenewalFailure
    {
        std::string message;                      ///< 面向运维的中文原因，不含 CSR 与私钥
        bool        retryable{false};             ///< 是否值得按退避重试
        bool        authorizationRejected{false}; ///< 服务端 403，表示节点证书或租户被拒
        int         httpStatus{0};                ///< 失败响应的 HTTP 状态码；0 表示未拿到响应
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
         * @return 200 返回 Accepted；传输失败与 408、425、429、5xx 返回 Retryable；其余状态返回 Terminal
         */
        AsynGyanis::Core::Task<ResultPostOutcome> reportCommandResult(std::int64_t commandId, std::int64_t nodeId, std::string_view status, std::string_view result,
                                                                      std::string_view message);

        /**
         * @brief 上报一批遥测样本
         * @param nodeId 节点标识
         * @param samples 样本对象数组，1 到 500 条，每项形如 {metric,value,unit,collectedAt}
         * @return 只有 HTTP 200 表示服务端已接受；失败带中文原因与重试分类
         * @note 传输失败与 408、425、429、5xx 可重试；其余状态重试同一批不会改变结论
         */
        AsynGyanis::Core::Task<std::expected<void, TelemetryFailure>> sendTelemetry(std::int64_t nodeId, const AsynGyanis::Base::ConfigValue &samples);

        /**
         * @brief 拉取服务端当前下发的配置
         * @param nodeId 节点 ID
         * @return 成功返回版本、正文与摘要；失败返回中文原因
         * @note 版本 0 与空对象表示服务端从未下发过配置
         */
        AsynGyanis::Core::Task<std::expected<AgentConfigSnapshot, std::string>> fetchConfig(std::int64_t nodeId);

        /**
         * @brief 上报一次配置应用结果
         * @param nodeId 节点 ID
         * @param version 配置版本
         * @param content 配置正文，必须是 JSON 对象
         * @param status 上报状态，只允许 applied 或 failed
         * @param message 中文说明
         * @return 成功为空；失败返回中文原因
         */
        AsynGyanis::Core::Task<std::expected<void, std::string>> reportConfig(std::int64_t nodeId, std::int64_t version, const AsynGyanis::Base::ConfigValue &content,
                                                                              std::string_view status, std::string_view message);

        /**
         * @brief 下载一份正文，按到达批次交给接收口
         * @param downloadPath 服务端给出的下载地址：以 / 开头时相对 base_url，否则必须是绝对地址
         * @param receiver 正文接收口，只在事件循环线程上调用
         * @return 成功返回状态码与服务端说明；传输失败或地址不合法时返回中文原因
         * @note 绝对地址只接受 https；明文 http 仅在配置开启 allow_plaintext_for_tests 时放行
         * @note 非 200 响应同样按成功返回并带上状态码，由调用方决定是否继续；接收口应自行拒绝非 200 正文
         */
        AsynGyanis::Core::Task<std::expected<DownloadResult, std::string>> downloadFile(std::string downloadPath, AsynGyanis::Net::HttpResponseBodyReceiver receiver);

        /**
         * @brief 上报一次固件任务状态
         * @param firmwareId 固件标识
         * @param nodeId 节点标识
         * @param status 状态文本，取 pending、downloading、installing、installed 或 failed
         * @param progress 进度百分比，0 到 100
         * @param message 中文说明
         * @return 成功为空；失败返回中文原因
         * @note 调用方按尽力而为处理：上报失败不影响下载与安装的结论
         */
        AsynGyanis::Core::Task<std::expected<void, std::string>> reportFirmwareStatus(std::int64_t firmwareId, std::int64_t nodeId, std::string_view status, int progress,
                                                                                      std::string_view message);

        /**
         * @brief 提交一份 CSR 换取新证书
         * @param nodeId 服务端分配的节点 ID
         * @param csrPem 本次生成的证书签名请求 PEM
         * @return 成功返回新证书与元数据；失败带中文原因与重试分类
         * @note 只有 403 表示节点证书已吊销或租户停用，按终止处理；400、404 与其他非 403 响应
         *       以及正文校验失败都标记为可重试，由续期循环决定退避节奏。请求与响应正文都不写入日志
         */
        AsynGyanis::Core::Task<std::expected<RenewedCertificate, CertificateRenewalFailure>> renewCertificate(std::int64_t nodeId, std::string csrPem);

        /**
         * @brief 在事件循环线程上重新装载磁盘上的客户端证书与私钥
         * @param restoreBackups 装载失败时的回退动作；返回 true 表示已把旧文件写回
         * @return 装载成功返回 true；失败返回 false，调用方应稍后再试
         * @details 候选客户端连同新证书与私钥一起构造并验证，全部通过后才在循环线程上整体替换；
         *          构造失败时旧客户端与旧身份原样保留，不会出现半换的状态。成功后更新身份并收掉
         *          旧客户端的空闲连接，后续请求按新身份重新握手；在途请求继续持有旧客户端直到完成。
         *          失败时（可选）执行回退动作，再用回退后的文件重建一次客户端。
         * @note 必须在客户端所属事件循环线程上调用；替换动作只改写本对象持有的客户端指针
         */
        bool reloadIdentityFromDisk(const std::function<bool()> &restoreBackups = {});

        /**
         * @brief 取当前生效的证书身份
         * @return const AgentIdentity & 指纹与到期时间；重载成功后随之更新
         */
        [[nodiscard]] const AgentIdentity &identity() const noexcept
        {
            return m_identity;
        }

    private:
        /**
         * @brief 按当前配置重建接口地址与单次请求时限
         */
        void configureEndpoints();

        /**
         * @brief 按当前配置构造一个可用的 HTTP 客户端
         * @return 成功返回新客户端；地址、TLS 策略或客户端身份装载失败时返回中文原因
         * @details 重建的是整个客户端而不是只换证书：连接池、TLS 策略与客户端身份都随新实例一起
         *          建立，调用方拿到的是一个完整可用的对象，不会观察到换到一半的状态
         */
        [[nodiscard]] std::expected<std::shared_ptr<AsynGyanis::Net::HttpClient>, std::string> createClient() const;

        AsynGyanis::Core::EventLoop                 &m_loop;                     ///< 请求所属事件循环
        AgentConfiguration                           m_configuration;            ///< 本次进程使用的配置副本
        AgentIdentity                                m_identity;                 ///< 证书指纹与到期时间
        std::shared_ptr<AsynGyanis::Net::HttpClient> m_client;                   ///< 带连接池的 HTTP 客户端；换证时整体替换
        std::string                                  m_baseUrl;                  ///< 去掉结尾斜杠的服务端基地址
        std::string                                  m_registerUrl;              ///< 注册接口的绝对地址
        std::string                                  m_heartbeatUrl;             ///< 心跳接口的绝对地址
        std::string                                  m_telemetryUrl;             ///< 遥测接口的绝对地址
        std::string                                  m_commandsUrl;              ///< 命令接口的绝对地址前缀
        std::string                                  m_agentNodesUrl;            ///< Agent 节点接口的绝对地址前缀
        std::string                                  m_firmwareUrl;              ///< 固件任务接口的绝对地址前缀
        std::string                                  m_renewalUrl;               ///< 证书续期接口的绝对地址
        std::string                                  m_initializationError;      ///< 客户端装配失败原因，空表示可用
        bool                                         m_tlsIdentityLoaded{false}; ///< 是否在 TLS 上下文上装载了客户端证书
        std::chrono::milliseconds                    m_requestTimeout{30000};    ///< 单次请求的整体时限
    };
} // namespace EdgePulse::Agent
