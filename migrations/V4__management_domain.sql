-- P1 管理域：组织、岗位、字典、参数、操作日志与完整菜单权限。
CREATE TABLE IF NOT EXISTS sys_dept (
    id BIGINT PRIMARY KEY,
    parent_id BIGINT NOT NULL DEFAULT 0,
    name VARCHAR(64) NOT NULL,
    leader VARCHAR(64) NOT NULL DEFAULT '',
    phone VARCHAR(32) NOT NULL DEFAULT '',
    email VARCHAR(128) NOT NULL DEFAULT '',
    sort INT NOT NULL DEFAULT 0,
    status TINYINT NOT NULL DEFAULT 1,
    deleted TINYINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by BIGINT NOT NULL DEFAULT 0,
    updated_by BIGINT NOT NULL DEFAULT 0
);
CREATE INDEX idx_sys_dept_parent ON sys_dept (parent_id);

CREATE TABLE IF NOT EXISTS sys_post (
    id BIGINT PRIMARY KEY,
    post_code VARCHAR(64) NOT NULL,
    post_name VARCHAR(64) NOT NULL,
    sort INT NOT NULL DEFAULT 0,
    status TINYINT NOT NULL DEFAULT 1,
    remark VARCHAR(255) NOT NULL DEFAULT '',
    deleted TINYINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by BIGINT NOT NULL DEFAULT 0,
    updated_by BIGINT NOT NULL DEFAULT 0,
    CONSTRAINT uq_sys_post_code UNIQUE (post_code)
);

CREATE TABLE IF NOT EXISTS sys_user_post (
    user_id BIGINT NOT NULL,
    post_id BIGINT NOT NULL,
    PRIMARY KEY (user_id, post_id)
);

CREATE TABLE IF NOT EXISTS sys_role_dept (
    role_id BIGINT NOT NULL,
    dept_id BIGINT NOT NULL,
    PRIMARY KEY (role_id, dept_id)
);

CREATE TABLE IF NOT EXISTS sys_dict_type (
    id BIGINT PRIMARY KEY,
    dict_name VARCHAR(64) NOT NULL,
    dict_type VARCHAR(64) NOT NULL,
    status TINYINT NOT NULL DEFAULT 1,
    remark VARCHAR(255) NOT NULL DEFAULT '',
    deleted TINYINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by BIGINT NOT NULL DEFAULT 0,
    updated_by BIGINT NOT NULL DEFAULT 0,
    CONSTRAINT uq_sys_dict_type UNIQUE (dict_type)
);

CREATE TABLE IF NOT EXISTS sys_dict_data (
    id BIGINT PRIMARY KEY,
    dict_type VARCHAR(64) NOT NULL,
    label VARCHAR(64) NOT NULL,
    value VARCHAR(64) NOT NULL,
    sort INT NOT NULL DEFAULT 0,
    status TINYINT NOT NULL DEFAULT 1,
    tone VARCHAR(16) NOT NULL DEFAULT 'neutral',
    is_default TINYINT NOT NULL DEFAULT 0,
    remark VARCHAR(255) NOT NULL DEFAULT '',
    deleted TINYINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by BIGINT NOT NULL DEFAULT 0,
    updated_by BIGINT NOT NULL DEFAULT 0
);
CREATE INDEX idx_sys_dict_data_type ON sys_dict_data (dict_type);

CREATE TABLE IF NOT EXISTS sys_config (
    id BIGINT PRIMARY KEY,
    config_name VARCHAR(128) NOT NULL,
    config_key VARCHAR(128) NOT NULL,
    config_value VARCHAR(512) NOT NULL DEFAULT '',
    config_type VARCHAR(1) NOT NULL DEFAULT 'N',
    builtin TINYINT NOT NULL DEFAULT 0,
    remark VARCHAR(255) NOT NULL DEFAULT '',
    deleted TINYINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by BIGINT NOT NULL DEFAULT 0,
    updated_by BIGINT NOT NULL DEFAULT 0,
    CONSTRAINT uq_sys_config_key UNIQUE (config_key)
);

