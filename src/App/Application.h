/**
 * @file Application.h
 * @brief 进程级装配、监听器与生命周期
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "App/ApplicationConfiguration.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"

#include <atomic>
#include <thread>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <Core/Coroutine/Task.h>

namespace AsynGyanis::Core
{
    class EventLoop;
    class IoContext;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    class HttpServer;
    class HttpsServer;
    class OtlpHttpMetricsExporter;
    class QuicServer;
    class Router;
} // namespace AsynGyanis::Net
namespace AsynGyanis::Core
{
    class ProcessMetricHandle;
} // namespace AsynGyanis::Core

namespace EdgePulse::Auth
{
    class AuthService;
    class CaptchaService;
    class DataScopeService;
    class LoginAudit;
    class OidcService;
    class SessionStore;
} // namespace EdgePulse::Auth

namespace EdgePulse::Common
{
    class Database;
}

namespace EdgePulse::Cache
{
    class KeyValueStore;
}

namespace EdgePulse::Device
{
    class AgentRegistry;
    class EnrollmentRepository;
    class EnrollmentService;
    class NodeConfigRepository;
    class NodeConfigService;
    class NodeGroupRepository;
    class NodeLifecycleService;
    class NodeRepository;
    class NodeService;
    struct NodeRecord;
} // namespace EdgePulse::Device

namespace EdgePulse::System
{
    class DictionaryRepository;
    class TenantRepository;
    class LogRepository;
    class MenuRepository;
    class OrgRepository;
    class RoleRepository;
    class UserRepository;
} // namespace EdgePulse::System

namespace EdgePulse::Notification
{
    class NotificationDispatcher;
    class NotificationRepository;
    class NotificationService;
} // namespace EdgePulse::Notification

namespace EdgePulse::Telemetry
{
    class TelemetryRepository;
    class UdpTelemetryEndpoint;
} // namespace EdgePulse::Telemetry
namespace EdgePulse::Alarm
{
    class AlarmRepository;
    class AlarmService;
} // namespace EdgePulse::Alarm
namespace EdgePulse::Firmware
{
    class FirmwareRepository;
}
namespace EdgePulse::Command
{
    class AgentChannelRegistry;
    class CommandRepository;
    struct CommandRecord;
} // namespace EdgePulse::Command
namespace EdgePulse::Ops
{
    class OpsRepository;
} // namespace EdgePulse::Ops

namespace EdgePulse::Web
{
    class BrowserWebSocket;
    class RealtimeBus;
    class SseBroadcaster;
} // namespace EdgePulse::Web

namespace EdgePulse::App
{
    /**
     * @brief 进程级装配：事件循环、监听器、协程生命周期与停止请求
     */
    class Application
    {
    public:
        /**
         * @brief 用已加载的配置构造应用（尚未绑定端口）
         * @param configuration 已校验的配置
         */
        explicit Application(ApplicationConfiguration configuration);

        ~Application();

        Application(const Application &)            = delete;
        Application &operator=(const Application &) = delete;

        /**
         * @brief 建立事件循环与 8080 监听器，并把接受协程投递到循环
         * @param error 失败原因
         * @return bool 是否启动成功
         * @note 必须在 run() 之前调用；监听器在此时尚未开始接受连接
         */
        bool start(std::string &error);

        /**
         * @brief 阻塞运行事件循环，直到 requestStop() 或进程收到停止信号
         * @return int 进程退出码
         */
        int run();

        /**
         * @brief 请求停止事件循环；可从非循环线程调用
         */
        void requestStop();

        /**
         * @brief 读回实际监听端口；端口 0 时用于获取内核分配的端口
         * @return std::uint16_t 端口，未启动时为 0
         */
        [[nodiscard]] std::uint16_t httpPort() const noexcept;

    private:
        /**
         * @brief 把健康、认证、节点、WebSocket 与兜底路由挂到一台公开监听器上
         * @param router 目标路由器
         * @param loop 服务器所属事件循环
         */
        void registerPublicRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 周期推进到期的计划命令
         * @param loop 服务器所属事件循环
         * @return AsynGyanis::Core::Task<> 周期协程，活到应用析构
         */
        AsynGyanis::Core::Task<> runCommandScheduler(AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 周期按保留天数清理遥测、日志与投递记录
         * @param loop 服务器所属事件循环
         * @return AsynGyanis::Core::Task<> 周期协程，活到应用析构
         */
        AsynGyanis::Core::Task<> runDataRetention(AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 周期扫描心跳超时的节点并刷新业务指标
         * @param loop 服务器所属事件循环
         * @return AsynGyanis::Core::Task<> 周期协程，活到应用析构
         */
        AsynGyanis::Core::Task<> runFleetLifecycle(AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 把节点状态推给实时总线
         * @param node 待推送的节点
         * @return AsynGyanis::Core::Task<> 推送完成即返回
         */
        AsynGyanis::Core::Task<> publishNodeStatus(const Device::NodeRecord &node);

        /**
         * @brief 把命令状态推给实时总线
         * @param command 待推送的命令
         * @return AsynGyanis::Core::Task<> 推送完成即返回
         */
        AsynGyanis::Core::Task<> publishCommandStatus(const Command::CommandRecord &command);

    private:
        ApplicationConfiguration                                  m_configuration;
        std::filesystem::path                                     m_webRoot; ///< 前端构建产物目录
        std::unique_ptr<AsynGyanis::Core::IoContext>              m_ioContext;
        std::shared_ptr<Common::Database>                         m_database;               ///< 进程级数据库
        std::shared_ptr<Cache::KeyValueStore>                     m_cache;                  ///< 会话与验证码存储
        std::shared_ptr<Auth::SessionStore>                       m_sessionStore;           ///< 会话表
        std::shared_ptr<Auth::CaptchaService>                     m_captchaService;         ///< 验证码
        std::shared_ptr<Auth::LoginAudit>                         m_loginAudit;             ///< 登录审计
        std::shared_ptr<Auth::AuthService>                        m_authService;            ///< 认证服务
        std::unique_ptr<Auth::OidcService>                        m_oidcService;            ///< OIDC 登录
        std::shared_ptr<Web::BrowserWebSocket>                    m_browserWebSocket;       ///< 浏览器 WebSocket 与节点扇出
        std::shared_ptr<Device::NodeRepository>                   m_nodeRepository;         ///< 节点仓储
        std::shared_ptr<Device::NodeRepository>                   m_fleetNodes;             ///< 离线扫描用的全租户节点视图
        std::shared_ptr<Device::EnrollmentRepository>             m_enrollmentRepository;   ///< 一次性注册令牌仓储
        std::shared_ptr<Device::EnrollmentService>                m_enrollmentService;      ///< 一次性注册令牌服务
        std::shared_ptr<Device::NodeService>                      m_nodeService;            ///< 节点服务
        std::shared_ptr<Device::NodeConfigRepository>             m_nodeConfigRepository;   ///< 配置影子仓储
        std::shared_ptr<Device::NodeConfigService>                m_nodeConfigService;      ///< 配置影子服务
        std::shared_ptr<Device::AgentRegistry>                    m_agentRegistry;          ///< Agent 注册表
        std::shared_ptr<Device::NodeLifecycleService>             m_lifecycle;              ///< 节点在线转换与离线扫描
        std::shared_ptr<System::UserRepository>                   m_userRepository;         ///< 用户仓储
        std::shared_ptr<System::RoleRepository>                   m_roleRepository;         ///< 角色仓储
        std::shared_ptr<System::MenuRepository>                   m_menuRepository;         ///< 菜单仓储
        std::shared_ptr<System::OrgRepository>                    m_orgRepository;          ///< 部门与岗位
        std::shared_ptr<System::DictionaryRepository>             m_dictionaryRepository;   ///< 字典与参数
        std::shared_ptr<System::TenantRepository>                 m_tenantRepository;       ///< 租户
        std::shared_ptr<System::LogRepository>                    m_logRepository;          ///< 日志与统计
        std::shared_ptr<Auth::DataScopeService>                   m_dataScopeService;       ///< 数据范围
        std::shared_ptr<Device::NodeGroupRepository>              m_nodeGroupRepository;    ///< 节点分组
        std::shared_ptr<Telemetry::TelemetryRepository>           m_telemetryRepository;    ///< 遥测
        std::unique_ptr<Telemetry::UdpTelemetryEndpoint>          m_udpTelemetry;           ///< UDP 遥测监听
        std::optional<AsynGyanis::Core::Task<>>                   m_udpListen;              ///< UDP 收包协程
        std::shared_ptr<Alarm::AlarmRepository>                   m_alarmRepository;        ///< 告警
        std::shared_ptr<Alarm::AlarmService>                      m_alarmService;           ///< 告警评估
        std::shared_ptr<Notification::NotificationRepository>     m_notificationRepository; ///< 通知渠道
        std::shared_ptr<Notification::NotificationService>        m_notificationService;    ///< 通知投递
        std::shared_ptr<Notification::NotificationDispatcher>     m_notificationDispatcher; ///< 通知队列
        std::shared_ptr<Firmware::FirmwareRepository>             m_firmwareRepository;     ///< 固件
        std::shared_ptr<Command::CommandRepository>               m_commandRepository;      ///< 命令
        std::optional<AsynGyanis::Core::Task<>>                   m_commandScheduler;       ///< 计划命令推进协程
        std::optional<AsynGyanis::Core::Task<>>                   m_fleetLifecycle;         ///< 离线扫描与指标刷新协程
        std::optional<AsynGyanis::Core::Task<>>                   m_dataRetention;          ///< 数据保留期清理协程
        std::shared_ptr<Command::AgentChannelRegistry>            m_agentChannels;          ///< Agent 长连接
        std::shared_ptr<Ops::OpsRepository>                       m_opsRepository;          ///< 运维总览
        std::shared_ptr<Web::SseBroadcaster>                      m_sseBroadcaster;         ///< 告警 SSE
        std::unique_ptr<Web::RealtimeBus>                         m_realtimeBus;            ///< 实时事件跨实例总线
        std::unique_ptr<AsynGyanis::Net::Router>                  m_h3Router;               ///< h3 专属路由器
        std::unique_ptr<AsynGyanis::Net::HttpServer>              m_httpServer;             ///< 8080 开发口
        std::optional<AsynGyanis::Core::Task<>>                   m_acceptTask;             ///< 8080 接受协程
        std::unique_ptr<AsynGyanis::Net::HttpsServer>             m_httpsServer;            ///< 8443 控制台
        std::optional<AsynGyanis::Core::Task<>>                   m_httpsAccept;            ///< 8443 接受协程
        std::unique_ptr<AsynGyanis::Net::QuicServer>              m_quicServer;             ///< 8443/UDP h3
        std::optional<AsynGyanis::Core::Task<>>                   m_quicListen;             ///< h3 监听协程
        std::unique_ptr<AsynGyanis::Net::HttpsServer>             m_agentServer;            ///< 9443 mTLS
        std::optional<AsynGyanis::Core::Task<>>                   m_agentAccept;            ///< 9443 接受协程
        std::unique_ptr<AsynGyanis::Net::HttpServer>              m_opsServer;              ///< 9100 运维口
        std::optional<AsynGyanis::Core::Task<>>                   m_opsAccept;              ///< 9100 接受协程
        std::optional<AsynGyanis::Core::Task<>>                   m_shutdownTask;           ///< 排空协程，活到应用析构
        std::optional<std::jthread>                               m_stopThread;             ///< 从池外叫停 IoContext 的线程
        std::jthread                                              m_healthProbe;            ///< 数据库健康探针
        std::unique_ptr<AsynGyanis::Net::OtlpHttpMetricsExporter> m_metricsExporter;        ///< OTLP 读数出口
        std::optional<AsynGyanis::Core::ProcessMetricHandle>      m_buildInfoMetric;        ///< 构建信息读数
        std::atomic<bool>                                         m_ready{false};           ///< 就绪探针的对外状态
        std::atomic<bool>                                         m_databaseReady{true};    ///< 最近一轮数据库探针
        std::atomic<bool>                                         m_stopRequested{false};
    };
} // namespace EdgePulse::App
