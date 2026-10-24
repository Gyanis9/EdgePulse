/**
 * @file ConfigExecutor.h
 * @brief config.apply 执行器：拉取、规范化、原子落盘、运行应用钩子并回报
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
#include <string>
#include <string_view>

namespace EdgePulse::Agent
{
    class AgentClient;

    /**
     * @brief 处理 config.apply 命令
     * @details 从服务端拉取当前配置，校验版本与正文大小，原子写入配置路径；
     *          配置了应用钩子时以配置文件路径为唯一参数运行，失败则恢复旧文件；
     *          恢复失败时保留备份并把备份路径写进失败原因。
     *          文件读写与钩子运行都在阻塞执行器上完成，事件循环线程只处理网络与协程调度。
     */
    class ConfigExecutor final : public CommandExecutor
    {
    public:
        /**
         * @brief 装配配置执行器
         * @param client 服务端客户端
         * @param configuration 已校验的配置，使用其中的配置路径、应用钩子与正文上限
         * @param nodeId 服务端分配的节点 ID
         */
        ConfigExecutor(AgentClient &client, AgentConfiguration configuration, std::int64_t nodeId);

        /**
         * @brief 是否处理 config.apply
         * @param type 命令类型
         * @return true 表示由本执行器执行
         */
        bool supports(std::string_view type) const override;

        /**
         * @brief 执行一次配置应用
         * @param loop 命令所属事件循环
         * @param command 已解析的命令，正文形如 {"version":N}
         * @return 执行结果；任何失败都带中文原因
         */
        AsynGyanis::Core::Task<CommandResult> execute(AsynGyanis::Core::EventLoop &loop, const AgentCommand &command) override;

    private:
        /**
         * @brief 把配置正文规范化并检查大小
         * @param content 服务端给出的正文
         * @return 成功返回紧凑 JSON 文本；失败返回中文原因
         */
        [[nodiscard]] std::expected<std::string, std::string> canonicalContent(const AsynGyanis::Base::ConfigValue &content) const;

        /**
         * @brief 上报结论并生成命令结果
         * @param version 配置版本
         * @param content 上报正文，必须是 JSON 对象
         * @param succeeded 应用是否成功
         * @param message 中文说明
         * @return 命令结果；上报失败时按失败返回并合并原因
         */
        AsynGyanis::Core::Task<CommandResult> finish(std::int64_t version, const AsynGyanis::Base::ConfigValue &content, bool succeeded, std::string message);

        AgentClient          &m_client;        ///< 服务端客户端
        AgentConfiguration    m_configuration; ///< 本次进程使用的配置副本
        std::int64_t          m_nodeId{0};     ///< 服务端分配的节点 ID
    };
} // namespace EdgePulse::Agent