CREATE TABLE IF NOT EXISTS sys_oper_log (
    id BIGINT PRIMARY KEY,
    title VARCHAR(128) NOT NULL DEFAULT '',
    module VARCHAR(64) NOT NULL DEFAULT '',
    action VARCHAR(64) NOT NULL DEFAULT '',
    method VARCHAR(16) NOT NULL DEFAULT '',
    url VARCHAR(512) NOT NULL DEFAULT '',
    ip VARCHAR(64) NOT NULL DEFAULT '',
    oper_name VARCHAR(64) NOT NULL DEFAULT '',
    dept_name VARCHAR(64) NOT NULL DEFAULT '',
    params TEXT NULL,
    result TINYINT NOT NULL DEFAULT 1,
    error_msg VARCHAR(512) NOT NULL DEFAULT '',
    cost_ms BIGINT NOT NULL DEFAULT 0,
    trace_id VARCHAR(64) NOT NULL DEFAULT '',
    oper_time DATETIME NOT NULL
);
CREATE INDEX idx_sys_oper_log_time ON sys_oper_log (oper_time);

ALTER TABLE sys_role ADD COLUMN sort INT NOT NULL DEFAULT 0;
ALTER TABLE sys_role ADD COLUMN remark VARCHAR(255) NOT NULL DEFAULT '';

ALTER TABLE sys_login_log ADD COLUMN location VARCHAR(128) NOT NULL DEFAULT '';
ALTER TABLE sys_login_log ADD COLUMN browser VARCHAR(64) NOT NULL DEFAULT '';
ALTER TABLE sys_login_log ADD COLUMN os VARCHAR(64) NOT NULL DEFAULT '';

ALTER TABLE biz_node ADD COLUMN group_id BIGINT NOT NULL DEFAULT 0;
ALTER TABLE biz_node ADD COLUMN owner_dept_id BIGINT NOT NULL DEFAULT 0;

INSERT INTO sys_dept (id, parent_id, name, leader, sort, status) VALUES
    (100, 0, 'EdgePulse', '超级管理员', 10, 1),
    (101, 100, '研发中心', '', 10, 1),
    (102, 100, '运维中心', '', 20, 1);

INSERT INTO sys_post (id, post_code, post_name, sort, status, remark) VALUES
    (100, 'platform_admin', '平台管理员', 10, 1, '负责平台配置与权限'),
    (101, 'ops_engineer', '运维工程师', 20, 1, '负责节点与告警处置');

INSERT INTO sys_user_post (user_id, post_id) VALUES (1, 100);
UPDATE sys_user SET dept_id = 100 WHERE id = 1;

INSERT INTO sys_role (id, role_name, role_code, data_scope, builtin, status, deleted) VALUES
    (2, '运维人员', 'ops_team', 'DEPT', 0, 1, 0);

INSERT INTO sys_dict_type (id, dict_name, dict_type, status, remark) VALUES
    (100, '用户状态', 'sys_user_status', 1, '启用与停用'),
    (101, '通用状态', 'sys_common_status', 1, '通用启用状态'),
    (102, '操作结果', 'sys_oper_result', 1, '成功与失败'),
    (103, '告警级别', 'sys_alarm_level', 1, '提示、警告、严重'),
    (104, '固件状态', 'sys_firmware_status', 1, '固件生命周期'),
    (105, '命令状态', 'sys_command_status', 1, '命令生命周期'),
    (106, '数据范围', 'sys_data_scope', 1, '角色可见数据范围');

INSERT INTO sys_dict_data (id, dict_type, label, value, sort, status, tone, is_default) VALUES
    (100, 'sys_user_status', '正常', '1', 10, 1, 'ok', 1),
    (101, 'sys_user_status', '停用', '0', 20, 1, 'danger', 0),
    (102, 'sys_common_status', '启用', '1', 10, 1, 'ok', 1),
    (103, 'sys_common_status', '停用', '0', 20, 1, 'neutral', 0),
    (104, 'sys_oper_result', '成功', '1', 10, 1, 'ok', 1),
    (105, 'sys_oper_result', '失败', '0', 20, 1, 'danger', 0),
    (106, 'sys_alarm_level', '提示', 'info', 10, 1, 'info', 0),
    (107, 'sys_alarm_level', '警告', 'warning', 20, 1, 'neutral', 1),
    (108, 'sys_alarm_level', '严重', 'critical', 30, 1, 'danger', 0),
    (109, 'sys_firmware_status', '待下发', 'ready', 10, 1, 'neutral', 1),
    (110, 'sys_firmware_status', '已安装', 'installed', 20, 1, 'ok', 0),
    (111, 'sys_firmware_status', '失败', 'failed', 30, 1, 'danger', 0),
    (112, 'sys_command_status', '待执行', 'pending', 10, 1, 'neutral', 1),
    (113, 'sys_command_status', '执行中', 'running', 20, 1, 'info', 0),
    (114, 'sys_command_status', '已成功', 'succeeded', 30, 1, 'ok', 0),
    (115, 'sys_command_status', '已失败', 'failed', 40, 1, 'danger', 0),
    (116, 'sys_command_status', '已超时', 'timeout', 50, 1, 'danger', 0),
    (117, 'sys_data_scope', '全部数据', 'ALL', 10, 1, 'ok', 0),
    (118, 'sys_data_scope', '本部门及以下', 'DEPT_AND_CHILD', 20, 1, 'info', 0),
    (119, 'sys_data_scope', '本部门', 'DEPT', 30, 1, 'info', 0),
    (120, 'sys_data_scope', '仅本人', 'SELF', 40, 1, 'neutral', 1),
    (121, 'sys_data_scope', '自定义', 'CUSTOM', 50, 1, 'neutral', 0);

