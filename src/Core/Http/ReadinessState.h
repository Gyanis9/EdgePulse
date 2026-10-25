/**
 * @file ReadinessState.h
 * @brief 就绪探针的依赖快照与判定
 * @author Gyanis
 * @date 2026-10-25
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Http/Server/ReadinessCheck.h"

#include <atomic>
#include <filesystem>

namespace EdgePulse::Web
{
    /**
     * @brief 一次就绪判定的五格依赖
     */
    struct ReadinessDependencies
    {
        bool databaseReady{false};          ///< 最近一轮数据库探针
        bool serviceReady{false};           ///< 进程是否已完成启动且未进入排空
        bool cacheReady{false};             ///< 最近一轮缓存探针
        bool realtimeHealthy{false};        ///< 实时事件总线是否可用
        bool firmwareDirectoryReady{false}; ///< 固件目录是否存在且可写
    };

    /**
     * @brief 把依赖快照折成 /readyz 的结论
     * @param dependencies 依赖快照
     * @return AsynGyanis::Net::ReadinessCheckResult 第一格不就绪的依赖决定原因；全绿时 reason 为空
     * @note 纯函数，不读全局状态；判定顺序固定为数据库、启停、缓存、实时总线、固件目录
     */
    [[nodiscard]] AsynGyanis::Net::ReadinessCheckResult evaluateReadiness(const ReadinessDependencies &dependencies);

    /**
     * @brief 在目录下落一个探针文件再删除，确认目录可写
     * @param directory 目标目录
     * @return bool 目录存在且能完整写入并收尾时为 true；目录缺失、无权限或写入失败为 false
     * @note 会做一次磁盘写入；只能在后台线程或周期协程里调用，不得放进 /readyz 的同步回调
     */
    [[nodiscard]] bool probeDirectoryWritable(const std::filesystem::path &directory);

    /**
     * @brief 就绪依赖的原子快照
     * @details 各依赖由后台探针线程或周期协程更新，/readyz 只做一次原子读并折成结论，
     *          不阻塞事件循环。
     */
    class ReadinessState
    {
    public:
        ReadinessState() = default;

        ReadinessState(const ReadinessState &)            = delete;
        ReadinessState &operator=(const ReadinessState &) = delete;

        /**
         * @brief 更新数据库探针结论
         * @param ready 是否可用
         */
        void setDatabaseReady(bool ready) noexcept;

        /**
         * @brief 更新进程启停状态
         * @param ready 完成启动且未进入排空时为 true
         */
        void setServiceReady(bool ready) noexcept;

        /**
         * @brief 更新缓存探针结论
         * @param ready 是否可用
         */
        void setCacheReady(bool ready) noexcept;

        /**
         * @brief 更新实时事件总线健康状态
         * @param healthy 是否可用
         */
        void setRealtimeHealthy(bool healthy) noexcept;

        /**
         * @brief 更新固件目录可写状态
         * @param ready 是否存在且可写
         */
        void setFirmwareDirectoryReady(bool ready) noexcept;

        /**
         * @brief 读取五格依赖的原子快照
         * @return ReadinessDependencies 当前值
         */
        [[nodiscard]] ReadinessDependencies snapshot() const noexcept;

        /**
         * @brief 按快照给出 /readyz 结论
         * @return AsynGyanis::Net::ReadinessCheckResult 可直接交给 ReadinessCheck 回调
         */
        [[nodiscard]] AsynGyanis::Net::ReadinessCheckResult check() const;

    private:
        std::atomic<bool> m_databaseReady{false};          ///< 数据库探针
        std::atomic<bool> m_serviceReady{false};           ///< 启停状态
        std::atomic<bool> m_cacheReady{false};             ///< 缓存探针
        std::atomic<bool> m_realtimeHealthy{false};        ///< 实时总线
        std::atomic<bool> m_firmwareDirectoryReady{false}; ///< 固件目录
    };
} // namespace EdgePulse::Web
