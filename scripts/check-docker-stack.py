#!/usr/bin/env python3
"""Docker 全栈验收：镜像、MySQL/Redis、Prometheus、OTel collector 与停机。

默认复用已构建的 edgepulse:ci 镜像；加 --build 先按仓库 Dockerfile 构建。
测试专用配置把验证码设为可见，只存在 build/gate，不进入交付镜像。
"""

from __future__ import annotations

import argparse
import json
import os
import secrets
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

ROOT = Path(__file__).resolve().parents[1]
MIGRATIONS_DIR = ROOT / "migrations"
MIGRATION_COUNT = len(list(MIGRATIONS_DIR.glob("V*__*.sql")))
PROJECT = "edgepulse-acceptance"
HTTP = "http://127.0.0.1:8080"
OPS = "http://127.0.0.1:9100"
PROMETHEUS = "http://127.0.0.1:9090"
GATE = ROOT / "build" / "gate"
COMPOSE_DIR = ROOT / "deploy" / "docker"
BASE_COMPOSE = COMPOSE_DIR / "docker-compose.yml"
ENV_FILE = COMPOSE_DIR / ".env"
OVERRIDE = GATE / "docker-compose.test.yml"
TEST_CONFIG = GATE / "docker-config"
COLLECTOR_CONFIG = GATE / "otel-collector.yaml"
failures: list[str] = []


def report(name: str, ok: bool, detail: str = "") -> None:
    print(("PASS " if ok else "FAIL ") + name + (("：" + detail) if detail else ""))
    if not ok:
        failures.append(name)


def prepare() -> dict[str, str]:
    GATE.mkdir(parents=True, exist_ok=True)
    if not (ROOT / "var" / "certs" / "server.pem").exists():
        subprocess.run(["bash", "scripts/dev-certs.sh"], cwd=ROOT, check=True)

    values: dict[str, str] = {}
    if ENV_FILE.exists():
        for line in ENV_FILE.read_text(encoding="utf-8").splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                key, value = line.split("=", 1)
                values[key.strip()] = value.strip()
    required = {
        "MYSQL_ROOT_PASSWORD": secrets.token_hex(16),
        "EDGEPULSE_DB_PASSWORD": secrets.token_hex(16),
        "EDGEPULSE_REDIS_PASSWORD": secrets.token_hex(16),
        "EDGEPULSE_OPS_BEARER_TOKEN": secrets.token_hex(32),
        "EDGEPULSE_AGENT_FINGERPRINT": "",
        "EDGEPULSE_INITIAL_PASSWORD": "",
    }
    for key, fallback in required.items():
        if not values.get(key):
            values[key] = fallback
    ENV_FILE.write_text("".join(f"{key}={values[key]}\n" for key in required), encoding="utf-8")
    (ROOT / "deploy" / "prometheus" / "ops-token").write_text(values["EDGEPULSE_OPS_BEARER_TOKEN"] + "\n", encoding="utf-8")

    TEST_CONFIG.mkdir(parents=True, exist_ok=True)
    (TEST_CONFIG / "prod.yaml").write_text(
        """edgepulse:
  listen_address: 0.0.0.0
  threads: 1
  hot_reload: true
  http_port: 8080
  https_port: 8443
  agent_port: 9443
  ops_port: 9100
  ops_address: 0.0.0.0
  web_root: web
  cert_dir: /etc/edgepulse/tls
  firmware_dir: /var/lib/edgepulse/firmware
  maximum_firmware_bytes: 268435456
  telemetry_retention_days: 30
  database:
    driver: mysql
    host: mysql
    port: 3306
    name: edgepulse
    user: edgepulse
  redis:
    enabled: true
    host: redis
    port: 6379
    database: 0
  migrations_dir: migrations
  captcha:
    ttl_seconds: 120
    expose_code: true
    requests_per_minute: 30
  session:
    ttl_seconds: 7200
  security:
    allowed_origins: http://127.0.0.1:8080
  agent:
    fingerprint: ""

server:
  limits:
    idle_timeout_ms: 75000
    read_timeout_ms: 60000
    write_timeout_ms: 60000
  parser_limits:
    maximum_body_size: 268435456
  rate_limit:
    requests_per_second: 0.0
    burst_capacity: 1.0

metrics:
  enabled: true
  service_name: EdgePulse
  service_version: 1.0.0
  export_interval_ms: 1000
  otlp:
    endpoint: http://otel-collector:4318/v1/metrics
    timeout_ms: 3000
""",
        encoding="utf-8",
    )
    logging = ROOT / "config" / "logging.yaml"
    if logging.exists():
        (TEST_CONFIG / "logging.yaml").write_text(logging.read_text(encoding="utf-8"), encoding="utf-8")
    COLLECTOR_CONFIG.write_text(
        """receivers:
  otlp:
    protocols:
      http:
        endpoint: 0.0.0.0:4318

processors:
  batch:

exporters:
  debug:
    verbosity: detailed

service:
  pipelines:
    metrics:
      receivers: [otlp]
      processors: [batch]
      exporters: [debug]
""",
        encoding="utf-8",
    )
    project = ROOT.as_posix()
    OVERRIDE.write_text(
        f"""services:
  edgepulse:
    image: edgepulse:ci
    volumes:
      - {project}/build/gate/docker-config:/app/config:ro
  otel-collector:
    image: otel/opentelemetry-collector-contrib:latest
    volumes:
      - {project}/build/gate/otel-collector.yaml:/etc/otelcol-contrib/config.yaml:ro
""",
        encoding="utf-8",
    )
    return values


