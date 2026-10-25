-- 命令截止时间在创建与下发时写入，超时扫描按 (status, deadline_at) 取最早到期的一批，
-- 不再先按固定窗口取候选再在内存判定。升级前已在途的命令保持 NULL，不参与扫描。
ALTER TABLE biz_command ADD COLUMN deadline_at DATETIME NULL;
CREATE INDEX idx_biz_command_status_deadline ON biz_command (status, deadline_at);
