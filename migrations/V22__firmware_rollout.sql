-- P10 固件灰度发布：rollout 聚合、波次、目标，以及任务与节点上的关联列。
CREATE TABLE IF NOT EXISTS biz_firmware_rollout (
    id BIGINT PRIMARY KEY,
    tenant_id BIGINT NOT NULL,
    firmware_id BIGINT NOT NULL,
    name VARCHAR(128) NOT NULL,
    policy_json TEXT NOT NULL,
    status VARCHAR(24) NOT NULL,
    current_wave INT NOT NULL DEFAULT 0,
    total_waves INT NOT NULL DEFAULT 0,
    created_by BIGINT NOT NULL,
    created_at DATETIME NOT NULL,
    started_at DATETIME NULL,
    finished_at DATETIME NULL,
    paused_at DATETIME NULL,
    pause_reason VARCHAR(255) NOT NULL DEFAULT '',
    last_error VARCHAR(512) NOT NULL DEFAULT '',
    version BIGINT NOT NULL DEFAULT 1
);
CREATE INDEX idx_fw_rollout_tenant ON biz_firmware_rollout (tenant_id, firmware_id, created_at);

CREATE TABLE IF NOT EXISTS biz_firmware_rollout_wave (
    id BIGINT PRIMARY KEY,
    rollout_id BIGINT NOT NULL,
    wave_index INT NOT NULL,
    status VARCHAR(24) NOT NULL,
    target_count INT NOT NULL DEFAULT 0,
    dispatched_at DATETIME NULL,
    deadline_at DATETIME NULL,
    settled_at DATETIME NULL,
    -- TEXT 在 MySQL 8.4 不接受字面量默认值，写入时始终显式给空串
    gate_json TEXT NOT NULL,
    created_at DATETIME NOT NULL,
    updated_at DATETIME NOT NULL
);
CREATE INDEX idx_fw_rollout_wave ON biz_firmware_rollout_wave (rollout_id, wave_index);

CREATE TABLE IF NOT EXISTS biz_firmware_rollout_target (
    id BIGINT PRIMARY KEY,
    rollout_id BIGINT NOT NULL,
    wave_id BIGINT NOT NULL,
    tenant_id BIGINT NOT NULL,
    node_id BIGINT NOT NULL,
    previous_firmware_id BIGINT NOT NULL DEFAULT 0,
    task_id BIGINT NOT NULL DEFAULT 0,
    command_id BIGINT NOT NULL DEFAULT 0,
    status VARCHAR(24) NOT NULL,
    health_json TEXT NOT NULL,
    updated_at DATETIME NOT NULL,
    UNIQUE (rollout_id, node_id)
);
CREATE INDEX idx_fw_rollout_target_wave ON biz_firmware_rollout_target (wave_id, status);

ALTER TABLE biz_firmware_task ADD COLUMN rollout_id BIGINT NOT NULL DEFAULT 0;
ALTER TABLE biz_firmware_task ADD COLUMN wave_id BIGINT NOT NULL DEFAULT 0;
ALTER TABLE biz_firmware_task ADD COLUMN command_id BIGINT NOT NULL DEFAULT 0;
CREATE INDEX idx_fw_task_rollout ON biz_firmware_task (rollout_id, wave_id);

ALTER TABLE biz_node ADD COLUMN installed_firmware_version VARCHAR(64) NOT NULL DEFAULT '';