INSERT INTO sys_config (id, config_name, config_key, config_value, config_type, builtin, remark) VALUES
    (100, '平台名称', 'sys.site.name', 'EdgePulse', 'N', 1, '显示在浏览器标题与登录页'),
    (101, '默认分页大小', 'sys.ui.pageSize', '10', 'N', 1, '列表页每页条数'),
    (102, '会话闲置分钟', 'sys.session.idleMinutes', '120', 'N', 1, '与 edgepulse.session.ttl_seconds 对齐'),
    (103, '初始口令提示', 'sys.user.initPasswordHint', '由组织统一发放', 'N', 1, '仅提示文案，不保存真实口令');

INSERT INTO sys_menu (id, parent_id, menu_name, menu_type, icon, path, component, permission, sort, visible, status) VALUES
    (1300, 0, '工作台', 'menu', 'layout-dashboard', '/dashboard', 'dashboard/index', '', 1, 1, 1),
    (1100, 0, '系统管理', 'dir', 'settings', '/system', '', '', 20, 1, 1),
    (1101, 1100, '用户管理', 'menu', 'users', '/system/user', 'system/user/index', 'system:user:list', 10, 1, 1),
    (1102, 1100, '角色管理', 'menu', 'shield', '/system/role', 'system/role/index', 'system:role:list', 20, 1, 1),
    (1103, 1100, '菜单管理', 'menu', 'list-tree', '/system/menu', 'system/menu/index', 'system:menu:list', 30, 1, 1),
    (1104, 1100, '部门管理', 'menu', 'building', '/system/dept', 'system/dept/index', 'system:dept:list', 40, 1, 1),
    (1105, 1100, '岗位管理', 'menu', 'briefcase', '/system/post', 'system/post/index', 'system:post:list', 50, 1, 1),
    (1106, 1100, '字典管理', 'menu', 'book', '/system/dict', 'system/dict/index', 'system:dict:list', 60, 1, 1),
    (1107, 1100, '参数配置', 'menu', 'sliders', '/system/config', 'system/config/index', 'system:config:list', 70, 1, 1),
    (1200, 0, '监控中心', 'dir', 'activity', '/monitor', '', '', 30, 1, 1),
    (1201, 1200, '操作日志', 'menu', 'scroll-text', '/monitor/operlog', 'monitor/operlog/index', 'monitor:operlog:list', 10, 1, 1),
    (1202, 1200, '登录日志', 'menu', 'log-in', '/monitor/loginlog', 'monitor/loginlog/index', 'monitor:loginlog:list', 20, 1, 1),
    (1003, 1000, '告警中心', 'menu', 'bell', '/device/alarms', 'device/Alarms', 'device:alarm:list', 20, 1, 1),
    (1004, 1000, '遥测数据', 'menu', 'line-chart', '/device/telemetry', 'device/Telemetry', 'device:telemetry:view', 30, 1, 1),
    (1005, 1000, '固件管理', 'menu', 'package', '/device/firmware', 'device/Firmware', 'device:firmware:list', 40, 1, 1),
    (1006, 1000, '命令中心', 'menu', 'terminal', '/device/commands', 'device/Commands', 'device:command:list', 50, 1, 1);

