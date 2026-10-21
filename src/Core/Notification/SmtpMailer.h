/**
 * @file SmtpMailer.h
 * @brief 极简 SMTP 发信：明文中继与隐式 TLS
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <Core/Coroutine/Task.h>

#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Notification
{
    /**
     * @brief 一次发信所需的连接与身份
     */
    struct SmtpSettings
    {
        std::string              endpoint;   ///< smtp://host:25 或 smtps://host:465
        std::string              username;   ///< 认证用户名，可空
        std::string              password;   ///< 认证口令，可空
        std::string              sender;     ///< 发件人地址；空时用 username
        std::string              caFile;     ///< 邮件 TLS 的私有 CA 文件，空表示系统信任库
        std::vector<std::string> recipients; ///< 收件人
    };

    /**
     * @brief SMTP 发信器
     * @note 只实现 EHLO、AUTH PLAIN、MAIL/RCPT/DATA 与 QUIT；不支持的服务器返回失败原因
     */
    class SmtpMailer
    {
    public:
        /**
         * @brief 发送一封纯文本邮件
         * @param loop 恢复协程用的事件循环
         * @param settings 连接与身份
         * @param subject 主题，按 UTF-8 base64 编码
         * @param body 正文
         * @param error 失败原因
         * @return bool 是否被服务器接收
         */
        [[nodiscard]] static AsynGyanis::Core::Task<bool> send(AsynGyanis::Core::EventLoop &loop, const SmtpSettings &settings, std::string subject, std::string body,
                                                               std::string &error);
    };
} // namespace EdgePulse::Notification
