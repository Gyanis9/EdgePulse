-- 告警通知渠道：每个租户可配置自己的 Webhook 与邮件出口。
CREATE TABLE IF NOT EXISTS biz_notification_channel (
    id BIGINT PRIMARY KEY,
    tenant_id BIGINT NOT NULL,
    name VARCHAR(64) NOT NULL,
    type VARCHAR(16) NOT NULL,
    endpoint VARCHAR(512) NOT NULL,
    recipients VARCHAR(512) NOT NULL DEFAULT '',
    username VARCHAR(128) NOT NULL DEFAULT '',
    secret VARCHAR(512) NOT NULL DEFAULT '',
    sender VARCHAR(128) NOT NULL DEFAULT '',
    min_level VARCHAR(16) NOT NULL DEFAULT 'info',
    enabled TINYINT NOT NULL DEFAULT 1,
    deleted TINYINT NOT NULL DEFAULT 0,
    created_by BIGINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL,
    updated_at DATETIME NOT NULL
);
CREATE INDEX idx_notification_channel_tenant ON biz_notification_channel (tenant_id, enabled, deleted);

INSERT INTO sys_menu (id, parent_id, menu_name, menu_type, icon, path, component, permission, sort, visible, status) VALUES
    (1109, 1100, '通知渠道', 'menu', 'bell', '/system/notification', 'system/notification/index', 'system:notification:list', 90, 1, 1),
    (1193, 1109, '通知新增', 'btn', '', '', '', 'system:notification:add', 10, 1, 1),
    (1194, 1109, '通知编辑', 'btn', '', '', '', 'system:notification:edit', 20, 1, 1),
    (1195, 1109, '通知删除', 'btn', '', '', '', 'system:notification:remove', 30, 1, 1);

INSERT INTO sys_role_menu (role_id, menu_id) VALUES
    (1, 1109), (1, 1193), (1, 1194), (1, 1195),
    (3, 1109), (3, 1193), (3, 1194), (3, 1195);
