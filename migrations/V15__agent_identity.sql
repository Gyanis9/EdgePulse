-- Agent 身份闭环：证书到期/吊销与一次性注册/改绑令牌。
ALTER TABLE biz_node ADD COLUMN cert_not_after DATETIME NULL;
ALTER TABLE biz_node ADD COLUMN cert_revoked_at DATETIME NULL;
CREATE INDEX idx_biz_node_sweep ON biz_node (status, last_seen);
CREATE INDEX idx_biz_node_tenant_status ON biz_node (tenant_id, status);

CREATE TABLE IF NOT EXISTS biz_agent_enrollment (
    id BIGINT PRIMARY KEY,
    tenant_id BIGINT NOT NULL,
    node_name VARCHAR(128) NOT NULL DEFAULT '',
    action VARCHAR(16) NOT NULL,
    token_hash CHAR(64) NOT NULL,
    expires_at DATETIME NOT NULL,
    used_at DATETIME NULL,
    revoked_at DATETIME NULL,
    created_by BIGINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL
);
CREATE UNIQUE INDEX uq_agent_enrollment_hash ON biz_agent_enrollment (token_hash);
CREATE INDEX idx_agent_enrollment_tenant ON biz_agent_enrollment (tenant_id, created_at);

CREATE INDEX idx_biz_alarm_node_metric_status ON biz_alarm (node_id, metric, status);
