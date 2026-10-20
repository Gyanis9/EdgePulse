#!/usr/bin/env python3
"""UDP 遥测验收：独立端口的服务、真实数据报、签名校验与落库回读。"""

from __future__ import annotations

import hashlib
import hmac
import json
import os
import secrets
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / "build" / "gate"
CONFIG_PATH = GATE / "udp-check.yaml"
SERVER_LOG = GATE / "udp-server.log"
DATABASE_PATH = GATE / "udp-check.db"
HTTP = "http://127.0.0.1:18080"
OPS = "http://127.0.0.1:19100"
UDP_PORT = 19555


def request(method: str, url: str, payload: dict | None = None, token: str | None = None, timeout: float = 10.0) -> tuple[int, dict]:
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {"Content-Type": "application/json"} if data is not None else {}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            body = response.read().decode("utf-8", "replace")
            return response.status, json.loads(body) if body else {}
    except urllib.error.HTTPError as failure:
        body = failure.read().decode("utf-8", "replace")
        try:
            return failure.code, json.loads(body) if body else {}
        except json.JSONDecodeError:
            return failure.code, {}


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
  database:
    driver: sqlite
    path: build/gate/udp-check.db
  redis:
    enabled: false
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
  udp_telemetry:
    enabled: true
    address: 127.0.0.1
    port: 19555

metrics:
  enabled: false
""",
        encoding="utf-8",
    )


def server_binary() -> Path:
    for name in ("edgepulse-server.exe", "edgepulse-server"):
        candidate = ROOT / "build" / "debug" / name
        if candidate.exists():
            return candidate
    return ROOT / "build" / "debug" / "edgepulse-server"


def main() -> int:
    GATE.mkdir(parents=True, exist_ok=True)
    write_configuration()
    if DATABASE_PATH.exists():
        DATABASE_PATH.unlink()
    secret = secrets.token_hex(32)
    process: subprocess.Popen[bytes] | None = None
    try:
        with SERVER_LOG.open("wb") as log:
            process = subprocess.Popen([str(server_binary()), "--config", str(CONFIG_PATH)], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                       env={**os.environ, "EDGEPULSE_UDP_TELEMETRY_SECRET": secret})
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                status, _ = request("GET", HTTP + "/api/health", timeout=1)
                if status == 200:
                    break
            except (urllib.error.URLError, TimeoutError, ConnectionError):
                pass
            if process.poll() is not None:
                print("FAIL UDP 遥测服务启动")
                return 1
            time.sleep(0.2)
        else:
            print("FAIL UDP 遥测服务未在时限内就绪")
            return 1

        status, captcha = request("GET", HTTP + "/api/auth/captcha")
        status, login = request("POST", HTTP + "/api/auth/login",
                                {"username": "admin", "password": "admin123", "captcha": captcha.get("data", {}).get("code"),
                                 "captchaUuid": captcha.get("data", {}).get("uuid")})
        token = login.get("data", {}).get("token", "")
        if status != 200 or not token:
            print("FAIL UDP 遥测用例登录")
            return 1
        status, node = request("POST", HTTP + "/api/nodes", {"name": "udp-check-node", "groupId": 200}, token)
        node_id = node.get("data", {}).get("id")
        if status != 200 or not node_id:
            print("FAIL UDP 遥测节点创建")
            return 1

        body = {"samples": [{"metric": "udp.cpu", "value": 42.5, "unit": "%"}]}
        body_text = json.dumps(body, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        timestamp = int(time.time())
        nonce = secrets.token_hex(8)
        canonical = f"v1\n{node_id}\n{timestamp}\n{nonce}\n{body_text}"
        signature = hmac.new(secret.encode(), canonical.encode(), hashlib.sha256).hexdigest()
        datagram = json.dumps({"nodeId": node_id, "timestamp": timestamp, "nonce": nonce, "body": body, "signature": signature},
                              separators=(",", ":"), ensure_ascii=False).encode()

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.settimeout(3.0)
            client.sendto(datagram, ("127.0.0.1", UDP_PORT))
            reply = json.loads(client.recv(4096).decode("utf-8"))
        print(("PASS " if reply.get("accepted") == 1 else "FAIL ") + "UDP 数据报签名与接收：" + json.dumps(reply, ensure_ascii=False))

        metric = None
        deadline = time.time() + 5
        while time.time() < deadline and metric is None:
            status, latest = request("GET", HTTP + f"/api/telemetry/latest?nodeId={node_id}", token=token)
            for point in latest.get("data", []):
                if point.get("metric") == "udp.cpu":
                    metric = point
                    break
            if metric is None:
                time.sleep(0.2)
        print(("PASS " if metric and metric.get("value") == 42.5 else "FAIL ") + "UDP 遥测落库并可回读：" + json.dumps(metric, ensure_ascii=False))
        return 0 if reply.get("accepted") == 1 and metric and metric.get("value") == 42.5 else 1
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
        if DATABASE_PATH.exists():
            DATABASE_PATH.unlink()


if __name__ == "__main__":
    sys.exit(main())
