-- 企业身份与 API：可轮换的 API 令牌、写请求幂等回放、用户 MFA 密钥。
CREATE TABLE IF NOT EXISTS sys_api_token (
    id BIGINT PRIMARY KEY,
    tenant_id BIGINT NOT NULL,
    owner_user_id BIGINT NOT NULL,
    name VARCHAR(64) NOT NULL,
    token_prefix VARCHAR(16) NOT NULL,
    token_hash CHAR(64) NOT NULL,
    previous_hash CHAR(64) NULL,
    previous_valid_until DATETIME NULL,
    scopes_json TEXT NOT NULL,
    expires_at DATETIME NOT NULL,
    last_used_at DATETIME NULL,
    revoked_at DATETIME NULL,
    created_by BIGINT NOT NULL,
    created_at DATETIME NOT NULL,
    UNIQUE (token_hash)
);
CREATE INDEX idx_api_token_tenant ON sys_api_token (tenant_id, revoked_at, expires_at);
CREATE INDEX idx_api_token_previous ON sys_api_token (previous_hash);

CREATE TABLE IF NOT EXISTS sys_idempotency (
    id BIGINT PRIMARY KEY,
    token_hash CHAR(64) NOT NULL,
    key_hash CHAR(64) NOT NULL,
    request_hash CHAR(64) NOT NULL,
    method VARCHAR(8) NOT NULL,
    path VARCHAR(255) NOT NULL,
    status VARCHAR(16) NOT NULL,
    response_status INT NULL,
    response_trace_id VARCHAR(64) NOT NULL DEFAULT '',
    response_content_type VARCHAR(128) NOT NULL DEFAULT '',
    response_body MEDIUMTEXT NULL,
    created_at DATETIME NOT NULL,
    expires_at DATETIME NOT NULL,
    UNIQUE (token_hash, key_hash)
);
CREATE INDEX idx_idempotency_expires ON sys_idempotency (expires_at);

CREATE TABLE IF NOT EXISTS sys_user_mfa (
    user_id BIGINT PRIMARY KEY,
    tenant_id BIGINT NOT NULL,
    secret_ciphertext TEXT NOT NULL,
    secret_nonce VARCHAR(32) NOT NULL,
    key_version INT NOT NULL,
    last_used_step BIGINT NOT NULL DEFAULT 0,
    recovery_codes_json TEXT NOT NULL,
    enabled TINYINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL,
    updated_at DATETIME NOT NULL
);
CREATE INDEX idx_user_mfa_tenant ON sys_user_mfa (tenant_id);

INSERT INTO sys_menu (id, parent_id, menu_name, menu_type, icon, path, component, permission, sort, visible, status) VALUES
    (1210, 1100, 'API 令牌', 'menu', 'key', '/system/apitoken', 'system/apitoken/index', 'system:token:list', 91, 1, 1),
    (1211, 1210, '令牌新增', 'btn', '', '', '', 'system:token:add', 10, 1, 1),
    (1212, 1210, '令牌轮换', 'btn', '', '', '', 'system:token:edit', 20, 1, 1),
    (1213, 1210, '令牌吊销', 'btn', '', '', '', 'system:token:remove', 30, 1, 1);

INSERT INTO sys_role_menu (role_id, menu_id) VALUES
    (1, 1210), (1, 1211), (1, 1212), (1, 1213),
    (3, 1210), (3, 1211), (3, 1212), (3, 1213);
