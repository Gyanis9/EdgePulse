-- 登录加固：按账号累计失败次数，达到阈值后锁定到指定时刻。
ALTER TABLE sys_user ADD COLUMN failed_login_count INT NOT NULL DEFAULT 0;
ALTER TABLE sys_user ADD COLUMN last_failed_login_at DATETIME NULL;
ALTER TABLE sys_user ADD COLUMN locked_until DATETIME NULL;
