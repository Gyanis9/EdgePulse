#!/usr/bin/env python3
"""EdgePulse 双实例 HA 验收：Compose 启栈、跨实例会话、Agent 命令路由、选主与两种停机。

默认复用已构建的 edgepulse:ha 镜像；加 --build 先按仓库 Dockerfile 构建。
测试专用配置把验证码设为可见、关闭固件签名并启用 Agent 内部 CA，只存在 build/gate/ha，
不进入交付镜像。Docker 或 Compose 不可用时打印 SKIP 并以 0 退出；加 --require-docker
则改为失败。栈一旦启动，任何断言失败都以 1 退出。
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
GATE_CERTS = GATE / "certs"
APP_CERTS = GATE_CERTS / "app"
NGINX_CERTS = GATE_CERTS / "nginx"
CA_KEY = GATE / "secrets" / "agent-ca-key.pem"
OPS_TOKEN = GATE / "ops-token"
LOG_PATH = GATE / "ha-stack.log"
APP_LOG = "/app/var/logs/ha-check.log"
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


def remove_gate_secrets() -> None:
    """验收生成的私钥、口令文件不留在工作树；Windows 上只读属性先恢复可写再删。"""
    for path in (CA_KEY, APP_CERTS / "server-key.pem", NGINX_CERTS / "server-key.pem", ENV_FILE, OPS_TOKEN):
        try:
            if path.exists():
                os.chmod(path, 0o600)
            path.unlink()
        except OSError:
            pass


atexit.register(remove_gate_secrets)


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


def compose(*arguments: str, check: bool = True, timeout: float = 600.0, input_text: str | None = None) -> subprocess.CompletedProcess[str]:
    command = [
        "docker", "compose", "-p", PROJECT,
        "--project-directory", str(COMPOSE_DIR),
        "--env-file", str(ENV_FILE),
        # 验收配置启用 agent_ca；ca-key-init 只在 agent-ca profile 下启动
        "--profile", "agent-ca",
        "-f", str(BASE_COMPOSE),
        "-f", str(OVERRIDE),
        *arguments,
    ]
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout, check=check, input=input_text)


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


def patch_section_line(text: str, section: str, key: str, old: str, new: str, label: str) -> str:
    """只替换顶层 section 内的一行，避免误改其它段里同名的 enabled。"""
    lines = text.splitlines(keepends=True)
    in_section = False
    replaced = 0
    for index, line in enumerate(lines):
        stripped = line.rstrip("\r\n")
        if not in_section:
            if re.match(rf"^  {re.escape(section)}:\s*$", stripped):
                in_section = True
            continue
        # edgepulse 下的子段都缩进两格；遇到下一个子段或顶层键就结束
        if re.match(r"^  \S", stripped) or re.match(r"^\S", stripped):
            break
        match = re.match(rf"^(\s+){re.escape(key)}:\s*{re.escape(old)}\s*$", stripped)
        if match:
            lines[index] = f"{match.group(1)}{key}: {new}\n"
            replaced += 1
    if replaced != 1:
        raise RuntimeError(f"config/prod.yaml.example 中 {label} 的匹配次数为 {replaced}，无法生成验收配置")
    return "".join(lines)


def prepare() -> dict[str, str]:
    GATE.mkdir(parents=True, exist_ok=True)
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    FIRMWARE_KEYS.mkdir(parents=True, exist_ok=True)
    APP_CERTS.mkdir(parents=True, exist_ok=True)
    NGINX_CERTS.mkdir(parents=True, exist_ok=True)
    CA_KEY.parent.mkdir(parents=True, exist_ok=True)

    required_certificates = ("ca.pem", "ca-key.pem", "server.pem", "server-key.pem", "agent.pem", "agent-key.pem")
    if not all((CERTS / name).exists() for name in required_certificates):
        if shutil.which("bash") is None:
            raise RuntimeError("缺少开发证书，且找不到 bash 来运行 scripts/dev-certs.sh")
        result = subprocess.run(["bash", "scripts/dev-certs.sh"], cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace", check=False)
        missing = [name for name in required_certificates if not (CERTS / name).exists()]
        if result.returncode != 0 or missing:
            raise RuntimeError("生成开发证书失败：" + ((result.stderr or result.stdout).strip()[-400:] or "产物缺失 " + ", ".join(missing)))

    # 应用只拿 ca.pem/server.pem/server-key.pem，nginx 只拿 server.pem/server-key.pem。
    # 旧版本把整个 var/certs 挂给两边，这里先清掉可能残留的 CA 私钥与 ca.pem。
    for stale in (APP_CERTS / "ca-key.pem", NGINX_CERTS / "ca-key.pem", NGINX_CERTS / "ca.pem"):
        stale.unlink(missing_ok=True)
    for name in ("ca.pem", "server.pem", "server-key.pem"):
        shutil.copyfile(CERTS / name, APP_CERTS / name)
    for name in ("server.pem", "server-key.pem"):
        shutil.copyfile(CERTS / name, NGINX_CERTS / name)

    source = (ROOT / "config" / "prod.yaml.example").read_text(encoding="utf-8")
    source = patch_line(source, r"^(\s*)https_port:\s*8443\s*$", r"\g<1>https_port: 0", "https_port")
    source = patch_line(source, r"^(\s*)expose_code:\s*false\s*$", r"\g<1>expose_code: true", "captcha.expose_code")
    source = patch_line(source, r"^(\s*)require_signature:\s*true\s*$", r"\g<1>require_signature: false", "firmware.require_signature")
    source = patch_line(source, r"^(\s*)allowed_origins:.*$",
                        r"\g<1>allowed_origins: http://127.0.0.1:%d,https://127.0.0.1:%d" % (NGINX_HTTP, NGINX_HTTPS),
                        "security.allowed_origins")
    source = patch_section_line(source, "agent_ca", "enabled", "false", "true", "agent_ca.enabled")
    (CONFIG_DIR / "prod.yaml").write_text(source, encoding="utf-8")

    # 入口脚本会把 logging.yaml 复制到运行时配置目录；改用独有文件名，能区分
    # 「配置被载入」与「内置日志器碰巧也写了文件」。
    logging_source = (ROOT / "config" / "logging.yaml")
    if not logging_source.exists():
        raise RuntimeError("缺少 config/logging.yaml，无法验证日志 sink")
    logging_text = logging_source.read_text(encoding="utf-8")
    logging_text = logging_text.replace("base_filename: edgepulse.log", "base_filename: ha-check.log")
    if "ha-check.log" not in logging_text or "formatter: json" not in logging_text:
        raise RuntimeError("config/logging.yaml 的滚动文件或 JSON 版式已变化，无法生成验收日志配置")
    (CONFIG_DIR / "logging.yaml").write_text(logging_text, encoding="utf-8")

    # CA 私钥单独放置并收紧权限；ca-key-init 会以 root 读取并复制成应用用户的 0400 文件。
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

    # 运维令牌只写 build/gate，不覆盖 deploy/prometheus/ops-token；Prometheus 由测试覆盖
    # 单独挂载这个文件，profile 打开时也能用。
    OPS_TOKEN.write_text(values["EDGEPULSE_OPS_BEARER_TOKEN"] + "\n", encoding="utf-8")

    env_lines = [f"{key}={value}" for key, value in values.items()]
    env_lines += [
        f"EDGEPULSE_HA_IMAGE={IMAGE}",
        f"EDGEPULSE_HA_CONFIG_DIR={CONFIG_DIR.as_posix()}",
        f"EDGEPULSE_HA_CERT_DIR={APP_CERTS.as_posix()}",
        f"EDGEPULSE_HA_NGINX_CERT_DIR={NGINX_CERTS.as_posix()}",
        f"EDGEPULSE_HA_FIRMWARE_KEYS_DIR={FIRMWARE_KEYS.as_posix()}",
        f"EDGEPULSE_AGENT_CA_KEY_FILE={CA_KEY.as_posix()}",
        f"EDGEPULSE_HA_HTTP_BIND_ADDRESS={HOST}",
        f"EDGEPULSE_HA_HTTPS_BIND_ADDRESS={HOST}",
        f"EDGEPULSE_HA_AGENT_BIND_ADDRESS={HOST}",
        f"EDGEPULSE_HA_HTTP_PORT={NGINX_HTTP}",
        f"EDGEPULSE_HA_HTTPS_PORT={NGINX_HTTPS}",
        f"EDGEPULSE_HA_AGENT_PORT={NGINX_AGENT}",
    ]
    ENV_FILE.write_text("\n".join(env_lines) + "\n", encoding="utf-8")

    override_lines = ["services:"]
    for instance, ports in INSTANCES.items():
        override_lines += [
            f"  {instance}:",
            # 复用旧镜像时也验证工作树里的入口脚本；CI 的 --build 会把它烘进镜像。
            # 用 sh 显式执行，避免 Windows 绑定挂载丢掉可执行位。
            "    entrypoint: [\"/bin/sh\", \"/app/entrypoint.sh\"]",
            "    volumes:",
            "      - type: bind",
            f"        source: {(COMPOSE_DIR / 'entrypoint.sh').as_posix()}",
            "        target: /app/entrypoint.sh",
            "        read_only: true",
            "    ports:",
            f"      - \"{HOST}:{ports['http']}:8080\"",
            f"      - \"{HOST}:{ports['ops']}:9100\"",
            f"      - \"{HOST}:{ports['agent']}:9443\"",
        ]
    override_lines += [
        "  prometheus:",
        "    volumes:",
        "      - type: bind",
        f"        source: {OPS_TOKEN.as_posix()}",
        "        target: /etc/prometheus/ops-token",
        "        read_only: true",
    ]
    OVERRIDE.write_text("\n".join(override_lines) + "\n", encoding="utf-8")
    return values


def request_raw(method: str, url: str, data: bytes | None = None, token: str | None = None,
                context: ssl.SSLContext | None = None, timeout: float = 15.0,
                headers: dict[str, str] | None = None) -> tuple[int, dict[str, str], bytes]:
    parts = urllib.parse.urlsplit(url)
    target = parts.path or "/"
    if parts.query:
        target += "?" + parts.query
    if parts.scheme == "https":
        connection = http.client.HTTPSConnection(parts.hostname, parts.port, context=context, timeout=timeout)
    else:
        connection = http.client.HTTPConnection(parts.hostname, parts.port, timeout=timeout)
    request_headers = dict(headers or {})
    if token:
        request_headers["Authorization"] = "Bearer " + token
    try:
        connection.request(method, target, body=data, headers=request_headers)
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        connection.close()


def request_json(method: str, url: str, payload: dict | None = None, token: str | None = None,
                 context: ssl.SSLContext | None = None, timeout: float = 15.0) -> tuple[int, dict]:
    body = json.dumps(payload).encode() if payload is not None else None
    headers = {"Content-Type": "application/json"} if body is not None else None
    status, _, raw = request_raw(method, url, data=body, token=token, context=context, timeout=timeout, headers=headers)
    text = raw.decode("utf-8", "replace")
    try:
        parsed = json.loads(text) if text else {}
    except json.JSONDecodeError:
        parsed = {}
    return status, parsed


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


def wait_ready_status(port: int, expected: int, timeout: float = 60.0) -> bool:
    """等待 /readyz 返回指定状态；Redis 停机与恢复都靠它观察，不把连接错误算作命中。"""
    def probe() -> bool:
        try:
            status, _ = readyz(port)
        except (OSError, http.client.HTTPException, ssl.SSLError):
            return False
        return status == expected
    return bool(wait_until(probe, timeout, 1.0))


def sample_allowed_origins() -> tuple[bool, str]:
    """静态检查交付样例：HA 发布的 8443 端口必须出现在 allowed_origins 中。"""
    text = (ROOT / "config" / "prod.yaml.example").read_text(encoding="utf-8")
    match = re.search(r"^\s*allowed_origins:\s*(.+)$", text, flags=re.MULTILINE)
    if match is None:
        return False, "config/prod.yaml.example 缺少 security.allowed_origins"
    origins = [item.strip() for item in match.group(1).split(",") if item.strip()]
    return "https://console.example.com:8443" in origins, ",".join(origins)


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


def mysql_scalar(sql: str) -> str | None:
    command = 'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql -uroot -N -B edgepulse -e "' + sql + '"'
    try:
        result = compose("exec", "-T", "mysql", "sh", "-c", command, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode != 0:
        return None
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    return lines[0] if len(lines) == 1 else None


def lease_state() -> tuple[str, int] | None:
    row = mysql_scalar("SELECT holder, fence FROM sys_lease WHERE name='control-plane'")
    if row is None:
        return None
    fields = row.split("\t")
    if len(fields) != 2:
        fields = row.split()
    if len(fields) != 2 or not fields[1].isdigit():
        return None
    return fields[0], int(fields[1])


def lease_remaining_seconds() -> int | None:
    row = mysql_scalar("SELECT TIMESTAMPDIFF(SECOND, UTC_TIMESTAMP(), expires_at) FROM sys_lease WHERE name='control-plane'")
    if row is None or not re.fullmatch(r"-?\d+", row):
        return None
    return int(row)


def wait_leader(timeout: float = 120.0) -> tuple[str, int] | None:
    def probe():
        state = lease_state()
        if state is not None and state[0] in INSTANCES:
            return state
        return None
    return wait_until(probe, timeout, 1.0)


class WebSocketConnection:
    """最小 RFC 6455 客户端：只需握手、文本帧与 ping/pong，避免引入第三方依赖。"""

    def __init__(self, host: str, port: int, path: str, origin: str | None, context: ssl.SSLContext | None = None):
        raw = socket.create_connection((host, port), timeout=15.0)
        self.socket = context.wrap_socket(raw, server_hostname=host) if context is not None else raw
        self.buffer = b""
        key = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
        )
        if origin:
            request += f"Origin: {origin}\r\n"
        request += "\r\n"
        self.socket.sendall(request.encode("ascii"))
        header = self._read_until(b"\r\n\r\n")
        status_line = header.split(b"\r\n", 1)[0].decode("latin-1")
        if " 101 " not in status_line:
            raise RuntimeError("WebSocket 握手失败：" + status_line)
        self.buffer = header.split(b"\r\n\r\n", 1)[1]

    def _read_until(self, marker: bytes) -> bytes:
        while marker not in self.buffer:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise RuntimeError("WebSocket 在握手阶段断开")
            self.buffer += chunk
        head, self.buffer = self.buffer.split(marker, 1)
        return head + marker

    def _read_exact(self, count: int) -> bytes:
        while len(self.buffer) < count:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise RuntimeError("WebSocket 连接已关闭")
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

    def wait_for_close(self, timeout: float) -> tuple[int, str] | None:
        """等待服务端 Close 帧并返回状态码与原因；只用于断言 1008 这类策略关闭。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            remaining = max(0.1, deadline - time.time())
            try:
                opcode, payload = self.receive(remaining)
            except socket.timeout:
                continue
            if opcode == 0x8:
                code = int.from_bytes(payload[:2], "big") if len(payload) >= 2 else 1005
                return code, payload[2:].decode("utf-8", "replace")
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


