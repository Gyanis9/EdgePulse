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
        std::int64_t id{0};              ///< 主键
        std::int64_t tenantId{1};        ///< 归属租户
        std::string  name;               ///< 名称
        std::string  version;            ///< 版本
        std::string  fileName;           ///< 原始文件名
        std::string  contentType;        ///< 媒体类型
        std::int64_t sizeBytes{0};       ///< 字节数
        std::string  sha256;             ///< 十六进制摘要
        std::string  signature;          ///< 摘要签名的 Base64；空表示未签名
        std::string  signingKeyId;       ///< 签名所用信任键的 key id
        std::string  signatureAlgorithm; ///< 签名算法标识
        std::string  storagePath;        ///< 落盘路径
        std::int64_t createdBy{0};       ///< 上传人
        std::string  createdAt;          ///< 上传时间
    };

    /**
     * @brief 固件任务状态
     */
    struct FirmwareTaskRecord
    {
        std::int64_t id{0};                 ///< 主键
        std::int64_t firmwareId{0};         ///< 固件
        std::int64_t nodeId{0};             ///< 节点
        std::string  nodeName;              ///< 节点名
        std::string  status;                ///< pending/downloading/installed/failed
        int          progress{0};           ///< 进度 0..100
        std::string  message;               ///< 说明
        std::string  createdAt;             ///< 创建时间
        std::string  updatedAt;             ///< 更新时间
        std::int64_t previousFirmwareId{0}; ///< 下发前版本
        std::int64_t rolloutId{0};          ///< 所属灰度发布，0 表示手动下发
        std::int64_t waveId{0};             ///< 所属灰度波次，0 表示手动下发
        std::int64_t commandId{0};          ///< 关联的固件命令，0 表示未关联
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
         * @brief 统计未签名固件数
         * @param loop 恢复协程用的事件循环
         * @return std::int64_t 未软删除且 signature 为空的固件数，受租户边界约束
         */
        AsynGyanis::Core::Task<std::int64_t> unsignedCount(AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 创建下发任务
         * @param loop 恢复协程用的事件循环
         * @param firmwareId 固件
         * @param nodeId 节点
         * @param previousFirmwareId 下发前版本，供回滚
         * @param rolloutId 所属灰度发布，0 表示手动下发
         * @param waveId 所属灰度波次，0 表示手动下发
         * @param commandId 关联的固件命令，0 表示未关联
         * @return std::int64_t 任务标识；重复下发返回 0
         */
        AsynGyanis::Core::Task<std::int64_t> createTask(AsynGyanis::Core::EventLoop &loop, std::int64_t firmwareId, std::int64_t nodeId, std::int64_t previousFirmwareId = 0,
                                                        std::int64_t rolloutId = 0, std::int64_t waveId = 0, std::int64_t commandId = 0);

        /**
         * @brief 节点最近一次安装过的其他版本
         * @param loop 恢复协程用的事件循环
         * @param nodeId 节点
         * @param excludeFirmwareId 排除的固件
         * @return std::int64_t 固件标识；没有历史时 0
         */
        AsynGyanis::Core::Task<std::int64_t> latestOtherFirmware(AsynGyanis::Core::EventLoop &loop, std::int64_t nodeId, std::int64_t excludeFirmwareId);

        /**
         * @brief 某个固件在节点上的回滚目标版本
         * @param loop 恢复协程用的事件循环
         * @param firmwareId 当前固件
         * @param nodeId 节点
         * @return std::int64_t 固件标识；没有记录时 0
         */
        AsynGyanis::Core::Task<std::int64_t> rollbackSource(AsynGyanis::Core::EventLoop &loop, std::int64_t firmwareId, std::int64_t nodeId);

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
         * @param commandId 只更新该命令关联的任务；0 表示按固件与节点更新全部任务
         * @return std::optional<FirmwareTaskRecord> 更新后的任务
         */
        AsynGyanis::Core::Task<std::optional<FirmwareTaskRecord>> updateTask(AsynGyanis::Core::EventLoop &loop, std::int64_t firmwareId, std::int64_t nodeId, std::string status,
                                                                             int progress, std::string message, std::int64_t commandId = 0);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
        System::TenantScope               m_tenant;   ///< 租户边界
    };
} // namespace EdgePulse::Firmware
