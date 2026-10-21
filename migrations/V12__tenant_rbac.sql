-- 租户级 RBAC：角色、部门、岗位、字典归租户；菜单与权限码仍是平台模板。
ALTER TABLE sys_role ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE sys_dept ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE sys_post ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE sys_dict_type ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;
ALTER TABLE sys_dict_data ADD COLUMN tenant_id BIGINT NOT NULL DEFAULT 1;

CREATE INDEX idx_sys_role_tenant ON sys_role (tenant_id);
CREATE INDEX idx_sys_dept_tenant ON sys_dept (tenant_id);
CREATE INDEX idx_sys_post_tenant ON sys_post (tenant_id);
CREATE INDEX idx_sys_dict_type_tenant ON sys_dict_type (tenant_id);
CREATE INDEX idx_sys_dict_data_tenant ON sys_dict_data (tenant_id, dict_type);

-- 租户管理员模板补上租户内可自助维护的角色、部门、岗位与字典页面；
-- 菜单管理与参数配置仍只属于平台管理员。
INSERT INTO sys_role_menu (role_id, menu_id)
SELECT 3, id FROM sys_menu
WHERE id IN (1102, 1120, 1121, 1122, 1123, 1104, 1140, 1141, 1142, 1143, 1105, 1150, 1151, 1152, 1153, 1106, 1160, 1161, 1162, 1163)
  AND id NOT IN (SELECT menu_id FROM sys_role_menu WHERE role_id = 3);
