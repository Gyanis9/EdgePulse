-- 通知投递记录：成功与失败都落库，供运维排查。
CREATE TABLE IF NOT EXISTS biz_notification_delivery (
    id BIGINT PRIMARY KEY,
    tenant_id BIGINT NOT NULL,
    channel_id BIGINT NOT NULL,
    channel_name VARCHAR(64) NOT NULL,
    channel_type VARCHAR(16) NOT NULL,
    alarm_id BIGINT NOT NULL DEFAULT 0,
    alarm_level VARCHAR(16) NOT NULL DEFAULT '',
    target VARCHAR(160) NOT NULL DEFAULT '',
    status VARCHAR(16) NOT NULL,
    attempts INT NOT NULL DEFAULT 0,
    error VARCHAR(512) NOT NULL DEFAULT '',
    created_at DATETIME NOT NULL
);
CREATE INDEX idx_notification_delivery_tenant_time ON biz_notification_delivery (tenant_id, created_at);
