#!/usr/bin/env python3
"""OTLP/HTTP 导出验收：起一个 mock collector，启动独立端口的服务，断言收到指标 POST。"""

from __future__ import annotations

import json
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / "build" / "gate"
CONFIG_PATH = GATE / "otlp-check.yaml"
SERVER_LOG = GATE / "otlp-server.log"
DATABASE_PATH = GATE / "otlp-check.db"
COLLECTOR_PORT = 14318

received: list[dict[str, str]] = []
received_lock = threading.Lock()


class CollectorHandler(BaseHTTPRequestHandler):
    def do_POST(self) -> None:  # noqa: N802
        size = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(size).decode("utf-8", "replace")
        with received_lock:
            received.append({"path": self.path, "type": self.headers.get("Content-Type", ""), "body": body})
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"{}")

    def log_message(self, format: str, *args: object) -> None:
        pass


def write_configuration() -> None:
    CONFIG_PATH.write_text(
        """edgepulse:
  listen_address: 127.0.0.1
  threads: 1
  hot_reload: false
  http_port: 18080
  https_port: 18443
  agent_port: 19443
  ops_port: 19100
  ops_address: 127.0.0.1
  web_root: web
  cert_dir: var/certs
  firmware_dir: var/firmware
  maximum_firmware_bytes: 268435456
  telemetry_retention_days: 30
  database:
    driver: sqlite
    path: build/gate/otlp-check.db
    host: 127.0.0.1
    port: 3306
    name: edgepulse
    user: edgepulse
  redis:
    enabled: false
    host: 127.0.0.1
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
    allowed_origins: ""
  agent:
    fingerprint: ""

server:
  limits:
    idle_timeout_ms: 75000
    read_timeout_ms: 60000
    write_timeout_ms: 60000
  parser_limits:
    maximum_body_size: 16777216
  rate_limit:
    requests_per_second: 0.0
    burst_capacity: 1.0

metrics:
  enabled: true
  service_name: EdgePulse
  service_version: 1.0.0
  export_interval_ms: 300
  otlp:
    endpoint: http://127.0.0.1:14318/v1/metrics
    timeout_ms: 2000
""",
        encoding="utf-8",
    )


def server_binary() -> Path:
    for name in ("edgepulse-server.exe", "edgepulse-server"):
        candidate = ROOT / "build" / "debug" / name
        if candidate.exists():
            return candidate
    return ROOT / "build" / "debug" / "edgepulse-server"


def find_metric_name(payload: object) -> str | None:
    """在 OTLP JSON 里找第一条指标名，用来证明导出的是真实读数。"""
    if isinstance(payload, dict):
        for key, value in payload.items():
            if key == "name" and isinstance(value, str):
                return value
            found = find_metric_name(value)
            if found is not None:
                return found
    elif isinstance(payload, list):
        for item in payload:
            found = find_metric_name(item)
            if found is not None:
                return found
    return None


def main() -> int:
    GATE.mkdir(parents=True, exist_ok=True)
    write_configuration()
    if DATABASE_PATH.exists():
        DATABASE_PATH.unlink()

    collector = ThreadingHTTPServer(("127.0.0.1", COLLECTOR_PORT), CollectorHandler)
    thread = threading.Thread(target=collector.serve_forever, daemon=True)
    thread.start()

    process: subprocess.Popen[bytes] | None = None
    try:
        with SERVER_LOG.open("wb") as log:
            process = subprocess.Popen([str(server_binary()), "--config", str(CONFIG_PATH)], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.time() + 20.0
        while time.time() < deadline:
            with received_lock:
                if received:
                    break
            if process.poll() is not None:
                break
            time.sleep(0.1)
        with received_lock:
            batch = list(received)
        if not batch:
            print("FAIL OTLP collector 未在 20 秒内收到请求")
            return 1
        request = batch[0]
        print("PASS OTLP collector 收到 POST " + request["path"])
        ok_path = request["path"] == "/v1/metrics"
        ok_type = "application/json" in request["type"].lower()
        try:
            payload = json.loads(request["body"])
            metric = find_metric_name(payload)
        except json.JSONDecodeError:
            metric = None
        ok_body = metric is not None
        print(("PASS " if ok_path else "FAIL ") + "OTLP 路径为 /v1/metrics")
        print(("PASS " if ok_type else "FAIL ") + "OTLP Content-Type 为 application/json")
        print(("PASS " if ok_body else "FAIL ") + "OTLP 正文含指标名：" + str(metric or "未找到"))
        return 0 if ok_path and ok_type and ok_body else 1
    finally:
        if process is not None and process.poll() is None:
            if sys.platform == "win32":
                subprocess.run(["taskkill", "/F", "/T", "/PID", str(process.pid)], capture_output=True, check=False)
            else:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
        collector.shutdown()
        collector.server_close()
        if DATABASE_PATH.exists():
            DATABASE_PATH.unlink()


if __name__ == "__main__":
    sys.exit(main())
