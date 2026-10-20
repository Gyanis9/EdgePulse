/**
 * @file FirmwareRepository.h
 * @brief 固件元数据与下发任务仓储
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Api/Pagination.h"
#include "Base/Database/Database.h"
#include "Core/System/TenantScope.h"

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Firmware
{
    /**
     * @brief 固件元数据
     */
    struct FirmwareRecord
    {
        std::int64_t id{0};        ///< 主键
        std::string  name;         ///< 名称
        std::string  version;      ///< 版本
        std::string  fileName;     ///< 原始文件名
        std::string  contentType;  ///< 媒体类型
        std::int64_t sizeBytes{0}; ///< 字节数
        std::string  sha256;       ///< 十六进制摘要
        std::string  storagePath;  ///< 落盘路径
        std::int64_t createdBy{0}; ///< 上传人
        std::string  createdAt;    ///< 上传时间
    };

    /**
     * @brief 固件任务状态
     */
    struct FirmwareTaskRecord
    {
        std::int64_t id{0};         ///< 主键
        std::int64_t firmwareId{0}; ///< 固件
        std::int64_t nodeId{0};     ///< 节点
        std::string  nodeName;      ///< 节点名
        std::string  status;        ///< pending/downloading/installed/failed
        int          progress{0};   ///< 进度 0..100
        std::string  message;       ///< 说明
        std::string  createdAt;     ///< 创建时间
        std::string  updatedAt;     ///< 更新时间
    };

    /**
     * @brief 固件仓储
     */
    class FirmwareRepository
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         */
        explicit FirmwareRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant = {});

        /// 按租户边界派生仓储视图
        [[nodiscard]] std::shared_ptr<FirmwareRepository> scoped(System::TenantScope tenant) const;

        /**
         * @brief 写入固件元数据
         * @param loop 恢复协程用的事件循环
         * @param record 元数据；id 为 0 时由仓储生成
         * @return std::int64_t 固件标识
         */
        AsynGyanis::Core::Task<std::int64_t> create(AsynGyanis::Core::EventLoop &loop, FirmwareRecord record);

        /**
         * @brief 分页查询固件
         * @param loop 恢复协程用的事件循环
         * @param keyword 名称或版本
         * @param page 分页
         * @return Common::PageRows<FirmwareRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<FirmwareRecord>> list(AsynGyanis::Core::EventLoop &loop, std::string keyword, const Common::PageParameters &page);

        /**
         * @brief 按标识查固件
         * @param loop 恢复协程用的事件循环
         * @param firmwareId 固件标识
         * @return std::optional<FirmwareRecord> 固件
         */
        AsynGyanis::Core::Task<std::optional<FirmwareRecord>> find(AsynGyanis::Core::EventLoop &loop, std::int64_t firmwareId);

        /**
         * @brief 软删除固件
         * @param loop 恢复协程用的事件循环
         * @param firmwareId 固件标识
         * @return std::optional<FirmwareRecord> 被删除的元数据；不存在时为空
         */
        AsynGyanis::Core::Task<std::optional<FirmwareRecord>> remove(AsynGyanis::Core::EventLoop &loop, std::int64_t firmwareId);

        /**
         * @brief 创建下发任务
         * @param loop 恢复协程用的事件循环
         * @param firmwareId 固件
         * @param nodeId 节点
         * @return std::int64_t 任务标识；重复下发返回 0
         */
        AsynGyanis::Core::Task<std::int64_t> createTask(AsynGyanis::Core::EventLoop &loop, std::int64_t firmwareId, std::int64_t nodeId);

        /**
         * @brief 查询固件的下发任务
         * @param loop 恢复协程用的事件循环
         * @param firmwareId 固件
         * @return std::vector<FirmwareTaskRecord> 任务
         */
        AsynGyanis::Core::Task<std::vector<FirmwareTaskRecord>> tasks(AsynGyanis::Core::EventLoop &loop, std::int64_t firmwareId);

        /**
         * @brief 更新节点上的任务状态
         * @param loop 恢复协程用的事件循环
         * @param firmwareId 固件
         * @param nodeId 节点
         * @param status 状态
         * @param progress 进度
         * @param message 说明
         * @return std::optional<FirmwareTaskRecord> 更新后的任务
         */
        AsynGyanis::Core::Task<std::optional<FirmwareTaskRecord>> updateTask(AsynGyanis::Core::EventLoop &loop, std::int64_t firmwareId, std::int64_t nodeId, std::string status,
                                                                             int progress, std::string message);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
        System::TenantScope               m_tenant;   ///< 租户边界
    };
} // namespace EdgePulse::Firmware
