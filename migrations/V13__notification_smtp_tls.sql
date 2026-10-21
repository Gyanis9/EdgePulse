-- 邮件渠道支持 STARTTLS 与私有 CA。
ALTER TABLE biz_notification_channel ADD COLUMN ca_file VARCHAR(512) NOT NULL DEFAULT '';
