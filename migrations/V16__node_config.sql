-- 设备影子：节点期望/实际配置、漂移判定与不可变修订。
CREATE TABLE IF NOT EXISTS biz_node_config (
    node_id BIGINT PRIMARY KEY,
    tenant_id BIGINT NOT NULL,
    desired_json TEXT NOT NULL,
    desired_version BIGINT NOT NULL DEFAULT 0,
    desired_hash CHAR(64) NOT NULL DEFAULT '',
    desired_updated_at DATETIME NOT NULL,
    desired_updated_by BIGINT NOT NULL DEFAULT 0,
    reported_json TEXT NOT NULL,
    reported_version BIGINT NOT NULL DEFAULT 0,
    reported_hash CHAR(64) NOT NULL DEFAULT '',
    reported_status VARCHAR(16) NOT NULL DEFAULT '',
    reported_message VARCHAR(255) NOT NULL DEFAULT '',
    reported_at DATETIME NULL
);

CREATE TABLE IF NOT EXISTS biz_node_config_revision (
    id BIGINT PRIMARY KEY,
    tenant_id BIGINT NOT NULL,
    node_id BIGINT NOT NULL,
    version BIGINT NOT NULL,
    content_json TEXT NOT NULL,
    content_hash CHAR(64) NOT NULL,
    note VARCHAR(255) NOT NULL DEFAULT '',
    created_by BIGINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL,
    CONSTRAINT uq_node_config_revision UNIQUE (node_id, version)
);
CREATE INDEX idx_node_config_revision_tenant ON biz_node_config_revision (tenant_id, node_id, version);
