-- P1 收口：把日志归属到用户与部门，数据范围查询才能落在 SQL 上。
ALTER TABLE sys_oper_log ADD COLUMN oper_id BIGINT NOT NULL DEFAULT 0;
ALTER TABLE sys_oper_log ADD COLUMN dept_id BIGINT NOT NULL DEFAULT 0;
ALTER TABLE sys_login_log ADD COLUMN user_id BIGINT NOT NULL DEFAULT 0;
ALTER TABLE sys_login_log ADD COLUMN dept_id BIGINT NOT NULL DEFAULT 0;

CREATE INDEX idx_sys_oper_log_dept_time ON sys_oper_log (dept_id, oper_time);
CREATE INDEX idx_sys_login_log_dept_time ON sys_login_log (dept_id, login_time);