def check_browser_websocket(port: int, token: str, context: ssl.SSLContext | None = None,
                            origin: str | None = None) -> tuple[bool, str]:
    connection: WebSocketConnection | None = None
    try:
        connection = WebSocketConnection(HOST, port, "/api/ws", origin or f"http://{HOST}:{port}", context)
        connection.send_text(json.dumps({"type": "auth", "token": token}))
        acknowledgement = connection.wait_for_text(10.0) or ""
        if not acknowledgement:
            return False, "10 秒内没有认证回执"
        try:
            parsed = json.loads(acknowledgement)
        except json.JSONDecodeError:
            return False, acknowledgement
        return parsed.get("type") == "auth" and bool(parsed.get("ok")), acknowledgement
    except (OSError, RuntimeError, ssl.SSLError, http.client.HTTPException) as failure:
        return False, str(failure)
    finally:
        if connection is not None:
            connection.close()


def check_ws_origin_rejected(port: int, origin: str) -> tuple[bool, str]:
    """未列入 allowed_origins 的浏览器 Origin 必须在升级前收到 403。"""
    connection: WebSocketConnection | None = None
    try:
        connection = WebSocketConnection(HOST, port, "/api/ws", origin)
        return False, "握手未被拒绝"
    except RuntimeError as failure:
        return " 403 " in str(failure), str(failure)
    except (OSError, ssl.SSLError, http.client.HTTPException) as failure:
        return False, str(failure)
    finally:
        if connection is not None:
            connection.close()


