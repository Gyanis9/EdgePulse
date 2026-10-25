-- 未绑定命令的任务在旧库里统一是 0，多行相同 (firmware_id, node_id, 0) 无法直接建唯一索引；
-- 改成每行唯一的负值占位后，未绑定语义仍按 <= 0 判断，索引在 SQLite 与 MySQL 上都能建立。
UPDATE biz_firmware_task SET command_id = -id WHERE command_id = 0;
CREATE UNIQUE INDEX uq_fw_task_command ON biz_firmware_task (firmware_id, node_id, command_id);
