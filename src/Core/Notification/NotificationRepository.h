/**
 * @file NotificationRepository.h
 * @brief 告警通知渠道仓储
 * @author Gyanis
 * @date 2026-10-21
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

namespace EdgePulse::Notification
{
    /**
     * @brief 一条通知渠道
     */
    struct ChannelRecord
    {
        std::int64_t id{0};            ///< 主键
        std::int64_t tenantId{1};      ///< 归属租户
        std::string  name;             ///< 渠道名
        std::string  type;             ///< webhook / email
        std::string  endpoint;         ///< Webhook URL 或 SMTP URL
        std::string  recipients;       ///< 邮件收件人，逗号分隔
        std::string  username;         ///< SMTP 用户名
        std::string  secret;           ///< Webhook 签名密钥或 SMTP 口令
        std::string  sender;           ///< 邮件发件人
        std::string  caFile;           ///< 邮件 TLS 的私有 CA 文件，空表示系统信任库
        std::string  minLevel{"info"}; ///< 最低告警级别
        bool         enabled{true};    ///< 是否启用
        std::int64_t createdBy{0};     ///< 创建人
        std::string  createdAt;        ///< 创建时间
    };

    /**
     * @brief 一次通知投递的记录
     */
    struct DeliveryRecord
    {
        std::int64_t id{0};        ///< 主键
        std::int64_t tenantId{1};  ///< 归属租户
        std::int64_t channelId{0}; ///< 渠道
        std::string  channelName;  ///< 渠道名快照
        std::string  channelType;  ///< webhook/email
        std::int64_t alarmId{0};   ///< 告警
        std::string  alarmLevel;   ///< 告警级别
        std::string  target;       ///< 脱敏后的目标
        std::string  status;       ///< succeeded/failed
        int          attempts{0};  ///< 尝试次数
        std::string  error;        ///< 失败原因
        std::string  createdAt;    ///< 投递时间
    };

    /**
     * @brief 通知渠道仓储
     */
    class NotificationRepository
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         * @param tenant 租户边界
         */
        explicit NotificationRepository(std::shared_ptr<Common::Database> database, System::TenantScope tenant = {});

        /// 按租户边界派生仓储视图
        [[nodiscard]] std::shared_ptr<NotificationRepository> scoped(System::TenantScope tenant) const;

        /**
         * @brief 分页查询渠道
         * @param loop 恢复协程用的事件循环
         * @param keyword 名称或地址
         * @param page 分页
         * @return Common::PageRows<ChannelRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<ChannelRecord>> list(AsynGyanis::Core::EventLoop &loop, std::string keyword, const Common::PageParameters &page);

        /**
         * @brief 按标识查渠道
         * @param loop 恢复协程用的事件循环
         * @param channelId 渠道标识
         * @return std::optional<ChannelRecord> 渠道
         */
        AsynGyanis::Core::Task<std::optional<ChannelRecord>> find(AsynGyanis::Core::EventLoop &loop, std::int64_t channelId);

        /**
         * @brief 当前范围内启用的渠道
         * @param loop 恢复协程用的事件循环
         * @return std::vector<ChannelRecord> 渠道
         */
        AsynGyanis::Core::Task<std::vector<ChannelRecord>> enabledChannels(AsynGyanis::Core::EventLoop &loop);

        /**
         * @brief 新建渠道
         * @param loop 恢复协程用的事件循环
         * @param record 渠道字段，id 由仓储生成
         * @return std::int64_t 新渠道标识
         */
        AsynGyanis::Core::Task<std::int64_t> create(AsynGyanis::Core::EventLoop &loop, ChannelRecord record);

        /**
         * @brief 修改渠道
         * @param loop 恢复协程用的事件循环
         * @param channelId 渠道标识
         * @param record 新字段；secret 为空保持原值
         * @return bool 是否命中
         */
        AsynGyanis::Core::Task<bool> update(AsynGyanis::Core::EventLoop &loop, std::int64_t channelId, ChannelRecord record);

        /**
         * @brief 软删除渠道
         * @param loop 恢复协程用的事件循环
         * @param channelId 渠道标识
         * @return bool 是否命中
         */
        AsynGyanis::Core::Task<bool> remove(AsynGyanis::Core::EventLoop &loop, std::int64_t channelId);

        /**
         * @brief 写入一条投递记录
         * @param loop 恢复协程用的事件循环
         * @param record 记录字段
         * @return std::int64_t 记录标识
         */
        AsynGyanis::Core::Task<std::int64_t> recordDelivery(AsynGyanis::Core::EventLoop &loop, DeliveryRecord record);

        /**
         * @brief 分页查询投递记录
         * @param loop 恢复协程用的事件循环
         * @param status 空、succeeded、failed
         * @param channelId 渠道过滤，0 表示全部
         * @param page 分页
         * @return Common::PageRows<DeliveryRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<DeliveryRecord>> listDeliveries(AsynGyanis::Core::EventLoop &loop, std::string status, std::int64_t channelId,
                                                                                const Common::PageParameters &page);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
        System::TenantScope               m_tenant;   ///< 租户边界
    };
} // namespace EdgePulse::Notification
