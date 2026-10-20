-- P2 设备域：固件与命令。
CREATE TABLE IF NOT EXISTS biz_firmware (
    id BIGINT PRIMARY KEY,
    name VARCHAR(128) NOT NULL,
    version VARCHAR(64) NOT NULL,
    file_name VARCHAR(255) NOT NULL,
    content_type VARCHAR(128) NOT NULL DEFAULT 'application/octet-stream',
    size_bytes BIGINT NOT NULL DEFAULT 0,
    sha256 VARCHAR(64) NOT NULL,
    storage_path VARCHAR(512) NOT NULL,
    created_by BIGINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    deleted TINYINT NOT NULL DEFAULT 0
);
CREATE INDEX idx_biz_firmware_created ON biz_firmware (created_at);

CREATE TABLE IF NOT EXISTS biz_firmware_task (
    id BIGINT PRIMARY KEY,
    firmware_id BIGINT NOT NULL,
    node_id BIGINT NOT NULL,
    status VARCHAR(16) NOT NULL DEFAULT 'pending',
    progress INT NOT NULL DEFAULT 0,
    message VARCHAR(255) NOT NULL DEFAULT '',
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX idx_biz_firmware_task_node ON biz_firmware_task (node_id, status);

CREATE TABLE IF NOT EXISTS biz_command (
    id BIGINT PRIMARY KEY,
    node_id BIGINT NOT NULL,
    type VARCHAR(64) NOT NULL,
    payload TEXT NULL,
    status VARCHAR(16) NOT NULL DEFAULT 'pending',
    result TEXT NULL,
    message VARCHAR(512) NOT NULL DEFAULT '',
    created_by BIGINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    dispatched_at DATETIME NULL,
    finished_at DATETIME NULL,
    timeout_seconds INT NOT NULL DEFAULT 60
);
CREATE INDEX idx_biz_command_node_status ON biz_command (node_id, status);
CREATE INDEX idx_biz_command_created ON biz_command (created_at);