def sse_probe(port: int, token: str, context: ssl.SSLContext | None = None,
              timeout: float = 10.0) -> tuple[int, dict[str, str]]:
    """只读 SSE 响应头：服务端先发 ready 事件，getresponse 在收到头部后立即返回。"""
    if context is not None:
        connection = http.client.HTTPSConnection(HOST, port, context=context, timeout=timeout)
    else:
        connection = http.client.HTTPConnection(HOST, port, timeout=timeout)
    try:
        connection.request("GET", "/api/events/alarms", headers={
            "Authorization": "Bearer " + token,
            "Accept": "text/event-stream",
        })
        response = connection.getresponse()
        return response.status, {key.lower(): value for key, value in response.getheaders()}
    finally:
        connection.close()


def app_log(service: str) -> str:
    try:
        result = compose("exec", "-T", service, "sh", "-c", f"cat {APP_LOG} 2>/dev/null || true", timeout=30)
    except (OSError, subprocess.SubprocessError):
        return ""
    return result.stdout


def wait_app_log(service: str, needle: str, timeout: float = 30.0) -> bool:
    def probe() -> bool:
        return needle in app_log(service)
    return bool(wait_until(probe, timeout, 1.0))


def wait_json_log_record(service: str, timeout: float = 30.0) -> dict | None:
    def probe():
        lines = [line.strip() for line in app_log(service).splitlines() if line.strip()]
        if not lines:
            return None
        try:
            record = json.loads(lines[-1])
        except json.JSONDecodeError:
            return None
        return record if {"timestamp", "level", "message"} <= set(record) else None
    return wait_until(probe, timeout, 1.0)


