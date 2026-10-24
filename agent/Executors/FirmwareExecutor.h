/**
 * @file FirmwareExecutor.h
 * @brief firmware.update 执行器：流式下载、SHA-256 与 Ed25519 验签、运行安装钩子并回报
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
#include <string>
#include <string_view>

namespace EdgePulse::Agent
{
    class AgentClient;
    class AgentState;

    /**
     * @brief 处理 firmware.update 命令
     * @details 正文流式写入暂存目录，每批字节在阻塞执行器上落盘并更新 SHA-256；收齐后核对字节数
     *          与摘要，再用信任键目录里的 Ed25519 公钥验签，最后以「文件、版本、摘要」三个参数运行
     *          安装钩子。任何一步失败都上报 failed 并删除暂存文件，已记录的固件版本保持不变。
     *          文件读写、摘要计算与子进程都在阻塞执行器上完成，事件循环线程只做网络与协程调度。
     */
    class FirmwareExecutor final : public CommandExecutor
    {
    public:
        /**
         * @brief 装配固件执行器
         * @param client 服务端客户端
         * @param state 本地状态库，安装成功后写入 installed_firmware_version
         * @param configuration 已校验的配置，使用其中的安装钩子、信任键目录与暂存目录
         * @param nodeId 服务端分配的节点标识
         */
        FirmwareExecutor(AgentClient &client, AgentState &state, AgentConfiguration configuration, std::int64_t nodeId);

        /**
         * @brief 是否处理 firmware.update
         * @param type 命令类型
         * @return true 表示由本执行器执行
         */
        bool supports(std::string_view type) const override;

        /**
         * @brief 执行一次固件更新
         * @param loop 命令所属事件循环
         * @param command 已解析的命令，正文是服务端给出的固件描述对象
         * @return 执行结果；失败时带中文原因，结果正文为 {}
         */
        AsynGyanis::Core::Task<CommandResult> execute(AsynGyanis::Core::EventLoop &loop, const AgentCommand &command) override;

    private:
        /**
         * @brief 已校验的固件命令正文
         */
        struct FirmwarePayload
        {
            std::int64_t firmwareId{0};     ///< 固件标识，正整数
            std::string  version;           ///< 固件版本，非空
            std::string  sha256;            ///< 小写 SHA-256 十六进制文本，64 个字符
            std::uint64_t sizeBytes{0};     ///< 固件字节数，正整数
            std::string  downloadPath;      ///< 下载地址，相对 base_url 或以 / 开头的路径
            std::string  signature;         ///< Base64 编码的 Ed25519 签名
            std::string  signingKeyId;      ///< 信任键目录里的公钥文件名（不含 .pem）
        };

        /**
         * @brief 解析并校验命令正文
         * @param payload 服务端给出的 JSON 文本
         * @return 成功返回校验后的正文；失败返回中文原因
         */
        [[nodiscard]] std::expected<FirmwarePayload, std::string> parsePayload(std::string_view payload) const;

        /**
         * @brief 取暂存文件路径
         * @param payload 已校验的正文
         * @return std::filesystem::path 暂存目录下的文件名，只由固件标识与摘要派生
         * @note 配置未给出暂存目录时返回空路径
         */
        [[nodiscard]] std::filesystem::path stagingPath(const FirmwarePayload &payload) const;

        /**
         * @brief 上报一条状态，失败只记 WARN
         * @param firmwareId 固件标识
         * @param status 状态文本
         * @param progress 进度百分比
         * @param message 中文说明
         */
        AsynGyanis::Core::Task<> reportStatus(std::int64_t firmwareId, std::string_view status, int progress, std::string message);

        /**
         * @brief 上报失败并删除暂存文件
         * @param loop 命令所属事件循环
         * @param payload 已校验的正文
         * @param staging 暂存文件路径；空路径表示尚未创建
         * @param message 中文原因
         * @return 失败的命令结果
         */
        AsynGyanis::Core::Task<CommandResult> fail(AsynGyanis::Core::EventLoop &loop, const FirmwarePayload &payload, const std::filesystem::path &staging,
                                                   std::string message);

        AgentClient       &m_client;        ///< 服务端客户端
        AgentState        &m_state;         ///< 本地状态库
        AgentConfiguration m_configuration; ///< 本次进程使用的配置副本
        std::int64_t       m_nodeId{0};     ///< 服务端分配的节点标识
    };
} // namespace EdgePulse::Agent
