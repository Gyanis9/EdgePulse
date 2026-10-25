#!/usr/bin/env python3
"""EdgePulse 双实例 HA 验收：Compose 启栈、跨实例会话、Agent 命令路由、选主与优雅停机。

默认复用已构建的 edgepulse:ha 镜像；加 --build 先按仓库 Dockerfile 构建。
测试专用配置把验证码设为可见、关闭固件签名，只存在 build/gate/ha，不进入交付镜像。
Docker 或 Compose 不可用时打印 SKIP 并以 0 退出；栈一旦启动，任何断言失败都以 1 退出。
"""

from __future__ import annotations

import argparse
import atexit
import base64
import hashlib
import http.client
import json
import os
import re
import secrets
import shutil
import socket
import ssl
import subprocess
import sys
import time
import urllib.parse
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

ROOT = Path(__file__).resolve().parents[1]
COMPOSE_DIR = ROOT / "deploy" / "docker"
BASE_COMPOSE = COMPOSE_DIR / "docker-compose.ha.yml"
CERTS = ROOT / "var" / "certs"
GATE = ROOT / "build" / "gate" / "ha"
ENV_FILE = GATE / "ha.env"
OVERRIDE = GATE / "docker-compose.ha.test.yml"
CONFIG_DIR = GATE / "config"
FIRMWARE_KEYS = GATE / "firmware-keys"
LOG_PATH = GATE / "ha-stack.log"
CA_KEY = GATE / "agent-ca-key.pem"
PROJECT = "edgepulse-ha-check"
IMAGE = "edgepulse:ha"
HOST = "127.0.0.1"

# 实例直连端口只用于验收：正式编排不发布 8080/8443/9443。
INSTANCES = {
    "edgepulse-1": {"http": 19081, "ops": 19101, "agent": 19443},
    "edgepulse-2": {"http": 19082, "ops": 19102, "agent": 19444},
}
NGINX_HTTP = 18080
NGINX_HTTPS = 18443
NGINX_AGENT = 19445
PROMETHEUS_PORT = 19090
LEASE_TTL_SECONDS = 15
LEADER_TAKEOVER_LIMIT = 2 * LEASE_TTL_SECONDS

failures: list[str] = []
passes = 0
known_secrets: set[str] = set()


def remember_secret(value: str) -> None:
    if value:
        known_secrets.add(value)


def redact(text: str) -> str:
    """把脚本持有的口令与令牌从输出里抹掉。"""
    for secret in sorted(known_secrets, key=len, reverse=True):
        if len(secret) >= 8:
            text = text.replace(secret, "***")
    return text


def report(name: str, ok: bool, detail: str = "") -> None:
    global passes
    print(("PASS " if ok else "FAIL ") + name + (("：" + redact(detail)) if detail else ""))
    if ok:
        passes += 1
    else:
        failures.append(name)


def skip(name: str, detail: str) -> None:
    print("SKIP " + name + "：" + redact(detail))


def remove_ca_key() -> None:
    """验收用的临时 CA 私钥副本不留在工作树里；Windows 上 0400 是只读属性，先恢复可写。"""
    try:
        if CA_KEY.exists():
            os.chmod(CA_KEY, 0o600)
        CA_KEY.unlink()
    except OSError:
        pass


atexit.register(remove_ca_key)


def docker_blocker() -> str:
    if shutil.which("docker") is None:
        return "找不到 docker 命令"
    probes = (["docker", "version", "--format", "{{.Server.Version}}"], ["docker", "compose", "version"])
    for command in probes:
        try:
            result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30, check=False)
        except (OSError, subprocess.SubprocessError) as failure:
            return f"{' '.join(command)} 无法执行：{failure}"
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            return f"{' '.join(command)} 失败：{detail}"
    return ""


def compose(*arguments: str, check: bool = True, timeout: float = 600.0) -> subprocess.CompletedProcess[str]:
    command = [
        "docker", "compose", "-p", PROJECT,
        "--project-directory", str(COMPOSE_DIR),
        "--env-file", str(ENV_FILE),
        "-f", str(BASE_COMPOSE),
        "-f", str(OVERRIDE),
        *arguments,
    ]
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout, check=check)


