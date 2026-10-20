-- P2 设备域：节点分组、遥测与告警。
CREATE TABLE IF NOT EXISTS biz_node_group (
    id BIGINT PRIMARY KEY,
    parent_id BIGINT NOT NULL DEFAULT 0,
    name VARCHAR(64) NOT NULL,
    sort INT NOT NULL DEFAULT 0,
    remark VARCHAR(255) NOT NULL DEFAULT '',
    deleted TINYINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by BIGINT NOT NULL DEFAULT 0,
    updated_by BIGINT NOT NULL DEFAULT 0
);
CREATE INDEX idx_biz_node_group_parent ON biz_node_group (parent_id);

CREATE TABLE IF NOT EXISTS biz_telemetry (
    id BIGINT PRIMARY KEY,
    node_id BIGINT NOT NULL,
    metric VARCHAR(64) NOT NULL,
    value DOUBLE NOT NULL,
    unit VARCHAR(32) NOT NULL DEFAULT '',
    labels JSON NULL,
    collected_at DATETIME NOT NULL,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX idx_biz_telemetry_node_metric ON biz_telemetry (node_id, metric, collected_at);
CREATE INDEX idx_biz_telemetry_time ON biz_telemetry (collected_at);

CREATE TABLE IF NOT EXISTS biz_alarm_rule (
    id BIGINT PRIMARY KEY,
    name VARCHAR(128) NOT NULL,
    node_id BIGINT NOT NULL DEFAULT 0,
    metric VARCHAR(64) NOT NULL,
    comparison VARCHAR(4) NOT NULL,
    threshold DOUBLE NOT NULL,
    level VARCHAR(16) NOT NULL DEFAULT 'warning',
    enabled TINYINT NOT NULL DEFAULT 1,
    cooldown_seconds INT NOT NULL DEFAULT 300,
    deleted TINYINT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by BIGINT NOT NULL DEFAULT 0,
    updated_by BIGINT NOT NULL DEFAULT 0
);
CREATE INDEX idx_biz_alarm_rule_metric ON biz_alarm_rule (metric);

CREATE TABLE IF NOT EXISTS biz_alarm (
    id BIGINT PRIMARY KEY,
    rule_id BIGINT NOT NULL,
    node_id BIGINT NOT NULL,
    level VARCHAR(16) NOT NULL,
    metric VARCHAR(64) NOT NULL,
    value DOUBLE NOT NULL,
    threshold DOUBLE NOT NULL,
    status VARCHAR(16) NOT NULL DEFAULT 'open',
    message VARCHAR(255) NOT NULL DEFAULT '',
    triggered_at DATETIME NOT NULL,
    acknowledged_at DATETIME NULL,
    acknowledged_by BIGINT NOT NULL DEFAULT 0,
    resolved_at DATETIME NULL,
    resolved_by BIGINT NOT NULL DEFAULT 0,
    dedup_key VARCHAR(128) NOT NULL DEFAULT '',
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX idx_biz_alarm_status ON biz_alarm (status);
CREATE INDEX idx_biz_alarm_node_time ON biz_alarm (node_id, triggered_at);

INSERT INTO biz_alarm_rule (id, name, node_id, metric, comparison, threshold, level, enabled, cooldown_seconds, deleted) VALUES
    (300, 'CPU 使用率过高', 0, 'cpu.usage', '>', 90, 'critical', 1, 300, 0),
    (301, '内存使用率过高', 0, 'memory.usage', '>', 85, 'warning', 1, 300, 0),
    (302, '磁盘使用率过高', 0, 'disk.usage', '>', 90, 'warning', 1, 300, 0);

INSERT INTO biz_node_group (id, parent_id, name, sort, remark, deleted) VALUES
    (200, 0, '默认分组', 10, '未显式分组的节点归入这里', 0);
