#!/usr/bin/env python3
"""生产 Agent 端到端验收：临时控制面加真实 edgepulse-agent，覆盖注册、心跳、遥测、配置、诊断、签名固件、离线补报、重启与证书改绑。"""

from __future__ import annotations

import base64
import hashlib
import http.client
import json
import math
import os
import re
import secrets
import shutil
import signal
import socket
import sqlite3
import ssl
import subprocess
import sys
import tempfile
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

ROOT = Path(__file__).resolve().parents[1]
CERTS = ROOT / "var" / "certs"
HTTP_BASE = "http://127.0.0.1:8080"
AGENT_BASE = "https://127.0.0.1:9443"
SERVER_PORTS = (8080, 8443, 9100, 9443)
RUN_ID = str(time.time_ns())[-8:]
AGENT_NAME = f"e2e-agent-{RUN_ID}"
ENROLLMENT_ENV = "EDGEPULSE_AGENT_E2E_ENROLLMENT_TOKEN"
REBIND_ENV = "EDGEPULSE_AGENT_E2E_REBIND_TOKEN"
HOOK_MARKER_ENV = "EDGEPULSE_AGENT_TEST_HOOK_MARKER"
# 开发 shell 可能留下这些 hook 覆盖变量，注入验收环境前必须清掉
SCRUBBED_AGENT_ENV = (ENROLLMENT_ENV, REBIND_ENV, "EDGEPULSE_AGENT_TEST_HOOK_EXIT", "EDGEPULSE_AGENT_TEST_HOOK_OUTPUT",
                      "EDGEPULSE_AGENT_TEST_HOOK_REPLACE_WITH_DIRECTORY")
AGENT_CONFIG_CONTENT = {"mode": "e2e", "sample_interval": 17}
TELEMETRY_METRICS = ("cpu.usage", "memory.usage", "disk.usage")

failures: list[str] = []
skips: list[str] = []
passed = 0
known_secrets: set[str] = set()
admin_tokens: list[str] = []


def remember_secret(value: str) -> None:
    if value:
        known_secrets.add(value)


def redact_secrets(text: str) -> str:
    """把脚本持有的令牌明文从任何输出里抹掉，避免失败诊断回显凭据。"""
    for secret in sorted(known_secrets, key=len, reverse=True):
        if len(secret) >= 12:
            text = text.replace(secret, "***")
    return text


def report(name: str, ok: bool, detail: str = "") -> None:
    global passed
    print(("PASS " if ok else "FAIL ") + name + (("：" + redact_secrets(detail)) if detail else ""))
    if ok:
        passed += 1
    else:
        failures.append(name)


def skip(name: str, detail: str = "") -> None:
    """记录本次无法构造的检查；跳过不是通过，也不让退出码变成失败。"""
    print("SKIP " + name + (("：" + redact_secrets(detail)) if detail else ""))
    skips.append(name)


def request_json(method: str, url: str, payload: dict | None = None, token: str | None = None, context: ssl.SSLContext | None = None,
                 timeout: float = 30.0) -> tuple[int, dict]:
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {"Content-Type": "application/json"} if data is not None else {}
    if token:
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout, context=context) as response:
            body = response.read().decode("utf-8", "replace")
            try:
                parsed = json.loads(body) if body else {}
            except json.JSONDecodeError:
                parsed = {}
            return response.status, parsed
    except urllib.error.HTTPError as failure:
        body = failure.read().decode("utf-8", "replace")
        try:
            parsed = json.loads(body) if body else {}
        except json.JSONDecodeError:
            parsed = {}
        return failure.code, parsed


def executable(base: Path) -> Path:
    candidate = base.with_suffix(".exe")
    return candidate if candidate.exists() else base


def client_context(certificate: Path | None = None, private_key: Path | None = None) -> ssl.SSLContext:
    context = ssl.create_default_context(cafile=str(CERTS / "ca.pem"))
    if certificate is not None and private_key is not None:
        context.load_cert_chain(certfile=str(certificate), keyfile=str(private_key))
    return context


def certificate_fingerprint(certificate: Path) -> str:
    result = subprocess.run(["openssl", "x509", "-in", str(certificate), "-outform", "DER"], check=True, capture_output=True)
    return hashlib.sha256(result.stdout).hexdigest()


def read_log_tail(path: Path, maximum_bytes: int = 4000) -> str:
    try:
        data = path.read_bytes()
    except OSError:
        return "无日志"
    return redact_secrets(data[-maximum_bytes:].decode("utf-8", "replace").strip()) or "无日志"


def stop_process(process: subprocess.Popen | None) -> None:
    if process is None or process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(process.pid)], capture_output=True, check=False)
    else:
        # start_logged_process 用 start_new_session 建了独立会话：按进程组发信号，
        # 服务端若在收尾期间还有子进程，单杀主 PID 会把监听套接字留在子进程里
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=8)
            return
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
    try:
        process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        pass


def start_logged_process(command: list[str], cwd: Path, env: dict, log_path: Path) -> tuple[subprocess.Popen, object]:
    # 追加写入：服务端与 Agent 都会重启，截断会让首轮输出（可能含令牌泄漏）逃过扫描
    stream = open(log_path, "ab")
    creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    process = subprocess.Popen(command, cwd=str(cwd), env=env, stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT,
                               creationflags=creation_flags, start_new_session=os.name != "nt")
    return process, stream


def process_alive(pid: int) -> bool:
    """只查询进程是否还在，不发送任何信号；Windows 上不能用 os.kill(pid, 0)。"""
    if os.name == "nt":
        result = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"], capture_output=True, text=True, encoding="utf-8",
                                errors="replace", check=False)
        return str(pid) in (result.stdout or "")
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def build_agent_environment(extra: dict[str, str]) -> dict[str, str]:
    environment = os.environ.copy()
    for name in SCRUBBED_AGENT_ENV:
        environment.pop(name, None)
    environment.update(extra)
    return environment


def port_is_free(port: int) -> bool:
    """用绑定冲突判断端口是否被监听；连接探测会被本机防火墙丢包而误判。"""
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        if os.name != "nt":
            # 服务端停止后已关闭的连接会让端口进入 TIME_WAIT；不带 SO_REUSEADDR 的绑定会把它误判成仍有监听者。
            # Windows 上 SO_REUSEADDR 允许抢占正在监听的端口，反而会让检查失真，所以只在 POSIX 上加。
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        probe.close()


