/**
 * @file AgentApplication.h
 * @brief Agent 进程的装配入口
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AgentCertificateManager.h"
#include "AgentClient.h"
#include "AgentConfiguration.h"
#include "AgentIdentity.h"

#include "Core/Coroutine/Task.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
} // namespace AsynGyanis::Core

namespace EdgePulse::Agent
{
    class AgentState;

    /**
     * @brief 证书续期循环依赖的外部动作
     * @details 生产装配把生成、续期、安装与两条通道的重载接到真实对象上；用例注入替身即可在
     *          不联网、不落盘的前提下核对调度与退避。
     */
    struct CertificateRenewalDependencies
    {
        std::string                                                             expiresAt;       ///< 当前证书到期时间（UTC，形如 2027-01-01T00:00:00Z）
        std::string                                                             notBefore;       ///< 当前证书生效时间，与 expiresAt 一起决定续期窗口；空表示生命周期未知
        std::function<std::expected<CertificateRequestMaterial, std::string>()> generateRequest; ///< 生成新私钥与 CSR
        std::function<AsynGyanis::Core::Task<std::expected<RenewedCertificate, CertificateRenewalFailure>>(std::int64_t, std::string)> renew;              ///< 提交 CSR
        std::function<std::expected<AgentIdentity, std::string>(std::string_view)>                                                     install;            ///< 安装新证书
        std::function<bool()>                                                                                                          reloadHttpIdentity; ///< 重载 HTTP 通道身份
        std::function<bool()>                                  reloadWebSocketIdentity;         ///< 重载 WebSocket 通道身份
        std::function<void()>                                  commitInstalledIdentity;         ///< 两条通道都重载后清理安装备份
        std::function<std::chrono::system_clock::time_point()> now;                             ///< 当前时刻，空表示系统时钟
        std::chrono::milliseconds                              checkInterval{0};                ///< 检查间隔覆盖值，0 表示按配置
        std::chrono::milliseconds                              websocketReloadRetryInterval{0}; ///< WebSocket 重载复查间隔，0 表示 30 秒
        std::vector<std::chrono::milliseconds>                 retryBackoff;                    ///< 失败退避表，空表示 5、10、30、60、300 秒
        std::function<void(std::chrono::milliseconds)>         waitObserver;                    ///< 每次等待前收到计划时长
    };

    /**
     * @brief 按配置装配 Agent 运行期资源并常驻运行
     * @details 负责状态目录、日志、证书身份、状态库与注册；注册成功后在同一条事件循环上
     *          运行心跳、WebSocket 命令通道、轮询兜底、遥测与证书自动续期，直到收到停止请求。
     */
    class AgentApplication
    {
    public:
        /**
         * @brief 保存一份已校验的配置
         * @param configuration 已通过形态校验的配置
         * @param consoleMode 是否以控制台方式运行；为 true 时接管 SIGINT/SIGTERM 并转为停止请求
         */
        explicit AgentApplication(AgentConfiguration configuration, bool consoleMode = false);

        /**
         * @brief 装配日志与身份、打开状态库、完成首次注册并常驻运行
         * @return int 正常停止返回 0；目录、日志、身份、状态库或注册失败返回 1
         * @note 状态库已有节点标识时优先使用换绑令牌；网络、5xx 与少量 409 失败按退避重试；
         *       403 立即停止，409 累计 3 次后按名字冲突停止并记录 ERROR
         */
        int run();

        /**
         * @brief 请求停止常驻运行
         * @note 可被任意线程调用；在注册阶段调用时，注册完成后的常驻段会直接跳过
         */
        void requestStop();

        /**
         * @brief 按证书到期时间自动续期并热切换 HTTP 与 WebSocket 身份
         * @param loop 事件循环
         * @param state 本地状态库，成功后写入 cert_fingerprint 与 cert_expiry
         * @param nodeId 服务端分配的节点 ID
         * @param configuration 已校验的配置，提供提前天数与检查间隔
         * @param dependencies 生成、续期、安装与重载动作
         * @param stopRequested 停止标记；等待按一秒切片，置位后最迟一秒内退出
         * @return 协程任务
         * @details 每轮先按当前证书的 notBefore/notAfter 计算续期窗口 max(25% 生命周期, 24h)，
         *          未到点就等待下一次检查；到点后生成新密钥与 CSR，换回证书并校验安装。签发结果与
         *          待装私钥会保留到安装成功：瞬时安装失败按依赖退避重试同一份材料，不会重新签发，
         *          避免服务端 active 被反复轮换而挤掉 Agent 实际持有的证书；证书链、密钥配对或
         *          有效期这类不可重试的安装错误记录可操作原因后停止续期。安装成功后重载 HTTP 与
         *          WebSocket；任一通道失败都不回退文件，而是保留新证书并按
         *          dependencies.websocketReloadRetryInterval（默认 30 秒）复查两条通道，成功日志只在
         *          两条通道都换好之后才写。只有 403 与调用参数错误记录 ERROR 后终止本循环，注册、
         *          心跳、遥测与命令通道不受影响。循环体有异常兜底，依赖抛出的异常不会结束协程。
         *          配置的 renew_before_days 是可选覆盖值，只在小于证书生命周期时生效，否则记录
         *          ERROR 并回退到规范窗口。
         * @note 日志只写指纹、到期时间与失败原因，不写私钥与 CSR 正文
         */
        static AsynGyanis::Core::Task<> runCertificateRenewal(AsynGyanis::Core::EventLoop &loop, AgentState &state, std::int64_t nodeId, const AgentConfiguration &configuration,
                                                              CertificateRenewalDependencies dependencies, const std::atomic<bool> &stopRequested);

    private:
        /**
         * @brief 首次成功心跳后确认一次升级结果
         * @param loop 事件循环，健康标记与升级标记的读写都在阻塞执行器上执行
         * @param state 本地状态库
         * @param agentVersion 当前进程版本，写入健康标记并用于核对升级目标版本
         * @return 协程任务
         * @details 没有待上报的升级结果时清理残留的回滚标记；若安装器写下了 update-expected，
         *          内容与当前运行版本一致就写健康标记并删除该标记，不一致则记 WARN 并保留标记，
         *          由安装器按超时回滚。存在待上报结果时按目标版本、回滚标记与运行版本判定成败：
         *          回滚标记存在或目标版本与当前运行版本不一致时按失败上报并删除回滚标记，不写健康
         *          标记；否则写健康标记、记录确认时刻并按成功上报，同时删除内容匹配的 update-expected。
         *          失败与成功都整行替换同一命令此前入队的结果。待上报内容损坏时记 ERROR 并清除，
         *          避免每次启动重复处理同一行。
         */
        AsynGyanis::Core::Task<> confirmUpdateHealth(AsynGyanis::Core::EventLoop &loop, AgentState &state, std::string agentVersion);

        AgentConfiguration           m_configuration;        ///< 本次进程使用的配置副本
        bool                         m_consoleMode{false};   ///< 是否接管控制台停机信号
        std::atomic<bool>            m_stopRequested{false}; ///< 是否已请求停止
        std::mutex                   m_loopMutex;            ///< 保护常驻循环指针与停止调用
        AsynGyanis::Core::EventLoop *m_loop{nullptr};        ///< 常驻事件循环，仅在 run() 期间有效；受 m_loopMutex 保护
    };
} // namespace EdgePulse::Agent
