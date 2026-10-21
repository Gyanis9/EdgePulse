-- 固件回滚：记录每个任务下发前的版本，回滚时据此找回上一版本。
ALTER TABLE biz_firmware_task ADD COLUMN previous_firmware_id BIGINT NOT NULL DEFAULT 0;
CREATE INDEX idx_firmware_task_previous ON biz_firmware_task (tenant_id, node_id, firmware_id);
