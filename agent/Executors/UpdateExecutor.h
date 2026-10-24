/**
 * @file UpdateExecutor.h
 * @brief agent.update 执行器：下载升级包、验签、记录待上报状态并脱离启动安装器
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AgentConfiguration.h"
#include "Executors/CommandExecutor.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace EdgePulse::Agent
{
    class AgentClient;
    class AgentState;

    /**
     * @brief 处理 agent.update 命令
     * @details 命令给出升级包的下载地址、归档摘要、manifest 原文与 Ed25519 签名。执行器先校验
     *          manifest 的版本、平台与文件清单，再用升级包信任键目录验签，然后下载归档并核对
     *          SHA-256。全部通过后确认安装器存在且可执行、清掉旧健康标记（先留快照）、写入升级
     *          状态与待上报结果，最后在阻塞执行器上把安装器作为脱离进程启动并请求当前进程停止。
     *          任何启动前失败都不改升级状态、不启动安装器，并删除暂存包；启动失败时回滚升级状态
     *          并恢复旧健康标记。安装器需要消费暂存包，成功路径不删除。
     *          文件读写与摘要计算在阻塞执行器上完成，事件循环线程只做网络与协程调度。
     */
    class UpdateExecutor final : public CommandExecutor
    {
    public:
        /**
         * @brief 脱离启动安装器的入口：可执行文件与参数表
         * @return 成功表示子进程已经创建；失败返回中文原因
         */
        using Launcher = std::function<std::expected<void, std::string>(const std::filesystem::path &, const std::vector<std::string> &)>;

        /// 请求当前进程停止的入口，可被任意线程调用
        using StopCallback = std::function<void()>;

        /**
         * @brief 装配更新执行器
         * @param client 服务端客户端，下载走它的通用正文下载
         * @param state 本地状态库，成功启动安装器前写入升级状态
         * @param configuration 已校验的配置，使用升级包信任键、安装器、暂存目录与健康时限
         * @param launcher 脱离启动入口；为空时使用生产实现（Windows CreateProcessW，Linux systemd-run）
         * @param requestStop 启动安装器后请求停止的回调；为空表示不请求
         */
        UpdateExecutor(AgentClient &client, AgentState &state, AgentConfiguration configuration, Launcher launcher = {}, StopCallback requestStop = {});

        /**
         * @brief 是否处理 agent.update
         * @param type 命令类型
         * @return true 表示由本执行器执行
         */
        bool supports(std::string_view type) const override;

        /**
         * @brief 执行一次 Agent 升级
         * @param loop 命令所属事件循环
         * @param command 已解析的命令，正文是升级包描述对象
         * @return 执行结果；失败时带中文原因，结果正文为 {}
         */
        AsynGyanis::Core::Task<CommandResult> execute(AsynGyanis::Core::EventLoop &loop, const AgentCommand &command) override;

    private:
        /**
         * @brief 升级前的健康标记快照
         */
        struct HealthMarkerSnapshot
        {
            bool        existed{false}; ///< 升级前是否存在健康标记
            std::string content;        ///< 标记原文；不存在时为空
        };

        /**
         * @brief 已校验的升级命令正文
         */
        struct UpdatePayload
        {
            std::string version;      ///< 目标版本，非空
            std::string url;          ///< 下载地址，相对 base_url 或绝对 http(s) 地址
            std::string sha256;       ///< 归档 SHA-256，小写十六进制
            std::string signature;    ///< Base64 编码的 Ed25519 签名
            std::string signingKeyId; ///< 信任键目录里的公钥文件名（不含 .pem）
            std::string algorithm;    ///< 签名算法，只允许 ed25519-sha256
            std::string manifest;     ///< 被签名的 manifest 原文
        };

        /**
         * @brief 解析并校验命令正文
         * @param payload 服务端给出的 JSON 文本
         * @return 成功返回校验后的正文；失败返回中文原因
         */
        [[nodiscard]] std::expected<UpdatePayload, std::string> parsePayload(std::string_view payload) const;

        /**
         * @brief 校验 manifest 的版本、平台、文件清单与签名声明
         * @param payload 已校验的正文
         * @return 成功为空；失败返回中文原因
         */
        [[nodiscard]] std::expected<void, std::string> validateManifest(const UpdatePayload &payload) const;

        /**
         * @brief 取暂存包路径
         * @param payload 已校验的正文
         * @return std::filesystem::path 暂存目录下的文件名，由净化后的版本与归档摘要派生
         * @note 配置未给出暂存目录时返回空路径
         */
        [[nodiscard]] std::filesystem::path stagingPath(const UpdatePayload &payload) const;

        /**
         * @brief 在阻塞执行器上删除暂存文件
         * @param loop 命令所属事件循环
         * @param path 暂存文件路径；空路径直接返回
         */
        AsynGyanis::Core::Task<> removeStagingFile(AsynGyanis::Core::EventLoop &loop, const std::filesystem::path &path);

        /**
         * @brief 删除暂存文件并交回失败结果
         * @param loop 命令所属事件循环
         * @param staging 暂存文件路径；空路径表示尚未创建
         * @param message 中文原因
         * @return 失败的命令结果
         */
        AsynGyanis::Core::Task<CommandResult> fail(AsynGyanis::Core::EventLoop &loop, const std::filesystem::path &staging, std::string message);

        /**
         * @brief 在阻塞执行器上读取并删除旧健康标记
         * @param loop 命令所属事件循环
         * @param path 健康标记路径
         * @return 成功返回标记快照；读取或删除失败返回中文原因
         * @note 只有拿到快照才允许删除；启动失败时按快照恢复
         */
        AsynGyanis::Core::Task<std::expected<HealthMarkerSnapshot, std::string>> takeHealthMarker(AsynGyanis::Core::EventLoop &loop, const std::filesystem::path &path);

        /**
         * @brief 在阻塞执行器上把健康标记恢复到升级前的状态
         * @param loop 命令所属事件循环
         * @param path 健康标记路径
         * @param snapshot 升级前的快照；原本不存在时不做任何事
         * @note 恢复失败只记 ERROR，不覆盖调用方正在返回的失败原因
         */
        AsynGyanis::Core::Task<> restoreHealthMarker(AsynGyanis::Core::EventLoop &loop, const std::filesystem::path &path, HealthMarkerSnapshot snapshot);

        AgentClient       &m_client;        ///< 服务端客户端
        AgentState        &m_state;         ///< 本地状态库
        AgentConfiguration m_configuration; ///< 本次进程使用的配置副本
        Launcher           m_launcher;      ///< 脱离启动入口
        StopCallback       m_requestStop;   ///< 请求停止的回调
    };
} // namespace EdgePulse::Agent