def compose_base_config() -> subprocess.CompletedProcess[str]:
    command = [
        "docker", "compose", "-p", PROJECT,
        "--project-directory", str(COMPOSE_DIR),
        "--env-file", str(ENV_FILE),
        "-f", str(BASE_COMPOSE),
        "config", "-q",
    ]
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60, check=False)


def free_port(port: int) -> bool:
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        if os.name != "nt":
            # 服务停止后的 TIME_WAIT 不该被当成仍有监听者；Windows 上 SO_REUSEADDR 会掩盖真实占用
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind((HOST, port))
        return True
    except OSError:
        return False
    finally:
        probe.close()


def patch_line(text: str, pattern: str, replacement: str, label: str) -> str:
    updated, count = re.subn(pattern, replacement, text, flags=re.MULTILINE)
    if count != 1:
        raise RuntimeError(f"config/prod.yaml.example 中 {label} 的匹配次数为 {count}，无法生成验收配置")
    return updated


def prepare() -> dict[str, str]:
    GATE.mkdir(parents=True, exist_ok=True)
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    FIRMWARE_KEYS.mkdir(parents=True, exist_ok=True)

    required_certificates = ("ca.pem", "ca-key.pem", "server.pem", "server-key.pem", "agent.pem", "agent-key.pem")
    if not all((CERTS / name).exists() for name in required_certificates):
        if shutil.which("bash") is None:
            raise RuntimeError("缺少开发证书，且找不到 bash 来运行 scripts/dev-certs.sh")
        result = subprocess.run(["bash", "scripts/dev-certs.sh"], cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace", check=False)
        missing = [name for name in required_certificates if not (CERTS / name).exists()]
        if result.returncode != 0 or missing:
            raise RuntimeError("生成开发证书失败：" + ((result.stderr or result.stdout).strip()[-400:] or "产物缺失 " + ", ".join(missing)))

    source = (ROOT / "config" / "prod.yaml.example").read_text(encoding="utf-8")
    source = patch_line(source, r"^(\s*)https_port:\s*8443\s*$", r"\g<1>https_port: 0", "https_port")
    source = patch_line(source, r"^(\s*)expose_code:\s*false\s*$", r"\g<1>expose_code: true", "captcha.expose_code")
    source = patch_line(source, r"^(\s*)require_signature:\s*true\s*$", r"\g<1>require_signature: false", "firmware.require_signature")
    source = patch_line(source, r"^(\s*)allowed_origins:.*$",
                        r"\g<1>allowed_origins: http://127.0.0.1:%d,https://127.0.0.1:%d" % (NGINX_HTTP, NGINX_HTTPS),
                        "security.allowed_origins")
    (CONFIG_DIR / "prod.yaml").write_text(source, encoding="utf-8")
    logging_config = ROOT / "config" / "logging.yaml"
    if logging_config.exists():
        shutil.copyfile(logging_config, CONFIG_DIR / "logging.yaml")

    # CA 私钥单独放置并收紧权限；容器内 agent_ca 默认关闭，这里仍验证挂载与 0400 文件可读。
    # Windows 上 0400 是只读属性，重复运行前先恢复可写，否则复制会失败。
    if CA_KEY.exists():
        try:
            os.chmod(CA_KEY, 0o600)
        except OSError:
            pass
    shutil.copyfile(CERTS / "ca-key.pem", CA_KEY)
    os.chmod(CA_KEY, 0o400)

    values = {
        "MYSQL_ROOT_PASSWORD": secrets.token_hex(16),
        "EDGEPULSE_DB_PASSWORD": secrets.token_hex(16),
        "EDGEPULSE_REDIS_PASSWORD": secrets.token_hex(16),
        "EDGEPULSE_OPS_BEARER_TOKEN": secrets.token_hex(32),
        "EDGEPULSE_AGENT_FINGERPRINT": "",
        "EDGEPULSE_INITIAL_PASSWORD": "",
    }
    for secret in values.values():
        remember_secret(secret)
    (ROOT / "deploy" / "prometheus" / "ops-token").write_text(values["EDGEPULSE_OPS_BEARER_TOKEN"] + "\n", encoding="utf-8")

    env_lines = [f"{key}={value}" for key, value in values.items()]
    env_lines += [
        f"EDGEPULSE_HA_IMAGE={IMAGE}",
        f"EDGEPULSE_HA_CONFIG_DIR={CONFIG_DIR.as_posix()}",
        f"EDGEPULSE_HA_CERT_DIR={CERTS.as_posix()}",
        f"EDGEPULSE_HA_FIRMWARE_KEYS_DIR={FIRMWARE_KEYS.as_posix()}",
        f"EDGEPULSE_AGENT_CA_KEY_FILE={CA_KEY.as_posix()}",
        f"EDGEPULSE_HA_BIND_ADDRESS={HOST}",
        f"EDGEPULSE_HA_HTTP_PORT={NGINX_HTTP}",
        f"EDGEPULSE_HA_HTTPS_PORT={NGINX_HTTPS}",
        f"EDGEPULSE_HA_AGENT_PORT={NGINX_AGENT}",
        f"EDGEPULSE_HA_PROMETHEUS_PORT={PROMETHEUS_PORT}",
    ]
    ENV_FILE.write_text("\n".join(env_lines) + "\n", encoding="utf-8")

    override_lines = ["services:"]
    for instance, ports in INSTANCES.items():
        override_lines += [
            f"  {instance}:",
            "    ports:",
            f"      - \"{HOST}:{ports['http']}:8080\"",
            f"      - \"{HOST}:{ports['ops']}:9100\"",
            f"      - \"{HOST}:{ports['agent']}:9443\"",
        ]
    OVERRIDE.write_text("\n".join(override_lines) + "\n", encoding="utf-8")
    return values


def request_json(method: str, url: str, payload: dict | None = None, token: str | None = None,
                 context: ssl.SSLContext | None = None, timeout: float = 15.0) -> tuple[int, dict]:
    parts = urllib.parse.urlsplit(url)
    target = parts.path or "/"
    if parts.query:
        target += "?" + parts.query
    if parts.scheme == "https":
        connection = http.client.HTTPSConnection(parts.hostname, parts.port, context=context, timeout=timeout)
    else:
        connection = http.client.HTTPConnection(parts.hostname, parts.port, timeout=timeout)
    body = json.dumps(payload).encode() if payload is not None else None
    headers = {"Content-Type": "application/json"} if body is not None else {}
    if token:
        headers["Authorization"] = "Bearer " + token
    try:
        connection.request(method, target, body=body, headers=headers)
        response = connection.getresponse()
        raw = response.read().decode("utf-8", "replace")
        try:
            parsed = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            parsed = {}
        return response.status, parsed
    finally:
        connection.close()


def readyz(port: int) -> tuple[int, dict]:
    return request_json("GET", f"http://{HOST}:{port}/readyz", timeout=2.0)


def wait_until(probe, timeout: float, interval: float = 0.5):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = probe()
        except (OSError, http.client.HTTPException, ssl.SSLError):
            last = None
        if last:
            return last
        time.sleep(interval)
    return last


def wait_ready(port: int, timeout: float = 180.0) -> bool:
    def probe() -> bool:
        status, body = readyz(port)
        return status == 200 and body.get("status") == "ready"
    return bool(wait_until(probe, timeout, 1.0))


def login(port: int) -> str | None:
    status, captcha = request_json("GET", f"http://{HOST}:{port}/api/auth/captcha")
    data = captcha.get("data") or {}
    code = data.get("code")
    report("验证码签发（实例 A）", status == 200 and bool(code), f"HTTP {status}")
    if not code:
        return None
    status, payload = request_json(
        "POST", f"http://{HOST}:{port}/api/auth/login",
        {"username": "admin", "password": "admin123", "captcha": code, "captchaUuid": data.get("uuid")},
    )
    token = (payload.get("data") or {}).get("token", "")
    remember_secret(token)
    report("管理员登录（实例 A）", status == 200 and len(token) == 64, f"HTTP {status}")
    return token or None


def lease_state() -> tuple[str, int] | None:
    sql = "SELECT holder, fence FROM sys_lease WHERE name='control-plane'"
    command = 'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql -uroot -N -B edgepulse -e "' + sql + '"'
    try:
        result = compose("exec", "-T", "mysql", "sh", "-c", command, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode != 0:
        return None
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        return None
    fields = lines[0].split("\t")
    if len(fields) != 2:
        fields = lines[0].split()
    if len(fields) != 2 or not fields[1].isdigit():
        return None
    return fields[0], int(fields[1])


def wait_leader(timeout: float = 120.0) -> tuple[str, int] | None:
    def probe():
        state = lease_state()
        if state is not None and state[0] in INSTANCES:
            return state
        return None
    return wait_until(probe, timeout, 1.0)


class WebSocketConnection:
    """最小 RFC 6455 客户端：只需握手、文本帧与 ping/pong，避免引入第三方依赖。"""

    def __init__(self, port: int, context: ssl.SSLContext):
        raw = socket.create_connection((HOST, port), timeout=15.0)
        self.socket = context.wrap_socket(raw, server_hostname=HOST)
        self.buffer = b""
        key = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
        request = (
            "GET /api/agent/ws HTTP/1.1\r\n"
            f"Host: {HOST}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            f"Origin: https://{HOST}:{NGINX_HTTPS}\r\n"
            "\r\n"
        )
        self.socket.sendall(request.encode("ascii"))
        header = self._read_until(b"\r\n\r\n")
        status_line = header.split(b"\r\n", 1)[0].decode("latin-1")
        if " 101 " not in status_line:
            raise RuntimeError("Agent WebSocket 握手失败：" + status_line)
        self.buffer = header.split(b"\r\n\r\n", 1)[1]

    def _read_until(self, marker: bytes) -> bytes:
        while marker not in self.buffer:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise RuntimeError("Agent WebSocket 在握手阶段断开")
            self.buffer += chunk
        head, self.buffer = self.buffer.split(marker, 1)
        return head + marker

    def _read_exact(self, count: int) -> bytes:
        while len(self.buffer) < count:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise RuntimeError("Agent WebSocket 连接已关闭")
            self.buffer += chunk
        data, self.buffer = self.buffer[:count], self.buffer[count:]
        return data

    def send_text(self, text: str) -> None:
        payload = text.encode("utf-8")
        header = bytearray([0x81])
        length = len(payload)
        if length < 126:
            header.append(0x80 | length)
        elif length < 65536:
            header.append(0x80 | 126)
            header += length.to_bytes(2, "big")
        else:
            header.append(0x80 | 127)
            header += length.to_bytes(8, "big")
        mask = secrets.token_bytes(4)
        header += mask
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        self.socket.sendall(bytes(header) + masked)

    def _send_control(self, opcode: int, payload: bytes = b"") -> None:
        mask = secrets.token_bytes(4)
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        self.socket.sendall(bytes([0x80 | opcode, 0x80 | len(payload)]) + mask + masked)

    def receive(self, timeout: float) -> tuple[int, bytes]:
        self.socket.settimeout(timeout)
        first, second = self._read_exact(2)
        opcode = first & 0x0F
        masked = bool(second & 0x80)
        length = second & 0x7F
        if length == 126:
            length = int.from_bytes(self._read_exact(2), "big")
        elif length == 127:
            length = int.from_bytes(self._read_exact(8), "big")
        mask = self._read_exact(4) if masked else b""
        payload = self._read_exact(length)
        if masked:
            payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        return opcode, payload

    def wait_for_text(self, timeout: float) -> str | None:
        deadline = time.time() + timeout
        while time.time() < deadline:
            remaining = max(0.1, deadline - time.time())
            try:
                opcode, payload = self.receive(remaining)
            except socket.timeout:
                continue
            if opcode == 0x1:
                return payload.decode("utf-8", "replace")
            if opcode == 0x8:
                return None
            if opcode == 0x9:
                self._send_control(0xA, payload)
        return None

    def close(self) -> None:
        try:
            self._send_control(0x8)
        except OSError:
            pass
        try:
            self.socket.close()
        except OSError:
            pass


def inspect_exit_code(service: str) -> str:
    container = compose("ps", "-aq", service, timeout=30).stdout.strip().splitlines()
    if not container:
        return "missing"
    result = subprocess.run(["docker", "inspect", "-f", "{{.State.ExitCode}}", container[0]], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=30, check=False)
    return result.stdout.strip() if result.returncode == 0 else "inspect-failed"


def main() -> int:
    parser = argparse.ArgumentParser(description="EdgePulse 双实例 HA 验收（Docker Compose）。")
    parser.add_argument("--build", action="store_true", help="先按仓库 Dockerfile 构建 edgepulse:ha")
    arguments = parser.parse_args()

    blocker = docker_blocker()
    if blocker:
        skip("HA 双实例验收", f"{blocker}；安装并启动 Docker Engine 与 Compose v2 后重跑")
        return 0

    busy = [str(port) for port in [item for ports in INSTANCES.values() for item in ports.values()] + [NGINX_HTTP, NGINX_HTTPS, NGINX_AGENT, PROMETHEUS_PORT]
            if not free_port(port)]
    if busy:
        report("验收端口空闲", False, "被占用：" + ", ".join(busy))
        return 1
    report("验收端口空闲", True)

    try:
        values = prepare()
    except Exception as failure:  # noqa: BLE001
        report("准备验收配置与证书", False, f"{type(failure).__name__}: {failure}")
        return 1
    report("准备验收配置与证书", True, str(GATE))

    if arguments.build:
        command = ["docker", "build", "-f", "deploy/docker/Dockerfile", "-t", IMAGE]
        node_image = os.environ.get("EDGEPULSE_DOCKER_NODE_IMAGE")
        apt_mirror = os.environ.get("EDGEPULSE_DOCKER_APT_MIRROR")
        if node_image:
            command += ["--build-arg", f"NODE_IMAGE={node_image}"]
        if apt_mirror:
            command += ["--build-arg", f"APT_MIRROR={apt_mirror}"]
        command.append(".")
        result = subprocess.run(command, cwd=ROOT, check=False)
        report("构建 edgepulse:ha 镜像", result.returncode == 0)
        if result.returncode != 0:
            return 1
    else:
        result = subprocess.run(["docker", "image", "inspect", IMAGE], capture_output=True, check=False)
        report("复用已构建的 edgepulse:ha 镜像", result.returncode == 0, "缺失时请加 --build")
        if result.returncode != 0:
            return 1

    base_config = compose_base_config()
    report("HA compose 基础配置可解析", base_config.returncode == 0, (base_config.stderr or "").strip())
    try:
        config = compose("config", "-q", timeout=60)
    except (OSError, subprocess.SubprocessError) as failure:
        report("HA compose 测试覆盖可解析", False, str(failure))
        return 1
    report("HA compose 测试覆盖可解析", config.returncode == 0, (config.stderr or "").strip())
    if base_config.returncode != 0 or config.returncode != 0:
        return 1

    started = False
    try:
        try:
            up = compose("up", "-d", "--no-build", "mysql", "redis", "edgepulse-1", "edgepulse-2", "nginx", timeout=600)
        except subprocess.TimeoutExpired:
            report("compose 启动 HA 栈", False, "600 秒内未完成启动")
            return 1
        report("compose 启动 HA 栈", up.returncode == 0, (up.stderr or up.stdout).strip()[-800:])
        if up.returncode != 0:
            return 1
        started = True

        ready_a = wait_ready(INSTANCES["edgepulse-1"]["ops"])
        ready_b = wait_ready(INSTANCES["edgepulse-2"]["ops"])
        report("实例 A /readyz 就绪", ready_a)
        report("实例 B /readyz 就绪", ready_b)
        if not (ready_a and ready_b):
            return 1

        logs_a = compose("logs", "--no-color", "edgepulse-1", timeout=60).stdout
        logs_b = compose("logs", "--no-color", "edgepulse-2", timeout=60).stdout
        report("实例 A 使用显式 instance_id", "instance=edgepulse-1" in logs_a)
        report("实例 B 使用显式 instance_id", "instance=edgepulse-2" in logs_b)
        if "instance=edgepulse-1" not in logs_a or "instance=edgepulse-2" not in logs_b:
            return 1

        token = login(INSTANCES["edgepulse-1"]["http"])
        if token is None:
            return 1
        status, me = request_json("GET", f"http://{HOST}:{INSTANCES['edgepulse-2']['http']}/api/auth/me", token=token)
        report("实例 A 登录的令牌在实例 B 可用", status == 200 and (me.get("data") or {}).get("username") == "admin", f"HTTP {status}")

        redis_password = values["EDGEPULSE_REDIS_PASSWORD"]
        try:
            stored = compose("exec", "-T", "redis", "redis-cli", "--no-auth-warning", "-a", redis_password, "EXISTS", "edgepulse:session:" + token, timeout=30)
        except (OSError, subprocess.SubprocessError) as failure:
            report("Redis 保存共享会话键", False, str(failure))
        else:
            report("Redis 保存共享会话键", stored.returncode == 0 and stored.stdout.strip() == "1", f"EXISTS={stored.stdout.strip() or '无输出'}")

        # 用开发 Agent 证书注册一个真实节点，再用同一证书建 WS；节点身份由 mTLS 指纹核对。
        status, enrollment = request_json("POST", f"http://{HOST}:{INSTANCES['edgepulse-1']['http']}/api/enrollment-tokens",
                                         {"action": "enroll", "expiresInSeconds": 3600, "nodeName": "ha-check-agent"}, token=token)
        enrollment_data = enrollment.get("data") or {}
        enrollment_token = str(enrollment_data.get("token", ""))
        remember_secret(enrollment_token)
        report("创建 Agent 注册令牌", status == 200 and enrollment_token.startswith("ep_enroll_"), f"HTTP {status}")
        if not enrollment_token:
            return 1

        fingerprint = hashlib.sha256(ssl.PEM_cert_to_DER_cert((CERTS / "agent.pem").read_text(encoding="ascii"))).hexdigest()
        client_context = ssl.create_default_context(cafile=str(CERTS / "ca.pem"))
        client_context.load_cert_chain(certfile=str(CERTS / "agent.pem"), keyfile=str(CERTS / "agent-key.pem"))
        status, registration = request_json(
            "POST", f"https://{HOST}:{INSTANCES['edgepulse-1']['agent']}/api/agent/register",
            {"name": "ha-check-agent", "agentVersion": "ha-check", "certFingerprint": fingerprint, "tenantCode": "default",
             "labels": "{}", "enrollmentToken": enrollment_token},
            context=client_context,
        )
        node_id = int((registration.get("data") or {}).get("id") or 0)
        report("Agent 经实例 A 的 9443 注册", status == 200 and node_id > 0, f"HTTP {status}，nodeId={node_id}")
        if node_id <= 0:
            return 1

        websocket = WebSocketConnection(INSTANCES["edgepulse-1"]["agent"], client_context)
        try:
            websocket.send_text(json.dumps({"type": "hello", "nodeId": node_id, "agentVersion": "ha-check"}))
            hello = websocket.wait_for_text(10.0)
            hello_ok = False
            if hello:
                try:
                    hello_ok = json.loads(hello).get("type") == "hello" and bool(json.loads(hello).get("ok"))
                except json.JSONDecodeError:
                    hello_ok = False
            report("测试 Agent WebSocket hello（实例 A）", hello_ok, hello or "10 秒内没有应答")
            if not hello_ok:
                return 1

            issued_at = time.monotonic()
            status, created = request_json(
                "POST", f"http://{HOST}:{INSTANCES['edgepulse-2']['http']}/api/commands",
                {"nodeId": node_id, "type": "diagnostics.collect", "payload": "{}", "timeoutSeconds": 60}, token=token,
            )
            command_id = int((created.get("data") or {}).get("id") or 0)
            report("在实例 B 创建 Agent 命令", status == 200 and command_id > 0, f"HTTP {status}，commandId={command_id}")
            if command_id <= 0:
                return 1

            delivered = False
            payload_text = ""
            deadline = time.monotonic() + 10.0
            while time.monotonic() < deadline:
                payload_text = websocket.wait_for_text(max(0.1, deadline - time.monotonic())) or ""
                if not payload_text:
                    continue
                try:
                    message = json.loads(payload_text)
                except json.JSONDecodeError:
                    continue
                command = message.get("command") or {}
                if message.get("type") == "command" and int(command.get("id") or 0) == command_id:
                    delivered = True
                    break
            elapsed = time.monotonic() - issued_at
            report("实例 B 的命令经 Redis 送达实例 A 的 Agent", delivered, f"{elapsed:.2f} 秒，commandId={command_id}")
            if not delivered:
                return 1
        finally:
            websocket.close()

        leader_state = wait_leader(30.0)
        report("sys_lease 选出 leader", leader_state is not None, f"holder={leader_state[0] if leader_state else '无'}，fence={leader_state[1] if leader_state else 0}")
        if leader_state is None:
            return 1
        leader, leader_fence = leader_state
        follower = "edgepulse-2" if leader == "edgepulse-1" else "edgepulse-1"
        leader_ports = INSTANCES[leader]

        # 用未完成的固件上传占住 HTTP 排空窗口：SIGTERM 后 /readyz 先变 503，ops 监听最后才关。
        stop_process: subprocess.Popen | None = None
        stop_started = 0.0
        upload = socket.create_connection((HOST, leader_ports["http"]), timeout=5.0)
        try:
            upload.sendall(
                (
                    f"POST /api/firmware?name=ha-check&version=1.0.0&fileName=ha-check.bin HTTP/1.1\r\n"
                    f"Host: {HOST}:{leader_ports['http']}\r\n"
                    f"Authorization: Bearer {token}\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "Content-Length: 1048576\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n"
                ).encode("ascii") + b"x" * 1024
            )
            time.sleep(0.5)
            upload.sendall(b"x" * 1024)
            # 未完成的正文应让处理协程保持忙碌；如果服务端已经拒绝并回应，排空窗口就不存在了
            upload.settimeout(0.2)
            try:
                early = upload.recv(1)
            except socket.timeout:
                early = None
            upload.settimeout(5.0)
            if early is not None:
                raise RuntimeError("固件上传在收到完整正文前已被处理，无法构造排空窗口")
            report("构造停机中的在途上传", True, f"leader={leader}")

            stop_started = time.monotonic()
            stop_process = subprocess.Popen(
                ["docker", "compose", "-p", PROJECT, "--project-directory", str(COMPOSE_DIR), "--env-file", str(ENV_FILE),
                 "-f", str(BASE_COMPOSE), "-f", str(OVERRIDE), "stop", "-t", "40", leader],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
            )

            saw_503 = False
            deadline = time.monotonic() + 10.0
            while time.monotonic() < deadline:
                try:
                    status, body = readyz(leader_ports["ops"])
                except (OSError, http.client.HTTPException, ssl.SSLError):
                    status, body = 0, {}
                if status == 503:
                    saw_503 = True
                    break
                time.sleep(0.1)
            report("停机中的实例 /readyz 返回 503", saw_503, "已观察到 503" if saw_503 else "10 秒内未观察到 503")
        finally:
            try:
                upload.close()
            except OSError:
                pass

        if stop_process is None:
            report("SIGTERM 优雅停机", False, "未能发起停止命令")
            return 1
        try:
            stdout, stderr = stop_process.communicate(timeout=90)
        except subprocess.TimeoutExpired:
            stop_process.kill()
            stdout, stderr = stop_process.communicate()
            report("SIGTERM 优雅停机", False, "stop 命令 90 秒未返回")
        else:
            exit_code = inspect_exit_code(leader)
            report("SIGTERM 优雅停机", stop_process.returncode == 0 and exit_code == "0",
                   f"stop rc={stop_process.returncode} exit={exit_code} {(stderr or stdout).strip()[-200:]}")

        takeover = wait_until(
            lambda: (lambda state: state if state is not None and state[0] == follower and state[1] > leader_fence else None)(lease_state()),
            LEADER_TAKEOVER_LIMIT + 10.0, 0.5,
        )
        elapsed = time.monotonic() - stop_started
        report("leader 停止后新 leader 在 2 个 TTL 内接管",
               takeover is not None and elapsed <= LEADER_TAKEOVER_LIMIT,
               f"{elapsed:.2f} 秒，holder={takeover[0] if takeover else '无'}，fence={takeover[1] if takeover else 0}")
        report("新 leader 实例 /readyz 就绪", wait_ready(INSTANCES[follower]["ops"], 60.0))
        status, _ = request_json("GET", f"http://{HOST}:{INSTANCES[follower]['http']}/api/auth/me", token=token)
        report("故障切换后共享会话仍有效", status == 200, f"HTTP {status}")

        nginx_ok = False
        deadline = time.time() + 20.0
        while time.time() < deadline:
            try:
                status, _ = request_json("GET", f"http://{HOST}:{NGINX_HTTP}/api/health", timeout=3.0)
            except (OSError, http.client.HTTPException):
                status = 0
            if status == 200:
                nginx_ok = True
                break
            time.sleep(0.5)
        report("nginx 8080 在 leader 停机后仍有可用上游", nginx_ok, f"HTTP {status}")

        try:
            tls_context = ssl.create_default_context(cafile=str(CERTS / "ca.pem"))
            status, _ = request_json("GET", f"https://{HOST}:{NGINX_HTTPS}/api/health", context=tls_context, timeout=5.0)
            report("nginx 8443 TLS 终止可用", status == 200, f"HTTP {status}")
        except (OSError, ssl.SSLError, http.client.HTTPException) as failure:
            report("nginx 8443 TLS 终止可用", False, str(failure))

        # 重启旧 leader：它只能作为 follower 就绪，不能覆盖仍在续约的 holder。
        compose("start", leader, timeout=120)
        restarted_ready = wait_ready(leader_ports["ops"], 120.0)
        report("旧 leader 重启后就绪", restarted_ready)
        time.sleep(3.0)
        after_restart = lease_state()
        report("旧 leader 重启后不抢占有效租约",
               after_restart is not None and after_restart[0] == follower and after_restart[1] == (takeover[1] if takeover else -1),
               f"holder={after_restart[0] if after_restart else '无'}，fence={after_restart[1] if after_restart else 0}")
    except Exception as failure:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        report("HA 验收执行未抛出异常", False, f"{type(failure).__name__}: {failure}")
    finally:
        if started:
            logs = compose("logs", "--no-color", check=False, timeout=60)
            LOG_PATH.write_text(redact(logs.stdout + logs.stderr), encoding="utf-8")
            down = compose("down", "-v", "--remove-orphans", check=False, timeout=120)
            report("清理 HA 容器、网络与卷", down.returncode == 0, (down.stderr or "").strip()[-300:])
        remove_ca_key()

    print()
    if failures:
        print(f"HA 双实例验收失败：{len(failures)} 项 -> " + ", ".join(failures))
        return 1
    print(f"HA 双实例验收全部通过（{passes} 项）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
