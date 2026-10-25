-- 控制面单飞租约：holder 为空或已过期的实例可以接手，fence 每次接手递增
CREATE TABLE IF NOT EXISTS sys_lease (
    name VARCHAR(64) PRIMARY KEY,
    holder VARCHAR(128) NOT NULL DEFAULT '',
    fence BIGINT NOT NULL DEFAULT 0,
    expires_at DATETIME NULL,
    updated_at DATETIME NOT NULL
);

INSERT INTO sys_lease (name, holder, fence, expires_at, updated_at)
VALUES ('control-plane', '', 0, NULL, '1970-01-01 00:00:00');