def wait_port_free(port: int, timeout: float = 15.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if port_is_free(port):
            return True
        time.sleep(0.2)
    return False


def wait_for_health(process: subprocess.Popen, timeout: float = 60.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if process.poll() is not None:
            return False
        try:
            status, payload = request_json("GET", HTTP_BASE + "/api/health", timeout=2.0)
            if status == 200 and (payload.get("data") or {}).get("status") == "UP":
                return True
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError):
            pass
        time.sleep(0.5)
    return False


def read_agent_state(database: Path, key: str) -> str | None:
    if not database.exists():
        return None
    for _ in range(5):
        try:
            connection = sqlite3.connect(database.resolve().as_uri() + "?mode=ro", uri=True, timeout=5)
        except sqlite3.Error:
            time.sleep(0.2)
            continue
        try:
            row = connection.execute("SELECT value FROM state WHERE key = ?", (key,)).fetchone()
            return str(row[0]) if row else None
        except sqlite3.Error:
            time.sleep(0.2)
        finally:
            connection.close()
    return None


def wait_agent_state(database: Path, key: str, expected: str | None, timeout: float) -> str | None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = read_agent_state(database, key)
        if value is not None and (expected is None or value == expected):
            return value
        time.sleep(0.3)
    return read_agent_state(database, key)


def queue_depths(database: Path) -> tuple[int, int] | None:
    if not database.exists():
        return None
    for _ in range(3):
        try:
            connection = sqlite3.connect(database.resolve().as_uri() + "?mode=ro", uri=True, timeout=5)
        except sqlite3.Error:
            time.sleep(0.2)
            continue
        try:
            telemetry = connection.execute("SELECT COUNT(*) FROM telemetry_queue").fetchone()[0]
            results = connection.execute("SELECT COUNT(*) FROM result_queue").fetchone()[0]
            return int(telemetry), int(results)
        except sqlite3.Error:
            time.sleep(0.2)
        finally:
            connection.close()
    return None


def server_telemetry_total(database: Path, node_id: int) -> int:
    for _ in range(5):
        if not database.exists():
            return 0
        try:
            connection = sqlite3.connect(database.resolve().as_uri() + "?mode=ro", uri=True, timeout=5)
        except sqlite3.Error:
            time.sleep(0.2)
            continue
        try:
            row = connection.execute("SELECT COUNT(*) FROM biz_telemetry WHERE node_id = ?", (node_id,)).fetchone()
            return int(row[0]) if row else 0
        except sqlite3.Error:
            time.sleep(0.2)
        finally:
            connection.close()
    return -1


def server_telemetry_duplicate_groups(database: Path, node_id: int) -> int:
    for _ in range(5):
        if not database.exists():
            return 0
        try:
            connection = sqlite3.connect(database.resolve().as_uri() + "?mode=ro", uri=True, timeout=5)
        except sqlite3.Error:
            time.sleep(0.2)
            continue
        try:
            row = connection.execute("SELECT COUNT(*) FROM (SELECT metric, collected_at FROM biz_telemetry WHERE node_id = ? "
                                     "GROUP BY metric, collected_at HAVING COUNT(*) > 1)", (node_id,)).fetchone()
            return int(row[0]) if row else 0
        except sqlite3.Error:
            time.sleep(0.2)
        finally:
            connection.close()
    return -1


def queued_telemetry_samples(database: Path) -> list[tuple[str, str, float]]:
    """读取离线队列每一行的 metric、collectedAt 与数值，作为补报必须逐条落库的精确身份。"""
    if not database.exists():
        return []
    for _ in range(5):
        try:
            connection = sqlite3.connect(database.resolve().as_uri() + "?mode=ro", uri=True, timeout=5)
        except sqlite3.Error:
            time.sleep(0.2)
            continue
        try:
            rows = connection.execute("SELECT payload FROM telemetry_queue ORDER BY id").fetchall()
        except sqlite3.Error:
            time.sleep(0.2)
            continue
        finally:
            connection.close()
        samples: list[tuple[str, str, float]] = []
        for (payload,) in rows:
            try:
                sample = json.loads(str(payload))
            except (TypeError, json.JSONDecodeError):
                continue
            if not isinstance(sample, dict):
                continue
            metric = sample.get("metric")
            collected_at = sample.get("collectedAt")
            value = sample.get("value")
            if isinstance(metric, str) and isinstance(collected_at, str) and isinstance(value, (int, float)):
                samples.append((metric, collected_at, float(value)))
        return samples
    return []


def server_telemetry_samples(database: Path, node_id: int) -> list[tuple[str, str, float]]:
    if not database.exists():
        return []
    for _ in range(5):
        try:
            connection = sqlite3.connect(database.resolve().as_uri() + "?mode=ro", uri=True, timeout=5)
        except sqlite3.Error:
            time.sleep(0.2)
            continue
        try:
            rows = connection.execute("SELECT metric, collected_at, value FROM biz_telemetry WHERE node_id = ?", (node_id,)).fetchall()
        except sqlite3.Error:
            time.sleep(0.2)
            continue
        finally:
            connection.close()
        return [(str(metric), str(collected_at), float(value)) for metric, collected_at, value in rows]
    return []


def missing_telemetry_samples(expected: list[tuple[str, str, float]], actual: list[tuple[str, str, float]]) -> list[tuple[str, str, float]]:
    """按 metric + collectedAt + 数值比对；数值用容差比较，其余两项必须精确相等。"""
    by_key: dict[tuple[str, str], list[float]] = {}
    for metric, collected_at, value in actual:
        by_key.setdefault((metric, collected_at), []).append(value)
    missing: list[tuple[str, str, float]] = []
    for metric, collected_at, value in expected:
        candidates = by_key.get((metric, collected_at), [])
        if not any(math.isclose(candidate, value, rel_tol=1e-9, abs_tol=1e-9) for candidate in candidates):
            missing.append((metric, collected_at, value))
    return missing


def format_telemetry_samples(samples: list[tuple[str, str, float]]) -> str:
    return "、".join(f"{metric}@{collected_at}={value}" for metric, collected_at, value in samples)


def patch_lines(text: str, replacements: list[tuple[str, str, str]]) -> str:
    for pattern, replacement, label in replacements:
        updated, count = re.subn(pattern, replacement, text, flags=re.MULTILINE)
        if count != 1:
            raise RuntimeError(f"config/dev.yaml 中 {label} 的匹配次数为 {count}，无法生成临时服务配置")
        text = updated
    return text


class FirmwareSigning:
    """一次验收运行使用的临时 Ed25519 签名材料，公钥同时复制给服务端与 Agent。"""

    def __init__(self, key_id: str, payload: bytes, signature: str) -> None:
        self.key_id = key_id
        self.payload = payload
        self.signature = signature


class Session:
    """持有本次运行的服务端与 Agent 进程、临时路径与日志句柄。"""

    def __init__(self, temp_root: Path) -> None:
        self.temp_root = temp_root
        self.server_database = temp_root / "server-data" / "edgepulse.db"
        self.agent_database = temp_root / "state" / "agent.db"
        self.agent_log = temp_root / "logs" / "agent.log"
        self.agent_stdout = temp_root / "logs" / "agent-stdout.log"
        self.server_log = temp_root / "logs" / "server.log"
        self.hook_marker = temp_root / "hook-marker.txt"
        self.server_config = temp_root / "server.yaml"
        self.agent_config = temp_root / "agent.yaml"
        self.ops_token = secrets.token_hex(32)
        remember_secret(self.ops_token)
        self.server_process: subprocess.Popen | None = None
        self.server_stream = None
        self.agent_process: subprocess.Popen | None = None
        self.agent_stream = None
        self.started_pids: list[int] = []
        self.server_ports_released = True
        self.node_id = 0
        self.agent_version = ""
        self.agent_fingerprint = ""
        (temp_root / "logs").mkdir(parents=True, exist_ok=True)
        (temp_root / "server-data").mkdir(parents=True, exist_ok=True)
        (temp_root / "server-firmware").mkdir(parents=True, exist_ok=True)
        (temp_root / "server-firmware-keys").mkdir(parents=True, exist_ok=True)
        (temp_root / "agent-firmware-keys").mkdir(parents=True, exist_ok=True)
        (temp_root / "agent-firmware").mkdir(parents=True, exist_ok=True)
        (temp_root / "agent-update").mkdir(parents=True, exist_ok=True)
        (temp_root / "state").mkdir(parents=True, exist_ok=True)
        (temp_root / "web").mkdir(parents=True, exist_ok=True)

    def write_server_config(self) -> None:
        source = (ROOT / "config" / "dev.yaml").read_text(encoding="utf-8")
        replacements = [
            (r"^(\s*)web_root: web\s*$", rf"\g<1>web_root: '{self.temp_root.as_posix()}/web'", "web_root"),
            (r"^(\s*)cert_dir: var/certs\s*$", rf"\g<1>cert_dir: '{(ROOT / 'var' / 'certs').as_posix()}'", "cert_dir"),
            (r"^(\s*)firmware_dir: var/firmware\s*$", rf"\g<1>firmware_dir: '{(self.temp_root / 'server-firmware').as_posix()}'", "firmware_dir"),
            (r"^(\s*)trusted_keys_dir: config/firmware-keys\s*$",
             rf"\g<1>trusted_keys_dir: '{(self.temp_root / 'server-firmware-keys').as_posix()}'", "firmware.trusted_keys_dir"),
            (r"^(\s*)path: var/edgepulse\.db\s*$", rf"\g<1>path: '{self.server_database.as_posix()}'", "database.path"),
            (r"^(\s*)migrations_dir: migrations\s*$", rf"\g<1>migrations_dir: '{(ROOT / 'migrations').as_posix()}'", "migrations_dir"),
            (r'^(\s*)enrollment_mode: "off"\s*$', r'\g<1>enrollment_mode: "required"', "agent.enrollment_mode"),
            (r"^(\s*)heartbeat_interval_seconds: 10\s*$", r"\g<1>heartbeat_interval_seconds: 5", "agent.heartbeat_interval_seconds"),
            (r"^(\s*)certificate_file: var/certs/ca\.pem\s*$", rf"\g<1>certificate_file: '{(CERTS / 'ca.pem').as_posix()}'", "agent_ca.certificate_file"),
            (r"^(\s*)key_file: var/certs/ca-key\.pem\s*$", rf"\g<1>key_file: '{(CERTS / 'ca-key.pem').as_posix()}'", "agent_ca.key_file"),
        ]
        self.server_config.write_text(patch_lines(source, replacements), encoding="utf-8")

    def write_agent_config(self, certificate: Path, private_key: Path) -> None:
        temporary = self.temp_root
        text = f"""agent:
  server:
    base_url: "https://127.0.0.1:9443"
    ca_file: "{(CERTS / 'ca.pem').as_posix()}"
    certificate_file: "{certificate.as_posix()}"
    private_key_file: "{private_key.as_posix()}"
    tenant_code: "default"
    name: "{AGENT_NAME}"
    enrollment_token_env: "{ENROLLMENT_ENV}"
    rebind_token_env: "{REBIND_ENV}"
    heartbeat_interval_seconds: 5
    command_poll_interval_seconds: 5
    websocket_enabled: true
    request_timeout_seconds: 10
  telemetry:
    interval_seconds: 5
    disk_path: "{'C:/' if os.name == 'nt' else '/'}"
  state:
    directory: "{(temporary / 'state').as_posix()}"
  logging:
    file: "{(temporary / 'logs' / 'agent.log').as_posix()}"
    level: info
    max_size_mb: 8
    max_backup: 2
  executors:
    config:
      path: "{(temporary / 'agent-config.json').as_posix()}"
      apply_hook: ""
      maximum_bytes: 65536
    firmware:
      install_hook: "{executable(ROOT / 'build' / 'debug' / 'bin' / 'edgepulse-agent-test-hook').as_posix()}"
      trusted_keys_dir: "{(temporary / 'agent-firmware-keys').as_posix()}"
      staging_directory: "{(temporary / 'agent-firmware').as_posix()}"
    diagnostics:
      log_tail_bytes: 65536
  update:
    package_trusted_keys_dir: "{(temporary / 'agent-package-keys').as_posix()}"
    installer_script: ""
    health_timeout_seconds: 60
    staging_directory: "{(temporary / 'agent-update').as_posix()}"
"""
        self.agent_config.write_text(text, encoding="utf-8")

    def server_environment(self) -> dict:
        environment = os.environ.copy()
        # 白名单一旦继承自开发 shell 会拒绝改绑后的新证书；本次验收只依赖 mTLS 链校验
        environment.pop("EDGEPULSE_AGENT_FINGERPRINT", None)
        environment["EDGEPULSE_OPS_BEARER_TOKEN"] = self.ops_token
        return environment

    def start_server(self, label: str) -> bool:
        binary = executable(ROOT / "build" / "debug" / "edgepulse-server")
        if not binary.exists():
            report(label, False, f"找不到服务端二进制：{binary}")
            return False
        close_stream(self.server_stream)
        self.server_stream = None
        self.server_process, self.server_stream = start_logged_process([str(binary), "--config", str(self.server_config)], self.temp_root,
                                                                      self.server_environment(), self.server_log)
        self.started_pids.append(self.server_process.pid)
        if not wait_for_health(self.server_process):
            report(label, False, "服务端未在 60 秒内就绪：" + read_log_tail(self.server_log))
            return False
        report(label, True, "8080 健康检查 UP")
        return True

    def stop_server(self) -> bool:
        stop_process(self.server_process)
        self.server_process = None
        close_stream(self.server_stream)
        self.server_stream = None
        released = True
        for port in SERVER_PORTS:
            if not wait_port_free(port):
                released = False
        self.server_ports_released = released
        return released

    def start_agent(self, environment: dict) -> bool:
        binary = executable(ROOT / "build" / "debug" / "bin" / "edgepulse-agent")
        if not binary.exists():
            report("Agent 启动", False, f"找不到 Agent 二进制：{binary}")
            return False
        close_stream(self.agent_stream)
        self.agent_stream = None
        self.agent_process, self.agent_stream = start_logged_process([str(binary), "--config", str(self.agent_config), "--console"], self.temp_root,
                                                                     environment, self.agent_stdout)
        self.started_pids.append(self.agent_process.pid)
        return True

    def stop_agent(self) -> None:
        stop_process(self.agent_process)
        self.agent_process = None
        close_stream(self.agent_stream)
        self.agent_stream = None

    def log_offsets(self) -> tuple[int, int]:
        def size(path: Path) -> int:
            try:
                return path.stat().st_size
            except OSError:
                return 0
        return size(self.agent_log), size(self.agent_stdout)

    def close(self) -> None:
        self.stop_agent()
        self.stop_server()
        for _ in range(10):
            shutil.rmtree(self.temp_root, ignore_errors=True)
            if not self.temp_root.exists():
                return
            time.sleep(0.5)


def close_stream(stream: object) -> None:
    if stream is None:
        return
    try:
        stream.close()
    except OSError:
        pass


def ensure_dev_certificates() -> str:
    required = ["ca.pem", "ca-key.pem", "server.pem", "server-key.pem", "agent.pem", "agent-key.pem"]
    if all((CERTS / name).exists() for name in required):
        return ""
    if shutil.which("bash") is None:
        return "缺少开发证书，且找不到 bash 来运行 scripts/dev-certs.sh；请先手动生成证书"
    result = subprocess.run(["bash", "scripts/dev-certs.sh"], cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace", check=False)
    missing = [name for name in required if not (CERTS / name).exists()]
    if missing:
        return f"缺少开发证书 {', '.join(missing)}；scripts/dev-certs.sh 退出码 {result.returncode}：{(result.stderr or result.stdout).strip()[-400:]}"
    return ""


def prepare_firmware_signing(session: Session) -> tuple[FirmwareSigning | None, str, bool]:
    """生成本次运行专用的 Ed25519 密钥并把公钥放进服务端与 Agent 的信任键目录。"""
    if shutil.which("openssl") is None:
        return None, "缺少 openssl，无法生成临时固件签名密钥", True
    signing_dir = session.temp_root / "signing"
    signing_dir.mkdir(parents=True, exist_ok=True)
    key_path = signing_dir / "e2e-firmware-key.pem"
    public_key_path = signing_dir / "e2e-firmware.pem"
    payload_path = signing_dir / "firmware.bin"
    digest_path = signing_dir / "firmware-digest.bin"
    signature_path = signing_dir / "firmware-signature.bin"
    payload = (f"EdgePulse agent e2e signed firmware {RUN_ID}\n" * 128).encode()
    try:
        subprocess.run(["openssl", "genpkey", "-algorithm", "ED25519", "-out", str(key_path)], check=True, capture_output=True)
        subprocess.run(["openssl", "pkey", "-in", str(key_path), "-pubout", "-out", str(public_key_path)], check=True, capture_output=True)
        payload_path.write_bytes(payload)
        digest_path.write_bytes(subprocess.run(["openssl", "dgst", "-sha256", "-binary", str(payload_path)], check=True, capture_output=True).stdout)
        subprocess.run(["openssl", "pkeyutl", "-sign", "-inkey", str(key_path), "-rawin", "-in", str(digest_path), "-out", str(signature_path)],
                       check=True, capture_output=True)
        signature = base64.b64encode(signature_path.read_bytes()).decode("ascii")
    except (OSError, subprocess.SubprocessError) as failure:
        return None, f"生成临时签名密钥失败：{failure}", False
    shutil.copyfile(public_key_path, session.temp_root / "server-firmware-keys" / "e2e-firmware.pem")
    shutil.copyfile(public_key_path, session.temp_root / "agent-firmware-keys" / "e2e-firmware.pem")
    return FirmwareSigning("e2e-firmware", payload, signature), "", True


def login(label: str = "管理员登录") -> str | None:
    status, captcha = request_json("GET", HTTP_BASE + "/api/auth/captcha")
    code = (captcha.get("data") or {}).get("code")
    report("8080 验证码签发", status == 200 and bool(code), f"HTTP {status}")
    if not code:
        return None
    status, login_payload = request_json(
            "POST", HTTP_BASE + "/api/auth/login",
            {"username": "admin", "password": "admin123", "captcha": code, "captchaUuid": captcha["data"]["uuid"]})
    token = (login_payload.get("data") or {}).get("token", "")
    if token:
        remember_secret(token)
        admin_tokens.append(token)
    report(label, status == 200 and len(token) == 64, f"HTTP {status}")
    return token or None


def create_enrollment_token(token: str, action: str) -> tuple[int | None, str]:
    status, created = request_json("POST", HTTP_BASE + "/api/enrollment-tokens",
                                    {"action": action, "expiresInSeconds": 3600, "nodeName": AGENT_NAME}, token=token)
    data = created.get("data") or {}
    token_id = data.get("id")
    plaintext = str(data.get("token", ""))
    remember_secret(plaintext)
    return (token_id if status == 200 else None), plaintext


def wait_registration(session: Session, timeout: float = 60.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if session.agent_process is not None and session.agent_process.poll() is not None:
            return False
        text = read_agent_state(session.agent_database, "node_id")
        if text and text.isdigit() and int(text) > 0:
            session.node_id = int(text)
            return True
        time.sleep(0.5)
    return False


def node_detail(token: str, node_id: int) -> dict:
    status, payload = request_json("GET", HTTP_BASE + f"/api/nodes/{node_id}", token=token)
    return (payload.get("data") or {}) if status == 200 else {}


def wait_node_detail(token: str, node_id: int, predicate, timeout: float, interval: float = 0.5) -> dict | None:
    deadline = time.time() + timeout
    last: dict = {}
    while time.time() < deadline:
        last = node_detail(token, node_id)
        if predicate(last):
            return last
        time.sleep(interval)
    return None


def wait_command_terminal(token: str, node_id: int, command_id: int, timeout: float) -> dict | None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        status, payload = request_json("GET", HTTP_BASE + f"/api/commands?pageNum=1&pageSize=100&nodeId={node_id}", token=token)
        if status == 200:
            for row in (payload.get("data") or {}).get("rows", []):
                if row.get("id") == command_id and row.get("status") in ("succeeded", "failed", "timeout", "cancelled"):
                    return row
        time.sleep(0.5)
    return None


def create_command(token: str, node_id: int, command_type: str, payload: dict, timeout_seconds: int = 120) -> tuple[int | None, int]:
    status, created = request_json("POST", HTTP_BASE + "/api/commands",
                                   {"nodeId": node_id, "type": command_type, "payload": json.dumps(payload, ensure_ascii=False),
                                    "timeoutSeconds": timeout_seconds}, token=token)
    data = created.get("data") or {}
    return (int(data["id"]) if status == 200 and data.get("id") else None), status


def wait_telemetry(token: str, node_id: int, timeout: float = 45.0) -> dict[str, dict] | None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        status, payload = request_json("GET", HTTP_BASE + f"/api/telemetry/latest?nodeId={node_id}", token=token)
        if status == 200:
            points = {point.get("metric"): point for point in payload.get("data") or []}
            if all(metric in points for metric in TELEMETRY_METRICS):
                return points
        time.sleep(0.5)
    return None


def wait_queue_depth(session: Session, minimum_telemetry: int, timeout: float) -> tuple[int, int] | None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        depths = queue_depths(session.agent_database)
        if depths is not None and depths[0] >= minimum_telemetry:
            return depths
        time.sleep(0.5)
    return None


def wait_queue_drained(session: Session, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        depths = queue_depths(session.agent_database)
        if depths is not None and depths[0] == 0:
            return True
        time.sleep(0.5)
    return False


def wait_heartbeat_rejection(session: Session, offsets: tuple[int, int], timeout: float) -> str:
    """只看吊销之后新增的日志，并要求同一行同时出现心跳动作与 HTTP 403，避免把别的 403 当证据。"""
    pattern = re.compile(r"心跳[^\r\n]*HTTP 403")
    deadline = time.time() + timeout
    while time.time() < deadline:
        for path, offset in zip((session.agent_log, session.agent_stdout), offsets):
            try:
                with open(path, "rb") as stream:
                    stream.seek(offset)
                    appended = stream.read().decode("utf-8", "replace")
            except OSError:
                continue
            match = pattern.search(appended)
            if match:
                return match.group(0)
        time.sleep(0.5)
    return ""


def upload_signed_firmware(token: str, signing: FirmwareSigning, version: str) -> dict | None:
    query = urllib.parse.urlencode({"name": "EdgePulse Agent E2E", "version": version, "fileName": "agent-e2e.bin"})
    connection = http.client.HTTPConnection("127.0.0.1", 8080, timeout=60)
    try:
        connection.putrequest("POST", f"/api/firmware?{query}")
        connection.putheader("Authorization", "Bearer " + token)
        connection.putheader("Content-Type", "application/octet-stream")
        connection.putheader("Content-Length", str(len(signing.payload)))
        connection.putheader("X-Firmware-Signature", signing.signature)
        connection.putheader("X-Firmware-Key-Id", signing.key_id)
        connection.endheaders()
        connection.send(signing.payload)
        response = connection.getresponse()
        body = response.read()
        if response.status != 200:
            return None
        return json.loads(body).get("data") or {}
    except (OSError, json.JSONDecodeError):
        return None
    finally:
        connection.close()


def wait_firmware_task(token: str, firmware_id: int, node_id: int, status: str, timeout: float) -> dict | None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        code, payload = request_json("GET", HTTP_BASE + f"/api/firmware/{firmware_id}/tasks", token=token)
        if code == 200:
            for task in payload.get("data") or []:
                if task.get("nodeId") == node_id and task.get("status") == status:
                    return task
        time.sleep(0.5)
    return None


def generate_rebind_certificate(session: Session) -> tuple[Path, Path] | None:
    """用开发 CA 在临时目录签发一张新的客户端证书，不触碰仓库里的证书文件。"""
    if shutil.which("openssl") is None:
        return None
    directory = session.temp_root / "rebind-certs"
    directory.mkdir(parents=True, exist_ok=True)
    key_path = directory / "agent-key.pem"
    request_path = directory / "agent.csr"
    certificate_path = directory / "agent.pem"
    extension_path = directory / "agent.ext"
    serial_path = directory / "ca.srl"
    extension_path.write_text("extendedKeyUsage=clientAuth\n", encoding="ascii")
    try:
        subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(key_path)], check=True, capture_output=True)
        # Linux 上 Agent 会拒绝 group/world 可读的私钥，临时证书也必须收紧到 0600
        os.chmod(key_path, 0o600)
        subprocess.run(["openssl", "req", "-new", "-key", str(key_path), "-subj", f"/CN=edgepulse-agent-e2e-rebind-{RUN_ID}", "-out", str(request_path)],
                       check=True, capture_output=True)
        subprocess.run(["openssl", "x509", "-req", "-in", str(request_path), "-CA", str(CERTS / "ca.pem"), "-CAkey", str(CERTS / "ca-key.pem"),
                        "-CAserial", str(serial_path), "-CAcreateserial", "-days", "2", "-sha256", "-extfile", str(extension_path), "-out", str(certificate_path)],
                       check=True, capture_output=True)
    except (OSError, subprocess.SubprocessError):
        return None
    return certificate_path, key_path


def check_tokens_not_leaked(session: Session, enrollment_token: str, rebind_token: str) -> None:
    needles = [("管理员令牌", token.encode()) for token in admin_tokens if token]
    needles += [("注册令牌", enrollment_token.encode()), ("改绑令牌", rebind_token.encode()), ("运维令牌", session.ops_token.encode())]
    needles += [("PEM 私钥", marker) for marker in (b"BEGIN PRIVATE KEY", b"BEGIN OPENSSH PRIVATE KEY", b"BEGIN EC PRIVATE KEY")]
    logs = sorted(path for path in (session.temp_root / "logs").glob("*") if path.is_file())
    databases = [session.agent_database, session.server_database]
    missing = [path.name for path in (session.agent_log, session.agent_stdout, session.server_log, *databases) if not path.exists()]
    scanned = logs + [path for database in databases for path in database_family(database) if path.exists()]
    findings: list[str] = ["缺失文件：" + "、".join(missing)] if missing else []
    for path in scanned:
        try:
            content = path.read_bytes()
        except OSError as failure:
            findings.append(f"{path.name}（无法读取：{failure}）")
            continue
        for label, needle in needles:
            if needle and needle in content:
                findings.append(f"{path.name}（{label}）")
                break
    report("令牌与私钥未出现在日志或状态库", not findings,
           "命中：" + "、".join(findings) if findings else "已扫描 " + "、".join(path.name for path in scanned))


def database_family(database: Path) -> list[Path]:
    """主库必须存在；WAL/SHM 与数据一样可能残留令牌，存在就要一起扫。"""
    paths = [database]
    for suffix in ("-wal", "-shm"):
        sidecar = database.with_name(database.name + suffix)
        if sidecar.exists():
            paths.append(sidecar)
    return paths


def verify_cleanup(session: Session) -> None:
    leftovers: list[str] = []
    busy = [str(port) for port in SERVER_PORTS if not port_is_free(port)]
    if busy:
        leftovers.append("端口仍被占用 " + ", ".join(busy))
    alive = [str(pid) for pid in session.started_pids if process_alive(pid)]
    if alive:
        leftovers.append("进程仍在运行 " + ", ".join(alive))
    if session.temp_root.exists():
        leftovers.append("临时目录未删除 " + str(session.temp_root))
    report("清理后无残留进程、端口与临时目录", not leftovers,
           "；".join(leftovers) if leftovers else f"{len(session.started_pids)} 个进程已退出，端口全部空闲，临时目录已删除")


def run(session: Session) -> None:
    if not (ROOT / "build" / "debug" / "bin" / "edgepulse-agent-test-hook.exe").exists() and not (
            ROOT / "build" / "debug" / "bin" / "edgepulse-agent-test-hook").exists():
        report("测试钩子存在", False, "build/debug/bin/edgepulse-agent-test-hook 缺失，无法验证固件安装")
        return
    report("测试钩子存在", True)

    signed_firmware_available = True
    signing, signing_detail, signing_skippable = prepare_firmware_signing(session)
    if signing is None:
        if signing_skippable:
            skip("签名固件上传与安装", signing_detail)
            signed_firmware_available = False
        else:
            report("签名固件密钥准备", False, signing_detail)
            signed_firmware_available = False

    agent_binary = executable(ROOT / "build" / "debug" / "bin" / "edgepulse-agent")
    if not agent_binary.exists():
        report("Agent 配置校验", False, f"找不到 Agent 二进制：{agent_binary}")
        return

    session.write_server_config()
    session.write_agent_config(CERTS / "agent.pem", CERTS / "agent-key.pem")
    configuration_check = subprocess.run([str(agent_binary), "--check-config", "--config", str(session.agent_config)], capture_output=True, text=True,
                                         encoding="utf-8", errors="replace", check=False)
    report("Agent 配置校验", configuration_check.returncode == 0,
           (configuration_check.stdout or configuration_check.stderr).strip() or f"退出码 {configuration_check.returncode}")
    if configuration_check.returncode != 0:
        return

    version_result = subprocess.run([str(agent_binary), "--version"], capture_output=True, text=True, encoding="utf-8", errors="replace", check=False)
    fingerprint_result = subprocess.run([str(agent_binary), "--print-fingerprint", "--config", str(session.agent_config)], capture_output=True, text=True,
                                        encoding="utf-8", errors="replace", check=False)
    session.agent_version = version_result.stdout.strip().split()[-1] if version_result.returncode == 0 else ""
    session.agent_fingerprint = fingerprint_result.stdout.strip() if fingerprint_result.returncode == 0 else ""
    report("Agent 版本与证书指纹可读", bool(session.agent_version) and len(session.agent_fingerprint) == 64,
           f"版本 {session.agent_version or '未知'}，指纹 {session.agent_fingerprint or '未知'}")
    if not session.agent_version or len(session.agent_fingerprint) != 64:
        return

    if not session.start_server("服务端启动（临时库、enrollment=required）"):
        return
    token = login()
    if token is None:
        return

    status, _ = request_json("POST", AGENT_BASE + "/api/agent/register",
                             {"name": AGENT_NAME + "-no-token", "agentVersion": session.agent_version, "certFingerprint": session.agent_fingerprint,
                              "tenantCode": "default", "labels": "{}"},
                             context=client_context(CERTS / "agent.pem", CERTS / "agent-key.pem"))
    report("缺少注册令牌的注册被拒", status == 403, f"HTTP {status}")

    enrollment_token_id, enrollment_token = create_enrollment_token(token, "enroll")
    report("创建注册令牌", enrollment_token_id is not None and enrollment_token.startswith("ep_enroll_"), f"id={enrollment_token_id}")
    if not enrollment_token:
        return

    agent_environment = build_agent_environment({ENROLLMENT_ENV: enrollment_token, HOOK_MARKER_ENV: str(session.hook_marker)})
    if not session.start_agent(agent_environment):
        return
    if not wait_registration(session):
        report("Agent 注册", False, "60 秒内没有写入 node_id：" + read_log_tail(session.agent_stdout))
        return
    report("Agent 注册", True, f"nodeId={session.node_id}")

    registered = wait_node_detail(token, session.node_id, lambda node: node.get("status") == "online", 30)
    report("Agent 节点上线", registered is not None, f"nodeId={session.node_id}")
    if registered is None:
        return
    report("Agent 版本与证书指纹登记", registered.get("agentVersion") == session.agent_version and registered.get("certFingerprint") == session.agent_fingerprint,
           f"版本 {registered.get('agentVersion')}，指纹 {registered.get('certFingerprint')}")

    last_seen_before = str(registered.get("lastSeen", ""))
    heartbeat = wait_node_detail(token, session.node_id, lambda node: node.get("status") == "online" and node.get("lastSeen") != last_seen_before, 30)
    report("心跳持续刷新节点在线与 lastSeen", heartbeat is not None, f"{last_seen_before} -> {(heartbeat or {}).get('lastSeen')}")

    telemetry = wait_telemetry(token, session.node_id)
    telemetry_ok = False
    telemetry_values = ""
    if telemetry is not None:
        readings: list[str] = []
        telemetry_ok = True
        for metric in TELEMETRY_METRICS:
            point = telemetry[metric]
            value = point.get("value")
            unit = point.get("unit")
            readings.append(f"{metric}={value}{unit}")
            if not isinstance(value, (int, float)) or not 0 <= float(value) <= 100 or unit != "%":
                telemetry_ok = False
        telemetry_values = "，".join(readings)
    report("遥测 cpu/memory/disk 上报", telemetry_ok, telemetry_values or "45 秒内没有收到三项指标")

    status, config_response = request_json("PUT", HTTP_BASE + f"/api/nodes/{session.node_id}/config",
                                           {"content": AGENT_CONFIG_CONTENT, "expectedVersion": 0, "note": "agent e2e"}, token=token)
    desired_version = int(((config_response.get("data") or {}).get("desired") or {}).get("version") or 0)
    report("配置下发版本 1", status == 200 and desired_version == 1, f"HTTP {status}，desired={desired_version}")
    config_command_id, command_status = create_command(token, session.node_id, "config.apply", {"version": desired_version})
    report("config.apply 命令创建", config_command_id is not None, f"HTTP {command_status}，commandId={config_command_id}")
    config_command = wait_command_terminal(token, session.node_id, config_command_id, 60) if config_command_id else None
    report("config.apply 执行成功", config_command is not None and config_command.get("status") == "succeeded",
           (config_command or {}).get("message", "未在 60 秒内结束"))
    applied_file = session.temp_root / "agent-config.json"
    applied_ok = False
    applied_detail = "Agent 未写出配置文件"
    if applied_file.exists():
        try:
            applied_ok = json.loads(applied_file.read_text(encoding="utf-8")) == AGENT_CONFIG_CONTENT
            applied_detail = applied_file.read_text(encoding="utf-8").strip()[:200]
        except (OSError, json.JSONDecodeError) as failure:
            applied_detail = f"读取落盘配置失败：{failure}"
    report("Agent 配置落盘内容一致", applied_ok, applied_detail)
    synced = wait_node_detail(token, session.node_id, lambda node: node.get("configInSync") is True, 30)
    report("管理端看到配置 inSync", synced is not None and (synced or {}).get("reportedConfigVersion") == desired_version,
           f"reported={(synced or {}).get('reportedConfigVersion')}")

    diagnostics_id, diagnostics_status = create_command(token, session.node_id, "diagnostics.collect", {})
    report("diagnostics.collect 命令创建", diagnostics_id is not None, f"HTTP {diagnostics_status}，commandId={diagnostics_id}")
    diagnostics = wait_command_terminal(token, session.node_id, diagnostics_id, 60) if diagnostics_id else None
    diagnostics_result: dict = {}
    if diagnostics is not None and diagnostics.get("status") == "succeeded":
        try:
            parsed = json.loads(str(diagnostics.get("result", "{}")))
            diagnostics_result = parsed if isinstance(parsed, dict) else {}
        except json.JSONDecodeError:
            diagnostics_result = {}
    queue_depth = diagnostics_result.get("queueDepth") or {}
    diagnostics_ok = (diagnostics is not None and diagnostics.get("status") == "succeeded" and diagnostics_result.get("agentVersion") == session.agent_version
                      and diagnostics_result.get("nodeName") == AGENT_NAME and isinstance(queue_depth.get("telemetry"), int)
                      and isinstance(queue_depth.get("results"), int))
    report("诊断结果含版本、节点名与队列深度", diagnostics_ok,
           f"agentVersion={diagnostics_result.get('agentVersion')}，nodeName={diagnostics_result.get('nodeName')}，queueDepth={queue_depth}")

    if signed_firmware_available and signing is not None:
        firmware_version = f"9.{RUN_ID}"
        firmware = upload_signed_firmware(token, signing, firmware_version)
        report("签名固件上传", firmware is not None and firmware.get("signed") is True and firmware.get("signatureKeyId") == signing.key_id
               and firmware.get("sha256") == hashlib.sha256(signing.payload).hexdigest(),
               f"id={(firmware or {}).get('id')}，signed={(firmware or {}).get('signed')}")
        firmware_id = int(firmware["id"]) if firmware and firmware.get("id") else 0
        if firmware_id:
            deploy_status, deployed = request_json("POST", HTTP_BASE + f"/api/firmware/{firmware_id}/deploy", {"nodeIds": [session.node_id]}, token=token)
            results = deployed.get("data") or []
            command_id = int(results[0]["commandId"]) if deploy_status == 200 and results and results[0].get("commandId") else None
            report("签名固件下发命令创建", command_id is not None, f"HTTP {deploy_status}，commandId={command_id}")
            firmware_command = wait_command_terminal(token, session.node_id, command_id, 120) if command_id else None
            installed_task = wait_firmware_task(token, firmware_id, session.node_id, "installed", 30)
            report("签名固件经测试 hook 安装完成",
                   firmware_command is not None and firmware_command.get("status") == "succeeded" and installed_task is not None,
                   (firmware_command or {}).get("message", "命令未成功") + (f"，安装状态 {installed_task.get('status')}" if installed_task else "，未看到 installed 任务"))
            marker_lines: list[str] = []
            if session.hook_marker.exists():
                marker_lines = [line for line in session.hook_marker.read_text(encoding="utf-8", errors="replace").splitlines() if line]
            marker_ok = len(marker_lines) >= 3 and marker_lines[1] == firmware_version and marker_lines[2] == hashlib.sha256(signing.payload).hexdigest()
            report("安装 hook 收到文件、版本与 SHA-256", marker_ok,
                   "；".join(marker_lines[:3]) if marker_lines else "标记文件为空")
        else:
            report("签名固件下发命令创建", False, "上传未返回固件标识")

    telemetry_before = server_telemetry_total(session.server_database, session.node_id)
    last_seen_before_outage = str(node_detail(token, session.node_id).get("lastSeen", ""))
    node_id_before_restart = read_agent_state(session.agent_database, "node_id") or str(session.node_id)
    ports_released = session.stop_server()
    busy_ports = [str(port) for port in SERVER_PORTS if not port_is_free(port)]
    report("服务端已停止", ports_released and not busy_ports,
           "用于构造离线遥测" if not busy_ports else "停止后端口仍被占用：" + ", ".join(busy_ports))
    queued = wait_queue_depth(session, 3, 60)
    report("离线期间遥测写入本地队列", queued is not None and queued[0] >= 3,
           f"telemetry_queue={(queued or (None, None))[0]}" if queued else "60 秒内没有队列行")
    queued_rows = queued[0] if queued else 0

    # 服务端离线时重启 Agent：注册会按退避重试，本地队列必须跨进程保留并在恢复后补报
    session.stop_agent()
    queued_samples = queued_telemetry_samples(session.agent_database)
    report("离线队列样本逐条取样", bool(queued_samples),
           f"{len(queued_samples)} 条：" + format_telemetry_samples(queued_samples) if queued_samples else "队列里读不到可解析样本")
    restart_environment = build_agent_environment({HOOK_MARKER_ENV: str(session.hook_marker)})
    if not session.start_agent(restart_environment):
        return
    report("服务端离线期间重启 Agent", session.agent_process is not None and session.agent_process.poll() is None, "Agent 进程已启动并在等待注册")

    if not session.start_server("服务端重启（同一临时库）"):
        return
    # 会话存放在进程内存里，服务端重启后旧管理员令牌已失效，必须重新登录
    token = login("服务端重启后管理员重新登录")
    if token is None:
        return
    drained = wait_queue_drained(session, 90)
    report("恢复后遥测队列补报清零", drained, "队列已清零" if drained else "90 秒内 queue 未清零")
    deadline = time.time() + 30
    recovered_total = telemetry_before
    while time.time() < deadline:
        recovered_total = server_telemetry_total(session.server_database, session.node_id)
        if drained and recovered_total >= telemetry_before + queued_rows:
            break
        time.sleep(0.5)
    report("服务端收到补报遥测", drained and recovered_total >= telemetry_before + queued_rows,
           f"before={telemetry_before}，queued={queued_rows}，after={recovered_total}")
    missing_samples = queued_samples
    deadline = time.time() + 30
    while time.time() < deadline and missing_samples:
        missing_samples = missing_telemetry_samples(queued_samples, server_telemetry_samples(session.server_database, session.node_id))
        if missing_samples:
            time.sleep(0.5)
    if not queued_samples:
        exact_detail = "离线队列没有可核对的样本"
    elif missing_samples:
        exact_detail = "缺失 " + format_telemetry_samples(missing_samples)
    else:
        exact_detail = f"队列 {len(queued_samples)} 条按 metric+collectedAt+value 全部命中"
    report("离线队列样本逐条落在服务端遥测", bool(queued_samples) and not missing_samples, exact_detail)
    recovered_node = wait_node_detail(token, session.node_id,
                                      lambda node: node.get("status") == "online" and node.get("lastSeen") != last_seen_before_outage, 60)
    state_after_restart = wait_agent_state(session.agent_database, "node_id", node_id_before_restart, 10)
    report("Agent 重启后以原节点标识上线", recovered_node is not None and state_after_restart == node_id_before_restart,
           f"nodeId {node_id_before_restart} -> {state_after_restart}")
    report("恢复后心跳继续刷新", recovered_node is not None, f"{last_seen_before_outage} -> {(recovered_node or {}).get('lastSeen')}")

    if recovered_node is None:
        report("重启后命令通道恢复", False, "节点未重新上线：" + read_log_tail(session.agent_stdout))
    else:
        after_restart_command, after_restart_status = create_command(token, session.node_id, "diagnostics.collect", {}, timeout_seconds=90)
        after_restart = wait_command_terminal(token, session.node_id, after_restart_command, 90) if after_restart_command else None
        report("重启后命令通道恢复", after_restart is not None and after_restart.get("status") == "succeeded",
               f"HTTP {after_restart_status}，commandId={after_restart_command}")
        deadline = time.time() + 30
        resumed_total = recovered_total
        while time.time() < deadline:
            resumed_total = server_telemetry_total(session.server_database, session.node_id)
            if resumed_total > recovered_total:
                break
            time.sleep(0.5)
        report("重启后遥测继续上报", resumed_total > recovered_total, f"{recovered_total} -> {resumed_total}")

    duplicate_groups = server_telemetry_duplicate_groups(session.server_database, session.node_id)
    report("补报遥测无重复行", duplicate_groups == 0, f"重复的 metric/collectedAt 分组 {duplicate_groups}")

    rebind_id, rebind_token = create_enrollment_token(token, "rebind")
    report("创建改绑令牌", rebind_id is not None and rebind_token.startswith("ep_enroll_"), f"id={rebind_id}")
    agent_log_offsets = session.log_offsets()
    revoke_status, revoked = request_json("POST", HTTP_BASE + f"/api/nodes/{session.node_id}/certificate/revoke", {}, token=token)
    revoked_data = revoked.get("data") or {}
    report("吊销节点证书", revoke_status == 200 and not revoked_data.get("certFingerprint") and bool(revoked_data.get("certRevokedAt")),
           f"HTTP {revoke_status}，revokedAt={revoked_data.get('certRevokedAt')}")
    old_context = client_context(CERTS / "agent.pem", CERTS / "agent-key.pem")
    status, _ = request_json("POST", AGENT_BASE + "/api/agent/heartbeat", {"nodeId": session.node_id, "agentVersion": session.agent_version},
                             context=old_context)
    report("吊销后旧证书心跳被拒", status == 403, f"HTTP {status}")
    rejection_line = wait_heartbeat_rejection(session, agent_log_offsets, 25)
    report("运行中的 Agent 收到吊销拒绝", bool(rejection_line), ("Agent 日志：" + rejection_line) if rejection_line else "新增日志未出现心跳 HTTP 403")
    offline = wait_node_detail(token, session.node_id, lambda node: node.get("status") == "offline", 30)
    report("吊销后节点离线", offline is not None, f"status={(offline or {}).get('status')}")

    new_certificate = generate_rebind_certificate(session)
    if rebind_token and new_certificate is not None:
        new_certificate_path, new_key_path = new_certificate
        new_fingerprint = certificate_fingerprint(new_certificate_path)
        report("新客户端证书签发", new_fingerprint != session.agent_fingerprint, f"指纹 {new_fingerprint}")
        session.stop_agent()
        session.write_agent_config(new_certificate_path, new_key_path)
        rebind_environment = build_agent_environment({REBIND_ENV: rebind_token, HOOK_MARKER_ENV: str(session.hook_marker)})
        if session.start_agent(rebind_environment):
            rebound = wait_node_detail(token, session.node_id,
                                       lambda node: node.get("status") == "online" and node.get("certFingerprint") == new_fingerprint
                                       and not node.get("certRevokedAt"), 60)
            state_after_rebind = wait_agent_state(session.agent_database, "node_id", node_id_before_restart, 10)
            report("改绑令牌注册恢复在线", rebound is not None and state_after_rebind == node_id_before_restart,
                   f"nodeId={state_after_rebind}，fingerprint={(rebound or {}).get('certFingerprint')}")
        else:
            report("改绑令牌注册恢复在线", False, "新证书 Agent 未启动")
    else:
        skip("证书吊销后的改绑注册", "未取得改绑令牌或无法用开发 CA 签发新证书")

    check_tokens_not_leaked(session, enrollment_token, rebind_token)


def main() -> int:
    certificate_error = ensure_dev_certificates()
    if certificate_error:
        report("开发证书就绪", False, certificate_error)
        return 1
    report("开发证书就绪", True, str(CERTS))
    busy_ports = [port for port in SERVER_PORTS if not port_is_free(port)]
    if busy_ports:
        report("8080/8443/9100/9443 端口空闲", False, "端口已被占用，E2E 不会复用或停止已有服务：" + ", ".join(str(port) for port in busy_ports))
        return 1
    report("8080/8443/9100/9443 端口空闲", True)

    temp_root = Path(tempfile.mkdtemp(prefix="edgepulse-agent-e2e-"))
    session = Session(temp_root)
    print(f"临时目录：{temp_root}")
    try:
        try:
            run(session)
        except Exception as failure:  # noqa: BLE001
            traceback.print_exc()
            report("端到端执行未抛出异常", False, f"{type(failure).__name__}: {failure}")
    finally:
        try:
            session.close()
        finally:
            try:
                verify_cleanup(session)
            except Exception as failure:  # noqa: BLE001
                report("清理后无残留进程、端口与临时目录", False, f"清理检查异常：{type(failure).__name__}: {failure}")
    print()
    print(f"Agent 端到端验收：通过 {passed} 项，失败 {len(failures)} 项，跳过 {len(skips)} 项")
    if failures:
        print("失败项：" + "、".join(failures))
        return 1
    if skips:
        print("验收通过，跳过项不计入通过：" + "、".join(skips))
    else:
        print("Agent 端到端验收全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