def inspect_exit_code(service: str) -> str:
    container = compose("ps", "-aq", service, timeout=30).stdout.strip().splitlines()
    if not container:
        return "missing"
    result = subprocess.run(["docker", "inspect", "-f", "{{.State.ExitCode}}", container[0]], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=30, check=False)
    return result.stdout.strip() if result.returncode == 0 else "inspect-failed"


def container_id(service: str) -> str:
    lines = compose("ps", "-aq", service, timeout=30).stdout.strip().splitlines()
    return lines[0] if lines else ""


def set_restart_policy(container: str, policy: str) -> bool:
    result = subprocess.run(["docker", "update", f"--restart={policy}", container], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=30, check=False)
    return result.returncode == 0


def create_csr() -> str:
    """在应用容器里生成临时密钥与 CSR；密钥只留在容器 /tmp 并在命令末尾删除。"""
    result = compose(
        "exec", "-T", "edgepulse-1", "sh", "-c",
        "openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -subj /CN=ha-check-renew "
        "-keyout /tmp/ha-check-key.pem -out /tmp/ha-check.csr 2>/dev/null && cat /tmp/ha-check.csr; "
        "rc=$?; rm -f /tmp/ha-check-key.pem /tmp/ha-check.csr; exit $rc",
        timeout=60,
    )
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description="EdgePulse 双实例 HA 验收（Docker Compose）。")
    parser.add_argument("--build", action="store_true", help="先按仓库 Dockerfile 构建 edgepulse:ha")
    parser.add_argument("--require-docker", action="store_true", help="Docker 不可用时失败而不是 SKIP")
    arguments = parser.parse_args()

    blocker = docker_blocker()
    if blocker:
        if arguments.require_docker:
            report("HA 双实例验收", False, f"{blocker}；--require-docker 要求 Docker 必须可用")
            return 1
        skip("HA 双实例验收", f"{blocker}；安装并启动 Docker Engine 与 Compose v2 后重跑")
        return 0

    busy = [str(port) for port in [item for ports in INSTANCES.values() for item in ports.values()] + [NGINX_HTTP, NGINX_HTTPS, NGINX_AGENT]
            if not free_port(port)]
    if busy:
        report("验收端口空闲", False, "被占用：" + ", ".join(busy))
        return 1
    report("验收端口空闲", True)

    origins_ok, origins_detail = sample_allowed_origins()
    report("交付样例 allowed_origins 覆盖 HA 发布端口 8443", origins_ok, origins_detail)

    try:
        prepare()
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

    # 先置 started：compose up 中途失败也要把已创建的容器、网络与卷清掉
    started = True
    try:
        try:
            up = compose("up", "-d", "--no-build", "mysql", "redis", "ca-key-init", "edgepulse-1", "edgepulse-2", "nginx", timeout=600)
        except subprocess.TimeoutExpired:
            report("compose 启动 HA 栈", False, "600 秒内未完成启动")
            return 1
        except (OSError, subprocess.SubprocessError) as failure:
            report("compose 启动 HA 栈", False, str(failure))
            return 1
        report("compose 启动 HA 栈", up.returncode == 0, (up.stderr or up.stdout).strip()[-800:])
        if up.returncode != 0:
            return 1

        ready_a = wait_ready(INSTANCES["edgepulse-1"]["ops"])
        ready_b = wait_ready(INSTANCES["edgepulse-2"]["ops"])
        report("实例 A /readyz 就绪", ready_a)
        report("实例 B /readyz 就绪", ready_b)
        if not (ready_a and ready_b):
            return 1

        # 应用日志断言不用 entrypoint 的打印：入口脚本同样会输出 instance=...，
        # 只有应用自己启动时写下的租约装配行才能证明配置真的传到了进程。
        instance_a_ok = wait_app_log("edgepulse-1", "控制面租约已装配：instance=edgepulse-1")
        instance_b_ok = wait_app_log("edgepulse-2", "控制面租约已装配：instance=edgepulse-2")
        sink_a_ok = wait_json_log_record("edgepulse-1") is not None
        sink_b_ok = wait_json_log_record("edgepulse-2") is not None
        report("实例 A 使用显式 instance_id（应用日志）", instance_a_ok)
        report("实例 B 使用显式 instance_id（应用日志）", instance_b_ok)
        report("实例 A 日志写入配置的 JSON 滚动文件", sink_a_ok, APP_LOG)
        report("实例 B 日志写入配置的 JSON 滚动文件", sink_b_ok, APP_LOG)
        if not (instance_a_ok and instance_b_ok and sink_a_ok and sink_b_ok):
            return 1

        report("实例 A 的 Agent 内部 CA 已启用", wait_app_log("edgepulse-1", "Agent 内部 CA：已启用"))
        report("实例 B 的 Agent 内部 CA 已启用", wait_app_log("edgepulse-2", "Agent 内部 CA：已启用"))
        uid_result = compose("exec", "-T", "edgepulse-1", "id", "-u", timeout=30)
        uid = uid_result.stdout.strip()
        key_stat = compose("exec", "-T", "edgepulse-1", "stat", "-c", "%a %u", "/run/secrets/agent_ca_key", timeout=30)
        fields = key_stat.stdout.split()
        report("Agent CA 私钥在应用容器内为 0400 且属主匹配",
               len(fields) == 2 and fields[0] == "400" and fields[1] == uid,
               f"stat={key_stat.stdout.strip() or '无输出'}，uid={uid}")
        report("应用证书目录不含 CA 私钥",
               compose("exec", "-T", "edgepulse-1", "sh", "-c", "test ! -e /etc/edgepulse/tls/ca-key.pem", timeout=30, check=False).returncode == 0)
        report("nginx 看不到 ca-key.pem、ca.pem 或 CA 私钥卷",
               compose("exec", "-T", "nginx", "sh", "-c",
                       "for p in /etc/nginx/certs/ca-key.pem /etc/nginx/certs/ca.pem /run/secrets/agent_ca_key; "
                       "do if [ -e \"$p\" ]; then echo \"$p\"; exit 1; fi; done", timeout=30, check=False).returncode == 0)

        token = login(INSTANCES["edgepulse-1"]["http"])
        if token is None:
            return 1
        status, me = request_json("GET", f"http://{HOST}:{INSTANCES['edgepulse-2']['http']}/api/auth/me", token=token)
        report("实例 A 登录的令牌在实例 B 可用", status == 200 and (me.get("data") or {}).get("username") == "admin", f"HTTP {status}")

        try:
            stored = compose("exec", "-T", "redis", "sh", "-c",
                             "REDISCLI_AUTH=\"$REDIS_PASSWORD\" redis-cli --no-auth-warning EXISTS edgepulse:session:" + token,
                             timeout=30)
        except (OSError, subprocess.SubprocessError) as failure:
            report("Redis 保存共享会话键", False, str(failure))
        else:
            report("Redis 保存共享会话键", stored.returncode == 0 and stored.stdout.strip() == "1", f"EXISTS={stored.stdout.strip() or '无输出'}")

        browser_ok, browser_detail = check_browser_websocket(NGINX_HTTP, token)
        report("nginx 8080 /api/ws 升级成功（101）", browser_ok, browser_detail)

        nginx_tls = ssl.create_default_context(cafile=str(CERTS / "ca.pem"))
        wss_ok, wss_detail = check_browser_websocket(NGINX_HTTPS, token, context=nginx_tls,
                                                     origin=f"https://{HOST}:{NGINX_HTTPS}")
        report("nginx 8443 WSS /api/ws 升级成功（101）", wss_ok, wss_detail)
        rejected_ok, rejected_detail = check_ws_origin_rejected(NGINX_HTTP, "https://evil.example.com")
        report("nginx 拒绝未列入 allowed_origins 的 WebSocket Origin", rejected_ok, rejected_detail)
        try:
            sse_status, sse_headers = sse_probe(NGINX_HTTPS, token, context=nginx_tls)
        except (OSError, ssl.SSLError, http.client.HTTPException) as failure:
            sse_status, sse_headers = 0, {"error": str(failure)}
        content_type = sse_headers.get("content-type", "")
        report("nginx 8443 SSE 返回 text/event-stream", sse_status == 200 and "text/event-stream" in content_type,
               f"HTTP {sse_status}，content-type={content_type or sse_headers.get('error', '无')}")

        # 用开发 Agent 证书注册一个真实节点，再用同一证书建 WS；节点身份由 mTLS 指纹核对。
        # 注册走 nginx 的 9443 TCP 透传，证明这条链路没有被 L7 终止。
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
            "POST", f"https://{HOST}:{NGINX_AGENT}/api/agent/register",
            {"name": "ha-check-agent", "agentVersion": "ha-check", "certFingerprint": fingerprint, "tenantCode": "default",
             "labels": "{}", "enrollmentToken": enrollment_token},
            context=client_context,
        )
        node_id = int((registration.get("data") or {}).get("id") or 0)
        report("Agent 经 nginx 9443 TCP 透传注册", status == 200 and node_id > 0, f"HTTP {status}，nodeId={node_id}")
        if node_id <= 0:
            return 1

        websocket = WebSocketConnection(HOST, INSTANCES["edgepulse-1"]["agent"], "/api/agent/ws",
                                        f"https://{HOST}:{NGINX_HTTPS}", client_context)
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

        # 跨实例共享固件卷：经 A 上传，经 B 下载，比较字节数。
        firmware_payload = b"ha-check-firmware-" + b"x" * 65536
        status, _, upload_raw = request_raw(
            "POST", f"http://{HOST}:{INSTANCES['edgepulse-1']['http']}/api/firmware?name=ha-check&version=1.0.0&fileName=ha-check.bin",
            data=firmware_payload, token=token, headers={"Content-Type": "application/octet-stream"}, timeout=60,
        )
        try:
            upload_data = (json.loads(upload_raw.decode("utf-8", "replace")).get("data") or {}) if upload_raw else {}
        except json.JSONDecodeError:
            upload_data = {}
        firmware_id = int(upload_data.get("id") or 0)
        report("实例 A 上传固件", status == 200 and firmware_id > 0, f"HTTP {status}，firmwareId={firmware_id}")
        if firmware_id <= 0:
            return 1
        status, _, downloaded = request_raw(
            "GET", f"https://{HOST}:{INSTANCES['edgepulse-2']['agent']}/api/agent/firmware/{firmware_id}/download?nodeId={node_id}",
            context=client_context, timeout=60,
        )
        report("实例 B 下载实例 A 上传的固件", status == 200 and downloaded == firmware_payload,
               f"HTTP {status}，{len(downloaded)}/{len(firmware_payload)} 字节")

        # 灰度单飞：两个实例都在跑 5 秒推进循环，只有 leader 能落库；同一目标跨多个 tick
        # 也只能有一条任务与一条命令。策略只要求派发，不让门控在本次检查内收尾。
        rollout_policy = {
            "waves": [{"percent": 100}],
            "maxConcurrentPerWave": 0,
            "settleSeconds": 60,
            "observeSeconds": 0,
            "maxWaveSeconds": 120,
            "minWaveSuccessRatio": 1,
            "minCumulativeSuccessRatio": 1,
            "maxFailures": 0,
            "autoRollback": False,
            "onTimeout": "pause",
            "requiredFreshTelemetrySeconds": 300,
            "exemptAlarmMetrics": [],
            "requireInstalledVersion": False,
        }
        status, created = request_json(
            "POST", f"http://{HOST}:{INSTANCES['edgepulse-1']['http']}/api/firmware/{firmware_id}/rollouts",
            {"name": "ha-rollout-single-flight", "nodeIds": [node_id], "policy": rollout_policy}, token=token,
        )
        rollout_id = int((created.get("data") or {}).get("id") or 0)
        report("实例 A 创建灰度发布", status == 200 and rollout_id > 0, f"HTTP {status}，rolloutId={rollout_id}")
        if rollout_id <= 0:
            return 1

        def dispatched_targets() -> int | None:
            row = mysql_scalar(f"SELECT COUNT(*) FROM biz_firmware_rollout_target WHERE rollout_id={rollout_id} AND command_id>0")
            return int(row) if row is not None and row.isdigit() else None

        def wait_dispatched() -> int | None:
            count = dispatched_targets()
            return count if count is not None and count >= 1 else None

        dispatched = wait_until(wait_dispatched, 30.0, 0.5)
        report("灰度目标已由 leader 派发命令", dispatched is not None, f"commandCount={dispatched_targets()}")
        # 覆盖至少两个 5 秒 tick 与一次租约续约窗口；若 follower 也推进，会多出任务或命令。
        time.sleep(12.0)
        task_count = mysql_scalar(f"SELECT COUNT(*) FROM biz_firmware_task WHERE rollout_id={rollout_id} AND node_id={node_id}")
        target_count = mysql_scalar(f"SELECT COUNT(*) FROM biz_firmware_rollout_target WHERE rollout_id={rollout_id}")
        command_count = mysql_scalar(f"SELECT COUNT(DISTINCT command_id) FROM biz_firmware_rollout_target WHERE rollout_id={rollout_id} AND command_id>0")
        lease = lease_state()
        report("灰度单飞：单目标只有一条任务与一条命令",
               task_count == "1" and target_count == "1" and command_count == "1",
               f"tasks={task_count}，targets={target_count}，commands={command_count}，"
               f"holder={lease[0] if lease else '无'}")
        status, aborted = request_json("POST", f"http://{HOST}:{INSTANCES['edgepulse-1']['http']}/api/firmware/rollouts/{rollout_id}/abort", {}, token=token)
        abort_status = (aborted.get("data") or {}).get("status")
        report("中止 HA 灰度发布", status == 200 and abort_status == "aborted", f"HTTP {status}，status={abort_status}")

        # 启用 agent_ca 后，经 nginx 9443 走一次真实续期：应用必须能读取 0400 私钥并完成签发。
        csr = create_csr()
        status, renewal = request_json(
            "POST", f"https://{HOST}:{NGINX_AGENT}/api/agent/certificate/renew",
            {"nodeId": node_id, "csrPem": csr}, context=client_context, timeout=30,
        )
        renewal_data = renewal.get("data") or {}
        certificate_pem = str(renewal_data.get("certificatePem", ""))
        renewal_fingerprint = str(renewal_data.get("fingerprint", "")).lower()
        report("Agent 经 nginx 9443 续期并取得 CA 签发证书",
               status == 200 and certificate_pem.startswith("-----BEGIN CERTIFICATE-----") and len(renewal_fingerprint) == 64,
               f"HTTP {status}，fingerprint={renewal_fingerprint[:16]}…" if status == 200 else f"HTTP {status}")
        if certificate_pem:
            verify = compose(
                "exec", "-T", "edgepulse-1", "sh", "-c",
                "cat > /tmp/ha-renewed.pem && openssl verify -CAfile /etc/edgepulse/tls/ca.pem /tmp/ha-renewed.pem && "
                "openssl x509 -in /tmp/ha-renewed.pem -noout -fingerprint -sha256; "
                "rc=$?; rm -f /tmp/ha-renewed.pem; exit $rc",
                timeout=60, input_text=certificate_pem, check=False,
            )
            match = re.search(r"Fingerprint=([0-9A-Fa-f:]+)", verify.stdout)
            observed = match.group(1).replace(":", "").lower() if match else ""
            report("续期证书由配置的 CA 验证通过且指纹一致",
                   verify.returncode == 0 and observed == renewal_fingerprint, (verify.stdout or verify.stderr).strip()[-300:])
        else:
            report("续期证书由配置的 CA 验证通过且指纹一致", False, "续期响应没有证书")

        # 在实例 A 建立一条已登记的 Agent WS，再从实例 B 吊销该节点证书：关闭消息必须
        # 经 Redis 转到 A，并把 A 上这条连接以 1008 关闭。前面的命令、固件与续期检查
        # 都已完成，吊销不会影响它们。
        revoke_ws = WebSocketConnection(HOST, INSTANCES["edgepulse-1"]["agent"], "/api/agent/ws",
                                        f"https://{HOST}:{NGINX_HTTPS}", client_context)
        try:
            revoke_ws.send_text(json.dumps({"type": "hello", "nodeId": node_id, "agentVersion": "ha-check"}))
            revoke_hello = revoke_ws.wait_for_text(10.0) or ""
            revoke_hello_ok = False
            if revoke_hello:
                try:
                    revoke_hello_ok = json.loads(revoke_hello).get("type") == "hello"
                except json.JSONDecodeError:
                    revoke_hello_ok = False
            report("吊销前在实例 A 登记新的 Agent WS", revoke_hello_ok, revoke_hello or "10 秒内没有应答")
            if not revoke_hello_ok:
                return 1
            status, _ = request_json("POST", f"http://{HOST}:{INSTANCES['edgepulse-2']['http']}/api/nodes/{node_id}/certificate/revoke",
                                     token=token)
            closed = revoke_ws.wait_for_close(15.0)
            report("实例 B 吊销节点证书后实例 A 的 Agent WS 以 1008 关闭",
                   status == 200 and closed is not None and closed[0] == 1008,
                   f"HTTP {status}，close={closed[0] if closed else '无'}（{(closed[1] if closed else '15 秒内未收到关闭帧')}）")
        finally:
            revoke_ws.close()

        leader_state = wait_leader(30.0)
        report("sys_lease 选出 leader", leader_state is not None, f"holder={leader_state[0] if leader_state else '无'}，fence={leader_state[1] if leader_state else 0}")
        if leader_state is None:
            return 1
        leader, leader_fence = leader_state
        follower = "edgepulse-2" if leader == "edgepulse-1" else "edgepulse-1"
        leader_ports = INSTANCES[leader]

        pre_status, _ = readyz(leader_ports["ops"])
        report("SIGTERM 前 leader /readyz 为 200", pre_status == 200, f"HTTP {pre_status}")
        if pre_status != 200:
            return 1

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

        browser_after, browser_after_detail = check_browser_websocket(NGINX_HTTP, token)
        report("单实例停机后 nginx /api/ws 仍可升级（101）", browser_after, browser_after_detail)

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

        # 崩溃停机：直接 SIGKILL。unless-stopped 会把 kill 当成普通退出并自动拉起，先临时
        # 改成 no，测完再恢复，才能观察「租约到期才接管」。
        crash_leader = follower
        crash_fence = takeover[1] if takeover else -1
        crash_container = container_id(crash_leader)
        if not crash_container:
            report("SIGKILL 崩溃停机", False, "找不到 leader 容器")
            return 1
        # 等到 holder 刚续约过再杀，才能观察「必须等 TTL 到期」；否则剩余时间太短，
        # 接管会看起来像立即抢占。
        fresh = wait_until(lambda: (lease_remaining_seconds() or 0) >= 10, 20.0, 0.5)
        if not fresh:
            report("SIGKILL 前 leader 租约处于有效期", False, "20 秒内没有读到至少 10 秒的剩余租期")
            return 1
        report("SIGKILL 前 leader 租约处于有效期", True, f"剩余约 {lease_remaining_seconds()} 秒")
        if not set_restart_policy(crash_container, "no"):
            report("暂停 leader 的自动重启策略", False, "docker update --restart=no 失败")
            return 1
        report("暂停 leader 的自动重启策略", True)
        crash_started = time.monotonic()
        try:
            killed = compose("kill", "-s", "SIGKILL", crash_leader, timeout=60)
        except (OSError, subprocess.SubprocessError) as failure:
            report("SIGKILL 崩溃停机", False, str(failure))
            return 1
        report("SIGKILL 崩溃停机", killed.returncode == 0, (killed.stderr or "").strip()[-200:])
        if killed.returncode != 0:
            return 1
        crash_exit = inspect_exit_code(crash_leader)
        report("SIGKILL 退出码为 137", crash_exit == "137", f"exit={crash_exit}")
        time.sleep(0.5)
        remaining = lease_remaining_seconds()
        retained = lease_state()
        report("SIGKILL 后旧 holder 仍留在租约行且未清空",
               retained is not None and retained[0] == crash_leader and retained[1] == crash_fence,
               f"holder={retained[0] if retained else '无'}，fence={retained[1] if retained else 0}，剩余 {remaining if remaining is not None else '?'} 秒")
        crash_takeover = wait_until(
            lambda: (lambda state: state if state is not None and state[0] == leader and state[1] > crash_fence else None)(lease_state()),
            LEADER_TAKEOVER_LIMIT + 10.0, 0.5,
        )
        crash_elapsed = time.monotonic() - crash_started
        # 旧 holder 的 expires_at 仍在未来时不允许立即接手；留 2 秒给轮询与整数秒截断。
        # 读不到剩余时间不能当成“未提前”：那会把无法证明的接管报成通过。
        waited_for_expiry = remaining is not None and crash_elapsed + 2.0 >= max(0, remaining)
        report("崩溃后新 leader 在 2 个 TTL 内接管且未提前抢占",
               crash_takeover is not None and crash_elapsed <= LEADER_TAKEOVER_LIMIT and waited_for_expiry,
               f"{crash_elapsed:.2f} 秒，holder={crash_takeover[0] if crash_takeover else '无'}，fence={crash_takeover[1] if crash_takeover else 0}，"
               f"剩余 {remaining if remaining is not None else '读取失败'} 秒")
        if crash_takeover is None:
            return 1
        report("崩溃接管后新 leader /readyz 就绪", wait_ready(INSTANCES[leader]["ops"], 60.0))
        report("崩溃后 fence 递增", crash_takeover[1] > crash_fence, f"{crash_fence} -> {crash_takeover[1]}")

        report("恢复旧 leader 的重启策略", set_restart_policy(crash_container, "unless-stopped"))
        compose("start", crash_leader, timeout=120)
        report("崩溃的旧 leader 重启后就绪", wait_ready(INSTANCES[crash_leader]["ops"], 120.0))
        time.sleep(3.0)
        after_crash_restart = lease_state()
        report("崩溃重启的旧 holder 不清空新租约",
               after_crash_restart is not None and after_crash_restart[0] == leader and after_crash_restart[1] == crash_takeover[1],
               f"holder={after_crash_restart[0] if after_crash_restart else '无'}，fence={after_crash_restart[1] if after_crash_restart else 0}")

        # 共享 Redis 是 /readyz 的第三项依赖：停掉后两个实例都必须变 503，恢复后都回到 200。
        # 放在最后执行，避免会话或命令路由断言受 Redis 停机影响。
        redis_stop = compose("stop", "redis", timeout=60, check=False)
        report("停止共享 Redis", redis_stop.returncode == 0, (redis_stop.stderr or redis_stop.stdout).strip()[-200:])
        report("停止 Redis 后实例 A /readyz 返回 503", wait_ready_status(INSTANCES["edgepulse-1"]["ops"], 503, 60.0))
        report("停止 Redis 后实例 B /readyz 返回 503", wait_ready_status(INSTANCES["edgepulse-2"]["ops"], 503, 60.0))
        redis_start = compose("start", "redis", timeout=120, check=False)
        report("重新启动共享 Redis", redis_start.returncode == 0, (redis_start.stderr or redis_start.stdout).strip()[-200:])
        report("Redis 恢复后实例 A /readyz 回到 200", wait_ready(INSTANCES["edgepulse-1"]["ops"], 120.0))
        report("Redis 恢复后实例 B /readyz 回到 200", wait_ready(INSTANCES["edgepulse-2"]["ops"], 120.0))
    except Exception as failure:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        report("HA 验收执行未抛出异常", False, f"{type(failure).__name__}: {failure}")
    finally:
        if started:
            try:
                logs = compose("logs", "--no-color", check=False, timeout=60)
                LOG_PATH.write_text(redact(logs.stdout + logs.stderr), encoding="utf-8")
            except (OSError, subprocess.SubprocessError) as failure:
                print("WARN 保存 HA 栈日志失败：" + redact(str(failure)))
            try:
                down = compose("down", "-v", "--remove-orphans", check=False, timeout=120)
            except (OSError, subprocess.SubprocessError) as failure:
                report("清理 HA 容器、网络与卷", False, str(failure))
            else:
                report("清理 HA 容器、网络与卷", down.returncode == 0, (down.stderr or "").strip()[-300:])
        remove_gate_secrets()

    print()
    if failures:
        print(f"HA 双实例验收失败：{len(failures)} 项 -> " + ", ".join(failures))
        return 1
    print(f"HA 双实例验收全部通过（{passes} 项）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
