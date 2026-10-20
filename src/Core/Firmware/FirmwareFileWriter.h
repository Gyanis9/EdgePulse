/**
 * @file FirmwareFileWriter.h
 * @brief 固件上传的流式落盘与 SHA-256 计算
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Firmware
{
    /**
     * @brief 把上传分段写入临时文件并增量计算摘要
     * @details 每次写入都移到工作线程执行，避免 100 MiB 固件占住事件循环；
     *          调用方必须按顺序 co_await，State 内部不再加锁。
     */
    class FirmwareFileWriter
    {
    public:
        /**
         * @brief 打开临时文件与摘要上下文
         * @param loop 恢复协程用的事件循环
         * @param temporaryPath 临时文件路径
         * @param finalPath 完成后重命名到的路径
         * @return std::shared_ptr<FirmwareFileWriter> 写入器
         * @throws Common::EdgePulseException 目录或文件无法创建
         */
        [[nodiscard]] static AsynGyanis::Core::Task<std::shared_ptr<FirmwareFileWriter>> open(AsynGyanis::Core::EventLoop &loop, std::string temporaryPath, std::string finalPath);

        ~FirmwareFileWriter();

        FirmwareFileWriter(const FirmwareFileWriter &)            = delete;
        FirmwareFileWriter &operator=(const FirmwareFileWriter &) = delete;

        /**
         * @brief 追加一段字节
         * @param loop 恢复协程用的事件循环
         * @param data 字节，按值收，协程恢复前不会失效
         * @return bool 是否写入成功
         */
        AsynGyanis::Core::Task<bool> append(AsynGyanis::Core::EventLoop &loop, std::string data);

        /**
         * @brief 收尾：关闭文件、重命名并返回摘要
         * @param loop 恢复协程用的事件循环
         * @return std::optional<std::string> 十六进制 SHA-256；失败为空
         */
        [[nodiscard]] AsynGyanis::Core::Task<std::optional<std::string>> finish(AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 放弃上传并删除临时文件
         */
        void abort() noexcept;

        /**
         * @brief 已写入字节数
         * @return std::int64_t 字节数
         */
        [[nodiscard]] std::int64_t size() const noexcept;

    private:
        struct State;

        explicit FirmwareFileWriter(std::shared_ptr<State> state);

        std::shared_ptr<State> m_state; ///< 文件与摘要状态
    };
} // namespace EdgePulse::Firmware
