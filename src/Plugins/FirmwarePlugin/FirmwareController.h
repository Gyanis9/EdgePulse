/**
 * @file FirmwareController.h
 * @brief 固件上传、下载与下发 HTTP 路由
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Plugins/CommandPlugin/CommandController.h"

#include <cstddef>
#include <memory>
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
namespace EdgePulse::Command
{
    class CommandRepository;
} // namespace EdgePulse::Command
namespace EdgePulse::Device
{
    class NodeRepository;
}
namespace EdgePulse::System
{
    class TenantRepository;
}
namespace EdgePulse::Web
{
    class RealtimeBus;
}
namespace EdgePulse::Firmware
{
    class FirmwareRepository;
    class FirmwareRolloutService;
    struct FirmwareSigningPolicy;
} // namespace EdgePulse::Firmware

namespace EdgePulse::Firmware
{
    /**
     * @brief 固件路由依赖
     */
    struct FirmwareDependencies
    {
        Auth::AuthService                     *auth{nullptr};                            ///< 认证
        FirmwareRepository                    *firmware{nullptr};                        ///< 固件元数据
        Device::NodeRepository                *nodes{nullptr};                           ///< 节点
        Command::CommandRepository            *commands{nullptr};                        ///< 命令
        Web::RealtimeBus                      *realtimeBus{nullptr};                     ///< Agent 命令总线
        Command::CommandNotifier               notifier;                                 ///< 浏览器通知
        System::TenantRepository              *tenants{nullptr};                         ///< 租户，用于校验平台管理员指定的归属
        std::string                            storageDirectory{"var/firmware"};         ///< 落盘目录
        std::size_t                            maximumUploadBytes{256U * 1024U * 1024U}; ///< 上传上限
        std::shared_ptr<FirmwareSigningPolicy> signingPolicy;                            ///< 签名门禁；为空时按不强制签名处理
        FirmwareRolloutService                *rollouts{nullptr};                        ///< 灰度服务；为空时不注册灰度路由
    };

    /**
     * @brief 注册浏览器固件路由
     * @param router 目标路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 依赖
     */
    void registerFirmwareRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const FirmwareDependencies &dependencies);

    /**
     * @brief 注册 Agent 固件下载与状态路由
     * @param router mTLS 路由器
     * @param loop 服务器所属事件循环
     * @param dependencies 依赖
     */
    void registerFirmwareAgentRoutes(AsynGyanis::Net::Router &router, AsynGyanis::Core::EventLoop &loop, const FirmwareDependencies &dependencies);
} // namespace EdgePulse::Firmware
