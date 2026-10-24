/**
 * @file ProcessRunner.h
 * @brief 阻塞式子进程执行：不经过 shell、限时、限输出
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace EdgePulse::Agent
{
    /**
     * @brief 一次子进程执行的结果
     */
    struct ProcessRunResult
    {
        bool        started{false};         ///< 是否成功创建子进程
        bool        timedOut{false};        ///< 是否因超时被终止
        bool        outputTruncated{false}; ///< 是否因超过输出上限而丢弃了内容
        int         exitCode{-1};           ///< 退出码；未启动时为 -1
        std::string output;                 ///< 标准输出与标准错误的合并文本，最多 64 KiB
        std::string failure;                ///< 中文失败原因；成功时为空

        /**
         * @brief 是否正常结束
         * @return true 启动成功、未超时且退出码为 0
         */
        [[nodiscard]] bool succeeded() const noexcept
        {
            return failure.empty();
        }
    };

    /**
     * @brief 执行一个外部程序并等待它结束
     * @param executable 可执行文件路径；Windows 上扩展名为 .cmd/.bat 时经 cmd.exe 运行，
     *                   .ps1 时经 powershell.exe 运行，其余直接创建进程
     * @param arguments 参数表，逐项传递，不拼接成 shell 命令
     * @param timeout 等待上限；超过后强制终止子进程
     * @return ProcessRunResult 启动结论、退出码、合并输出与中文失败原因
     * @note 本函数阻塞调用线程，必须经 AsyncExecutor 在事件循环之外调用
     * @note 标准输出与标准错误共用一条管道，合计最多保留 64 KiB；超出部分读取后丢弃，
     *       避免子进程写满管道而卡死
     * @note Windows 上 .cmd/.bat 的脚本路径与参数中出现双引号、CR、LF 或百分号时直接拒绝执行：
     *       cmd.exe 在引号内仍会展开 %NAME%，没有可靠的字面量转义
     */
    [[nodiscard]] ProcessRunResult runProcess(const std::filesystem::path &executable, const std::vector<std::string> &arguments, std::chrono::milliseconds timeout);
} // namespace EdgePulse::Agent
