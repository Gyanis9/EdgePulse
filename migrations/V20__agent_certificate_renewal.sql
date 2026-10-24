-- Agent 证书自动轮换：节点表保存当前与上一枚证书，历史表记录每次签发。
ALTER TABLE biz_node ADD COLUMN previous_cert_fingerprint CHAR(64) NOT NULL DEFAULT '';
ALTER TABLE biz_node ADD COLUMN previous_cert_not_after DATETIME NULL;

CREATE TABLE IF NOT EXISTS biz_agent_certificate (
    id BIGINT PRIMARY KEY,
    tenant_id BIGINT NOT NULL,
    node_id BIGINT NOT NULL,
    serial VARCHAR(64) NOT NULL,
    fingerprint CHAR(64) NOT NULL,
    subject VARCHAR(255) NOT NULL DEFAULT '',
    not_before DATETIME NOT NULL,
    not_after DATETIME NOT NULL,
    status VARCHAR(16) NOT NULL,
    issued_at DATETIME NOT NULL,
    revoked_at DATETIME NULL
);
CREATE INDEX idx_agent_certificate_node ON biz_agent_certificate (tenant_id, node_id, issued_at);
CREATE INDEX idx_agent_certificate_fingerprint ON biz_agent_certificate (fingerprint);
