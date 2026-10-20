-- P0 业务表：边缘节点。
-- version 用于乐观锁；删除是软删除，所有查询都要带 deleted = 0。
CREATE TABLE IF NOT EXISTS biz_node (
    id BIGINT PRIMARY KEY,
    name VARCHAR(128) NOT NULL,
    status VARCHAR(16) NOT NULL DEFAULT 'offline',
    agent_version VARCHAR(64) NOT NULL DEFAULT '',
    cert_fingerprint VARCHAR(64) NOT NULL DEFAULT '',
    labels JSON NULL,
    last_seen DATETIME NULL,
    version BIGINT NOT NULL DEFAULT 1,
    deleted TINYINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by BIGINT NOT NULL DEFAULT 0,
    updated_by BIGINT NOT NULL DEFAULT 0,
    CONSTRAINT uq_biz_node_name UNIQUE (name)
);

CREATE INDEX idx_biz_node_status ON biz_node (status);
CREATE INDEX idx_biz_node_last_seen ON biz_node (last_seen);