INSERT INTO sys_menu (id, parent_id, menu_name, menu_type, icon, path, component, permission, sort, visible, status) VALUES
    (1010, 1001, '节点写入', 'btn', '', '', '', 'device:node:write', 10, 1, 1),
    (1011, 1003, '告警处置', 'btn', '', '', '', 'device:alarm:handle', 10, 1, 1),
    (1012, 1003, '告警规则新增', 'btn', '', '', '', 'device:alarm:add', 20, 1, 1),
    (1013, 1003, '告警规则编辑', 'btn', '', '', '', 'device:alarm:edit', 30, 1, 1),
    (1014, 1003, '告警规则删除', 'btn', '', '', '', 'device:alarm:remove', 40, 1, 1),
    (1015, 1005, '固件上传', 'btn', '', '', '', 'device:firmware:upload', 10, 1, 1),
    (1016, 1005, '固件删除', 'btn', '', '', '', 'device:firmware:remove', 20, 1, 1),
    (1017, 1006, '命令下发', 'btn', '', '', '', 'device:command:send', 10, 1, 1),
    (1110, 1101, '用户新增', 'btn', '', '', '', 'system:user:add', 10, 1, 1),
    (1111, 1101, '用户编辑', 'btn', '', '', '', 'system:user:edit', 20, 1, 1),
    (1112, 1101, '用户删除', 'btn', '', '', '', 'system:user:remove', 30, 1, 1),
    (1113, 1101, '用户导出', 'btn', '', '', '', 'system:user:export', 40, 1, 1),
    (1114, 1101, '重置密码', 'btn', '', '', '', 'system:user:reset', 50, 1, 1),
    (1120, 1102, '角色新增', 'btn', '', '', '', 'system:role:add', 10, 1, 1),
    (1121, 1102, '角色编辑', 'btn', '', '', '', 'system:role:edit', 20, 1, 1),
    (1122, 1102, '角色删除', 'btn', '', '', '', 'system:role:remove', 30, 1, 1),
    (1123, 1102, '角色导出', 'btn', '', '', '', 'system:role:export', 40, 1, 1),
    (1130, 1103, '菜单新增', 'btn', '', '', '', 'system:menu:add', 10, 1, 1),
    (1131, 1103, '菜单编辑', 'btn', '', '', '', 'system:menu:edit', 20, 1, 1),
    (1132, 1103, '菜单删除', 'btn', '', '', '', 'system:menu:remove', 30, 1, 1),
    (1133, 1103, '菜单导出', 'btn', '', '', '', 'system:menu:export', 40, 1, 1),
    (1140, 1104, '部门新增', 'btn', '', '', '', 'system:dept:add', 10, 1, 1),
    (1141, 1104, '部门编辑', 'btn', '', '', '', 'system:dept:edit', 20, 1, 1),
    (1142, 1104, '部门删除', 'btn', '', '', '', 'system:dept:remove', 30, 1, 1),
    (1143, 1104, '部门导出', 'btn', '', '', '', 'system:dept:export', 40, 1, 1),
    (1150, 1105, '岗位新增', 'btn', '', '', '', 'system:post:add', 10, 1, 1),
    (1151, 1105, '岗位编辑', 'btn', '', '', '', 'system:post:edit', 20, 1, 1),
    (1152, 1105, '岗位删除', 'btn', '', '', '', 'system:post:remove', 30, 1, 1),
    (1153, 1105, '岗位导出', 'btn', '', '', '', 'system:post:export', 40, 1, 1),
    (1160, 1106, '字典新增', 'btn', '', '', '', 'system:dict:add', 10, 1, 1),
    (1161, 1106, '字典编辑', 'btn', '', '', '', 'system:dict:edit', 20, 1, 1),
    (1162, 1106, '字典删除', 'btn', '', '', '', 'system:dict:remove', 30, 1, 1),
    (1163, 1106, '字典导出', 'btn', '', '', '', 'system:dict:export', 40, 1, 1),
    (1170, 1107, '参数新增', 'btn', '', '', '', 'system:config:add', 10, 1, 1),
    (1171, 1107, '参数编辑', 'btn', '', '', '', 'system:config:edit', 20, 1, 1),
    (1172, 1107, '参数删除', 'btn', '', '', '', 'system:config:remove', 30, 1, 1),
    (1173, 1107, '参数导出', 'btn', '', '', '', 'system:config:export', 40, 1, 1),
    (1180, 1201, '操作日志导出', 'btn', '', '', '', 'monitor:operlog:export', 10, 1, 1),
    (1181, 1201, '操作日志删除', 'btn', '', '', '', 'monitor:operlog:remove', 20, 1, 1),
    (1182, 1202, '登录日志导出', 'btn', '', '', '', 'monitor:loginlog:export', 10, 1, 1),
    (1183, 1202, '登录日志删除', 'btn', '', '', '', 'monitor:loginlog:remove', 20, 1, 1);

INSERT INTO sys_role_menu (role_id, menu_id)
SELECT 1, id FROM sys_menu WHERE id NOT IN (SELECT menu_id FROM sys_role_menu WHERE role_id = 1);

INSERT INTO sys_role_menu (role_id, menu_id) VALUES
    (2, 1300), (2, 1000), (2, 1001), (2, 1003), (2, 1004), (2, 1011), (2, 1200), (2, 1201), (2, 1202), (2, 1180), (2, 1182);
