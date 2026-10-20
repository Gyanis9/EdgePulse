-- P0 核心表：认证与菜单所需的最小集合。
-- 所有时间列用 DATETIME，由引擎的 LocalDateTime/Timestamp 映射；ID 由应用生成，不依赖自增语法。
CREATE TABLE IF NOT EXISTS sys_user (
    id BIGINT PRIMARY KEY,
    username VARCHAR(64) NOT NULL,
    password VARCHAR(255) NOT NULL,
    nickname VARCHAR(64) NOT NULL,
    phone VARCHAR(32) NOT NULL DEFAULT '',
    email VARCHAR(128) NOT NULL DEFAULT '',
    dept_id BIGINT NOT NULL DEFAULT 0,
    status TINYINT NOT NULL DEFAULT 1,
    last_login_time DATETIME NULL,
    deleted TINYINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by BIGINT NOT NULL DEFAULT 0,
    updated_by BIGINT NOT NULL DEFAULT 0,
    CONSTRAINT uq_sys_user_username UNIQUE (username)
);

CREATE TABLE IF NOT EXISTS sys_role (
    id BIGINT PRIMARY KEY,
    role_name VARCHAR(64) NOT NULL,
    role_code VARCHAR(64) NOT NULL,
    data_scope VARCHAR(32) NOT NULL DEFAULT 'SELF',
    builtin TINYINT NOT NULL DEFAULT 0,
    status TINYINT NOT NULL DEFAULT 1,
    deleted TINYINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by BIGINT NOT NULL DEFAULT 0,
    updated_by BIGINT NOT NULL DEFAULT 0,
    CONSTRAINT uq_sys_role_code UNIQUE (role_code)
);

CREATE TABLE IF NOT EXISTS sys_menu (
    id BIGINT PRIMARY KEY,
    parent_id BIGINT NOT NULL DEFAULT 0,
    menu_name VARCHAR(64) NOT NULL,
    menu_type VARCHAR(16) NOT NULL,
    icon VARCHAR(64) NOT NULL DEFAULT '',
    path VARCHAR(128) NOT NULL DEFAULT '',
    component VARCHAR(128) NOT NULL DEFAULT '',
    permission VARCHAR(128) NOT NULL DEFAULT '',
    sort INT NOT NULL DEFAULT 0,
    visible TINYINT NOT NULL DEFAULT 1,
    status TINYINT NOT NULL DEFAULT 1,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by BIGINT NOT NULL DEFAULT 0,
    updated_by BIGINT NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS sys_user_role (
    user_id BIGINT NOT NULL,
    role_id BIGINT NOT NULL,
    PRIMARY KEY (user_id, role_id)
);

CREATE TABLE IF NOT EXISTS sys_role_menu (
    role_id BIGINT NOT NULL,
    menu_id BIGINT NOT NULL,
    PRIMARY KEY (role_id, menu_id)
);

CREATE TABLE IF NOT EXISTS sys_login_log (
    id BIGINT PRIMARY KEY,
    username VARCHAR(64) NOT NULL,
    ip VARCHAR(64) NOT NULL DEFAULT '',
    status TINYINT NOT NULL,
    message VARCHAR(255) NOT NULL DEFAULT '',
    trace_id VARCHAR(64) NOT NULL DEFAULT '',
    login_time DATETIME NOT NULL
);

CREATE INDEX idx_sys_menu_parent_id ON sys_menu (parent_id);
CREATE INDEX idx_sys_login_log_login_time ON sys_login_log (login_time);
