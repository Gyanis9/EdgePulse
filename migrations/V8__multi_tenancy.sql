-- 多租户：租户注册表与各业务表的归属列。
-- 既有数据统一落到默认租户（1）；用户名、角色码、岗位码仍是全局唯一。
CREATE TABLE IF NOT EXISTS sys_tenant (
    id BIGINT PRIMARY KEY,
    tenant_code VARCHAR(64) NOT NULL,
    tenant_name VARCHAR(128) NOT NULL,
    status TINYINT NOT NULL DEFAULT 1,
    remark VARCHAR(255) NOT NULL DEFAULT '',
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE UNIQUE INDEX uq_sys_tenant_code ON sys_tenant (tenant_code);
INSERT INTO sys_tenant (id, tenant_code, tenant_name, status, remark) VALUES (1, 'default', '默认租户', 1, '迁移时承接既有数据');

ALTER TABLE sys_user ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE sys_user ADD COLUMN platform_admin TINYINT NOT NULL DEFAULT 0;
UPDATE sys_user SET platform_admin = 1 WHERE id = 1;
ALTER TABLE sys_oper_log ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE sys_login_log ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE biz_node ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE biz_node_group ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE biz_telemetry ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE biz_alarm_rule ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE biz_alarm ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE biz_firmware ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE biz_firmware_task ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE biz_command ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;

CREATE INDEX idx_sys_user_tenant ON sys_user (tenant_id);
CREATE INDEX idx_sys_login_log_tenant_time ON sys_login_log (tenant_id, login_time);
CREATE INDEX idx_sys_oper_log_tenant_time ON sys_oper_log (tenant_id, oper_time);
CREATE INDEX idx_biz_node_tenant ON biz_node (tenant_id);
CREATE INDEX idx_biz_node_group_tenant ON biz_node_group (tenant_id);
CREATE INDEX idx_biz_telemetry_tenant_time ON biz_telemetry (tenant_id, collected_at);
CREATE INDEX idx_biz_alarm_rule_tenant ON biz_alarm_rule (tenant_id);
CREATE INDEX idx_biz_alarm_tenant_status ON biz_alarm (tenant_id, status);
CREATE INDEX idx_biz_firmware_tenant ON biz_firmware (tenant_id);
CREATE INDEX idx_biz_firmware_task_tenant ON biz_firmware_task (tenant_id);
CREATE INDEX idx_biz_command_tenant ON biz_command (tenant_id);

INSERT INTO sys_menu (id, parent_id, menu_name, menu_type, icon, path, component, permission, sort, visible, status) VALUES
    (1108, 1100, '租户管理', 'menu', 'building-2', '/system/tenant', 'system/tenant/index', 'system:tenant:list', 80, 1, 1),
    (1190, 1108, '租户新增', 'btn', '', '', '', 'system:tenant:add', 10, 1, 1),
    (1191, 1108, '租户编辑', 'btn', '', '', '', 'system:tenant:edit', 20, 1, 1),
    (1192, 1108, '租户停用', 'btn', '', '', '', 'system:tenant:remove', 30, 1, 1);

INSERT INTO sys_role_menu (role_id, menu_id) VALUES (1, 1108), (1, 1190), (1, 1191), (1, 1192);

-- 全局“租户管理员”角色：只含租户内可用的用户与设备菜单，不含平台级 RBAC、字典、参数与租户管理
INSERT INTO sys_role (id, role_name, role_code, data_scope, builtin, status, deleted) VALUES
    (3, '租户管理员', 'tenant_admin', 'ALL', 0, 1, 0);
INSERT INTO sys_role_menu (role_id, menu_id) VALUES
    (3, 1300), (3, 1100), (3, 1101),
    (3, 1110), (3, 1111), (3, 1112), (3, 1113), (3, 1114),
    (3, 1200), (3, 1201), (3, 1202), (3, 1180), (3, 1182),
    (3, 1000), (3, 1001), (3, 1010),
    (3, 1003), (3, 1011), (3, 1012), (3, 1013), (3, 1014),
    (3, 1004),
    (3, 1005), (3, 1015), (3, 1016),
    (3, 1006), (3, 1017);
