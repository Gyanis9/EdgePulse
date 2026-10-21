-- 命令计划执行与批量下发。
ALTER TABLE biz_command ADD COLUMN scheduled_at DATETIME NULL;
ALTER TABLE biz_command ADD COLUMN batch_id BIGINT NOT NULL DEFAULT 0;
CREATE INDEX idx_biz_command_due ON biz_command (tenant_id, status, scheduled_at);