def compose(*arguments: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    command = [
        "docker", "compose", "-p", PROJECT,
        "--project-directory", str(COMPOSE_DIR),
        "--env-file", str(ENV_FILE),
        "-f", str(BASE_COMPOSE),
        "-f", str(OVERRIDE),
        *arguments,
    ]
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", check=check)


def request(method: str, url: str, payload: dict | None = None, token: str | None = None, timeout: float = 10.0) -> tuple[int, dict]:
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {"Content-Type": "application/json"} if data is not None else {}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            body = response.read().decode("utf-8", "replace")
            try:
                return response.status, json.loads(body) if body else {}
            except json.JSONDecodeError:
                return response.status, {}
    except urllib.error.HTTPError as failure:
        body = failure.read().decode("utf-8", "replace")
        try:
            return failure.code, json.loads(body) if body else {}
        except json.JSONDecodeError:
            return failure.code, {}


def wait_for(probe, timeout: float = 120.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            status, _ = probe()
            if status == 200:
                return True
        except (urllib.error.URLError, TimeoutError, ConnectionError):
            pass
        time.sleep(1)
    return False


def login() -> str:
    status, captcha = request("GET", HTTP + "/api/auth/captcha")
    code = captcha.get("data", {}).get("code")
    report("Docker 验证码签发", status == 200 and bool(code), f"HTTP {status}")
    status, payload = request(
        "POST", HTTP + "/api/auth/login",
        {"username": "admin", "password": "admin123", "captcha": code, "captchaUuid": captcha.get("data", {}).get("uuid")},
    )
    token = payload.get("data", {}).get("token", "")
    report("Docker 管理员登录（MySQL + Redis 档）", status == 200 and len(token) == 64, f"HTTP {status}")
    return token


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", action="store_true", help="先按仓库 Dockerfile 构建 edgepulse:ci")
    arguments = parser.parse_args()
    values = prepare()

    if arguments.build:
        build_command = ["docker", "build", "-f", "deploy/docker/Dockerfile", "-t", "edgepulse:ci"]
        node_image  = os.environ.get("EDGEPULSE_DOCKER_NODE_IMAGE")
        apt_mirror  = os.environ.get("EDGEPULSE_DOCKER_APT_MIRROR")
        if node_image:
            build_command += ["--build-arg", f"NODE_IMAGE={node_image}"]
        if apt_mirror:
            build_command += ["--build-arg", f"APT_MIRROR={apt_mirror}"]
        build_command.append(".")
        result = subprocess.run(build_command, cwd=ROOT, check=False)
        report("构建 edgepulse:ci 镜像", result.returncode == 0)
        if result.returncode != 0:
            return 1
    else:
        probe = subprocess.run(["docker", "image", "inspect", "edgepulse:ci"], capture_output=True, check=False)
        report("复用已构建的 edgepulse:ci 镜像", probe.returncode == 0)
        if probe.returncode != 0:
            return 1

    ops_token = values["EDGEPULSE_OPS_BEARER_TOKEN"]
    try:
        compose("up", "-d", "mysql", "redis", "otel-collector", "edgepulse", "prometheus")
        report("compose 启动 MySQL/Redis/OTel/EdgePulse/Prometheus", True)

        ready = wait_for(lambda: request("GET", HTTP + "/api/health"))
        report("8080 /api/health 就绪", ready)
        if not ready:
            return 1
        status, payload = request("GET", OPS + "/readyz")
        report("9100 /readyz 就绪", status == 200 and payload.get("status") == "ready", f"HTTP {status}")

        status, _ = request("GET", OPS + "/metrics")
        report("/metrics 无令牌被拒", status == 401, f"HTTP {status}")
        status, _ = request("GET", OPS + "/metrics", token=ops_token)
        report("/metrics 带令牌可读", status == 200, f"HTTP {status}")

        token = login()
        if not token:
            return 1

        suffix = str(int(time.time()))[-8:]
        role_code = "docker_custom_" + suffix
        status, created = request(
            "POST", HTTP + "/api/system/roles",
            {"name": "Docker 自定义角色 " + suffix, "code": role_code, "dataScope": "CUSTOM", "deptIds": [100, 101], "menuIds": [1300]},
            token,
        )
        role_id = created.get("data", {}).get("id")
        report("MySQL 档创建 CUSTOM 角色", status == 200 and bool(role_id), f"HTTP {status}")
        status, depts = request("GET", HTTP + f"/api/system/roles/{role_id}/depts", token=token)
        report("MySQL 档读回角色部门", status == 200 and depts.get("data", {}).get("deptIds") == [100, 101], json.dumps(depts.get("data"), ensure_ascii=False))

        node_name = "docker-node-" + suffix
        status, node = request("POST", HTTP + "/api/nodes", {"name": node_name, "groupId": 200}, token)
        node_id = node.get("data", {}).get("id")
        report("MySQL 档创建节点", status == 200 and bool(node_id), f"HTTP {status}")
        status, listing = request("GET", HTTP + f"/api/nodes?pageNum=1&pageSize=10&keyword={node_name}", token=token)
        rows = listing.get("data", {}).get("rows", [])
        report("MySQL 档读回节点", status == 200 and any(row.get("name") == node_name for row in rows), f"HTTP {status}")

        status, oper = request("GET", HTTP + "/api/monitor/oper-logs?pageNum=1&pageSize=10", token=token)
        report("MySQL 档审计日志可读", status == 200 and oper.get("data", {}).get("total", 0) >= 1, f"HTTP {status}")

        mysql_check = compose(
            "exec", "-T", "mysql", "sh", "-c",
            "MYSQL_PWD=\"$MYSQL_ROOT_PASSWORD\" mysql -uroot -N -B edgepulse -e "
            "\"SELECT CONCAT((SELECT COUNT(*) FROM schema_migrations),'|',"
            "(SELECT COUNT(*) FROM sys_user WHERE username='admin'),'|',"
            f"(SELECT COUNT(*) FROM sys_role_dept WHERE role_id={role_id}),'|',"
            f"(SELECT COUNT(*) FROM biz_node WHERE name='{node_name}'),'|',"
            "(SELECT COUNT(*) FROM sys_tenant));\"",
        )
        stored = mysql_check.stdout.strip().split("|")
        report("MySQL 迁移、种子、角色部门、节点与租户落库", stored == [str(MIGRATION_COUNT), "1", "2", "1", "1"], mysql_check.stdout.strip() or mysql_check.stderr.strip())

        redis_check = compose("exec", "-T", "redis", "redis-cli", "--no-auth-warning", "-a", values["EDGEPULSE_REDIS_PASSWORD"], "KEYS", "edgepulse:session:*")
        session_keys = [line for line in redis_check.stdout.splitlines() if line.strip()]
        report("Redis 保存会话", len(session_keys) >= 1, f"{len(session_keys)} 个会话键")

        status, _ = request("GET", HTTP + "/api/auth/me", token=token)
        report("重启前会话有效", status == 200, f"HTTP {status}")
        compose("restart", "edgepulse")
        report("重启后健康", wait_for(lambda: request("GET", HTTP + "/api/health")))
        status, me = request("GET", HTTP + "/api/auth/me", token=token)
        report("重启后 Redis 会话仍有效", status == 200 and me.get("data", {}).get("username") == "admin", f"HTTP {status}")

        deadline = time.time() + 30
        collector_ok = False
        while time.time() < deadline:
            logs = compose("logs", "--no-color", "otel-collector")
            output = logs.stdout + logs.stderr
            if "edgepulse_build_info" in output or "asyn_" in output:
                collector_ok = True
                break
            time.sleep(1)
        report("OTel collector 收到指标", collector_ok)

        deadline = time.time() + 45
        prometheus_ok = False
        prometheus_detail = ""
        while time.time() < deadline:
            try:
                status, targets = request("GET", PROMETHEUS + "/api/v1/targets")
                active = targets.get("data", {}).get("activeTargets", [])
                edgepulse_targets = [target for target in active if target.get("labels", {}).get("job") == "edgepulse"]
                if edgepulse_targets and all(target.get("health") == "up" for target in edgepulse_targets):
                    prometheus_ok = True
                    prometheus_detail = "edgepulse 抓取目标 up"
                    break
                prometheus_detail = f"目标数 {len(edgepulse_targets)}"
            except (urllib.error.URLError, TimeoutError, ConnectionError):
                prometheus_detail = "Prometheus 尚未就绪"
            time.sleep(1)
        report("Prometheus 抓取 edgepulse", prometheus_ok, prometheus_detail)

        container = compose("ps", "-aq", "edgepulse").stdout.strip()
        stop = compose("stop", "edgepulse", check=False)
        inspect = subprocess.run(["docker", "inspect", "-f", "{{.State.ExitCode}}", container], capture_output=True, text=True, check=False)
        report("SIGTERM 优雅停机", stop.returncode == 0 and inspect.stdout.strip() == "0", f"stop rc={stop.returncode} exit={inspect.stdout.strip()}")
    finally:
        logs = compose("logs", "--no-color", check=False)
        (GATE / "docker-stack.log").write_text(logs.stdout + logs.stderr, encoding="utf-8")
        compose("down", "-v", "--remove-orphans", check=False)

    print()
    if failures:
        print(f"Docker 验收失败：{len(failures)} 项 -> " + ", ".join(failures))
        return 1
    print("Docker 全栈验收全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
