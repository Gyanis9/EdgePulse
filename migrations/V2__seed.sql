-- P0 种子：一个超级管理员、一个节点菜单与两个权限码。
-- 每行都用 WHERE NOT EXISTS 保护，迁移半途失败后重跑不会撞主键。
INSERT INTO sys_role (id, role_name, role_code, data_scope, builtin, status, deleted)
SELECT 1, '超级管理员', 'super_admin', 'ALL', 1, 1, 0
WHERE NOT EXISTS (SELECT 1 FROM sys_role WHERE id = 1);

INSERT INTO sys_user (id, username, password, nickname, phone, email, dept_id, status, deleted)
SELECT 1, 'admin',
       'pbkdf2_sha256$210000$nUwvGntuPFCBorPE1eb3CA==$cxCcCezVVbPzfukzjfcpWoTZpVyQ3AR1H9Z2V1QWObk=',
       '超级管理员', '', '', 0, 1, 0
WHERE NOT EXISTS (SELECT 1 FROM sys_user WHERE id = 1);

INSERT INTO sys_menu (id, parent_id, menu_name, menu_type, icon, path, component, permission, sort, visible, status)
SELECT 1000, 0, '边缘运维', 'dir', 'layers', '/device', '', '', 10, 1, 1
WHERE NOT EXISTS (SELECT 1 FROM sys_menu WHERE id = 1000);

INSERT INTO sys_menu (id, parent_id, menu_name, menu_type, icon, path, component, permission, sort, visible, status)
SELECT 1001, 1000, '节点', 'menu', 'server', '/device/nodes', 'device/Nodes', 'device:node:list', 10, 1, 1
WHERE NOT EXISTS (SELECT 1 FROM sys_menu WHERE id = 1001);

INSERT INTO sys_menu (id, parent_id, menu_name, menu_type, icon, path, component, permission, sort, visible, status)
SELECT 1002, 1001, '节点写入', 'btn', '', '', '', 'device:node:write', 10, 1, 1
WHERE NOT EXISTS (SELECT 1 FROM sys_menu WHERE id = 1002);

INSERT INTO sys_user_role (user_id, role_id)
SELECT 1, 1
WHERE NOT EXISTS (SELECT 1 FROM sys_user_role WHERE user_id = 1 AND role_id = 1);

INSERT INTO sys_role_menu (role_id, menu_id)
SELECT 1, 1000
WHERE NOT EXISTS (SELECT 1 FROM sys_role_menu WHERE role_id = 1 AND menu_id = 1000);

INSERT INTO sys_role_menu (role_id, menu_id)
SELECT 1, 1001
WHERE NOT EXISTS (SELECT 1 FROM sys_role_menu WHERE role_id = 1 AND menu_id = 1001);

INSERT INTO sys_role_menu (role_id, menu_id)
SELECT 1, 1002
WHERE NOT EXISTS (SELECT 1 FROM sys_role_menu WHERE role_id = 1 AND menu_id = 1002);
