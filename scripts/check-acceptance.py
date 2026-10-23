#!/usr/bin/env python3
"""全量验收：启动开发服务，逐项验证认证、系统管理、节点、遥测告警、固件、命令、WebSocket、h3、mTLS 与运维口。"""

from __future__ import annotations

import base64
import hashlib
import hmac
import http.client
import json
import os
import queue
import shutil
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

ROOT = Path(__file__).resolve().parents[1]
CERTS = ROOT / "var" / "certs"
OPS_TOKEN_FILE = ROOT / "var" / "ops-token"
HTTP_BASE = "http://127.0.0.1:8080"
HTTPS_BASE = "https://127.0.0.1:8443"
AGENT_BASE = "https://127.0.0.1:9443"
OPS_BASE = "http://127.0.0.1:9100"
RUN_ID = str(time.time_ns())[-8:]
# 开发档 heartbeat_timeout_seconds=30、offline_sweep_seconds=5，再留 10 秒给扫描周期与落库，上限 45 秒
OFFLINE_WAIT_SECONDS = 30 + 5 + 10
failures: list[str] = []
skips: list[str] = []
passed = 0
webhook_events: queue.Queue = queue.Queue()
mail_events: queue.Queue = queue.Queue()


class FirmwareSigning:
    """一次验收运行使用的临时固件签名材料，公钥放在服务启动时读取的信任键目录。"""

    def __init__(self, key_id: str, key_dir: Path, public_key_path: Path, temp_dir: Path, payload: bytes, signature: str,
                 created_key_dir: bool) -> None:
        self.key_id = key_id
        self.key_dir = key_dir
        self.public_key_path = public_key_path
        self.temp_dir = temp_dir
        self.payload = payload
        self.signature = signature
        self.created_key_dir = created_key_dir


class WebhookHandler(BaseHTTPRequestHandler):
    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("content-length", "0"))
        body = self.rfile.read(length)
        webhook_events.put((body, self.headers.get("x-edgepulse-signature", "")))
        self.send_response(204)
        self.end_headers()

    def log_message(self, *args: object) -> None:
        return


class SmtpHandler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        self.wfile.write(b"220 fake-smtp ready\r\n")
        in_data = False
        message = bytearray()
        while True:
            line = self.rfile.readline()
            if not line:
                break
            if in_data:
                if line in (b".\r\n", b".\n"):
                    in_data = False
                    mail_events.put(bytes(message))
                    self.wfile.write(b"250 queued\r\n")
                else:
                    message.extend(line)
                continue
            upper = line.upper()
            if upper.startswith(b"EHLO") or upper.startswith(b"HELO"):
                self.wfile.write(b"250-fake-smtp\r\n250 AUTH PLAIN\r\n")
            elif upper.startswith(b"AUTH"):
                self.wfile.write(b"235 authenticated\r\n")
            elif upper.startswith(b"STARTTLS"):
                self.wfile.write(b"220 ready for tls\r\n")
                self.wfile.flush()
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(str(CERTS / "server.pem"), str(CERTS / "server-key.pem"))
                self.connection = context.wrap_socket(self.connection, server_side=True)
                self.rfile = self.connection.makefile("rb", self.rbufsize)
                self.wfile = self.connection.makefile("wb", self.wbufsize)
            elif upper.startswith(b"DATA"):
                self.wfile.write(b"354 send data\r\n")
                in_data = True
            elif upper.startswith(b"QUIT"):
                self.wfile.write(b"221 bye\r\n")
                break
            elif upper.startswith((b"MAIL", b"RCPT")):
                self.wfile.write(b"250 ok\r\n")
            else:
                self.wfile.write(b"250 ok\r\n")

    def log_message(self, *args: object) -> None:
        return


class SmtpServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


def start_smtp_receiver() -> tuple[SmtpServer, threading.Thread]:
    server = SmtpServer(("127.0.0.1", 0), SmtpHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


def start_webhook_receiver() -> tuple[ThreadingHTTPServer, threading.Thread]:
    server = ThreadingHTTPServer(("127.0.0.1", 0), WebhookHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread



def report(name: str, ok: bool, detail: str = "") -> None:
    global passed
    print(("PASS " if ok else "FAIL ") + name + (("：" + detail) if detail else ""))
    if ok:
        passed += 1
    else:
        failures.append(name)


def skip(name: str, detail: str = "") -> None:
    """记录本次无法构造的检查；跳过不是通过，也不让退出码变成失败。"""
    print("SKIP " + name + (("：" + detail) if detail else ""))
    skips.append(name)


def prepare_firmware_signing() -> tuple[FirmwareSigning | None, str]:
    """服务由脚本自己启动时生成本次运行专用的 Ed25519 密钥，并在启动前放入信任键目录。"""
    if shutil.which("openssl") is None:
        return None, "缺少 openssl，无法生成临时签名密钥"
    key_dir = ROOT / "config" / "firmware-keys"
    public_key_path = key_dir / "acceptance.pem"
    if public_key_path.exists():
        return None, f"{public_key_path.relative_to(ROOT)} 已存在，拒绝覆盖"
    created_key_dir = False
    temp_dir: Path | None = None
    try:
        created_key_dir = not key_dir.exists()
        key_dir.mkdir(parents=True, exist_ok=True)
        temp_dir = Path(tempfile.mkdtemp(prefix="acceptance-signing-", dir=str(ROOT / "var")))
        key_path = temp_dir / "acceptance-key.pem"
        digest_path = temp_dir / "firmware-digest.bin"
        signature_path = temp_dir / "firmware-signature.bin"
        payload = b"EdgePulse acceptance signed firmware\n" * 2048
        payload_path = temp_dir / "signed-firmware.bin"
        payload_path.write_bytes(payload)
        subprocess.run(["openssl", "genpkey", "-algorithm", "ED25519", "-out", str(key_path)], check=True, capture_output=True)
        subprocess.run(["openssl", "pkey", "-in", str(key_path), "-pubout", "-out", str(public_key_path)], check=True, capture_output=True)
        digest_path.write_bytes(subprocess.run(["openssl", "dgst", "-sha256", "-binary", str(payload_path)], check=True, capture_output=True).stdout)
        subprocess.run(["openssl", "pkeyutl", "-sign", "-inkey", str(key_path), "-rawin", "-in", str(digest_path), "-out", str(signature_path)],
                       check=True, capture_output=True)
        signature = base64.b64encode(signature_path.read_bytes()).decode("ascii")
    except (OSError, subprocess.SubprocessError) as failure:
        if temp_dir is not None:
            shutil.rmtree(temp_dir, ignore_errors=True)
        public_key_path.unlink(missing_ok=True)
        if created_key_dir:
            try:
                key_dir.rmdir()
            except OSError:
                pass
        return None, f"生成签名密钥失败：{failure}"
    return FirmwareSigning("acceptance", key_dir, public_key_path, temp_dir, payload, signature, created_key_dir), ""


def cleanup_firmware_signing(signing: FirmwareSigning | None) -> None:
    if signing is None:
        return
    shutil.rmtree(signing.temp_dir, ignore_errors=True)
    signing.public_key_path.unlink(missing_ok=True)
    if signing.created_key_dir:
        try:
            signing.key_dir.rmdir()
        except OSError:
            # 目录里还有别的公钥时保持原样，只清理本次写入的 acceptance.pem
            pass


def request_json(method: str, url: str, payload: dict | None = None, token: str | None = None, context: ssl.SSLContext | None = None,
                 timeout: float = 30.0, headers_extra: dict | None = None) -> tuple[int, dict, dict]:
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {"Content-Type": "application/json"} if data is not None else {}
    headers.update(headers_extra or {})
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
            return response.status, parsed, dict(response.headers)
    except urllib.error.HTTPError as failure:
        body = failure.read().decode("utf-8", "replace")
        try:
            parsed = json.loads(body) if body else {}
        except json.JSONDecodeError:
            parsed = {}
        return failure.code, parsed, dict(failure.headers)


def client_context(client_cert: Path | None = None, client_key: Path | None = None) -> ssl.SSLContext:
    context = ssl.create_default_context(cafile=str(CERTS / "ca.pem"))
    if client_cert is not None and client_key is not None:
        context.load_cert_chain(certfile=str(client_cert), keyfile=str(client_key))
    return context


def agent_context() -> ssl.SSLContext:
    return client_context(CERTS / "agent.pem", CERTS / "agent-key.pem")


def wait_for_health(process: subprocess.Popen[bytes] | None, timeout_seconds: float = 40.0) -> bool:
    deadline = time.time() + timeout_seconds
    while time.time() < deadline:
        try:
            status, payload, _ = request_json("GET", HTTP_BASE + "/api/health", timeout=2.0)
            if status == 200 and payload.get("data", {}).get("status") == "UP":
                return True
        except (urllib.error.URLError, TimeoutError, ConnectionError):
            pass
        if process is not None and process.poll() is not None:
            return False
        time.sleep(0.5)
    return False


def executable(base: Path) -> Path:
    candidate = base.with_suffix(".exe")
    return candidate if candidate.exists() else base


def stop_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(process.pid)], capture_output=True, check=False)
    else:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()


def check_authentication() -> str | None:
    status, captcha, _ = request_json("GET", HTTP_BASE + "/api/auth/captcha")
    code = captcha.get("data", {}).get("code")
    report("8080 验证码签发", status == 200 and bool(captcha.get("data", {}).get("uuid")) and bool(captcha.get("data", {}).get("imageSvg")), f"HTTP {status}")
    if not code:
        report("开发档验证码暴露 code", False, "缺少 code")
        return None
    status, login, headers = request_json(
        "POST", HTTP_BASE + "/api/auth/login",
        {"username": "admin", "password": "admin123", "captcha": code, "captchaUuid": captcha["data"]["uuid"]},
    )
    token = login.get("data", {}).get("token", "")
    report("管理员登录", status == 200 and len(token) == 64, f"HTTP {status}")
    report("响应回写 x-request-id", bool(headers.get("x-request-id")), str(headers.get("x-request-id", "")))
    if not token:
        return None
    status, me, _ = request_json("GET", HTTP_BASE + "/api/auth/me", token=token)
    permissions = me.get("data", {}).get("permissions", [])
    report("me 返回角色与权限", status == 200 and "device:node:list" in permissions and "device:node:write" in permissions, f"HTTP {status}")
    status, routes, _ = request_json("GET", HTTP_BASE + "/api/auth/routes", token=token)
    tree = routes.get("data", {}).get("routes", [])
    device = next((node for node in tree if node.get("path") == "/device"), None)
    component = ""
    if device and device.get("children"):
        component = device["children"][0].get("component", "")
    report("routes 返回设备菜单", status == 200 and component == "device/Nodes", component)
    return token


def check_system(token: str) -> None:
    for path, label in [("/api/system/users?pageNum=1&pageSize=10", "用户"), ("/api/system/roles", "角色"),
                        ("/api/system/menus/tree", "菜单"), ("/api/system/depts/tree", "部门"),
                        ("/api/system/posts", "岗位"), ("/api/system/dict/types", "字典"),
                        ("/api/system/configs", "参数"), ("/api/monitor/oper-logs/stats", "操作统计"),
                        ("/api/monitor/login-logs/stats", "登录统计"), ("/api/dashboard/summary?range=24h", "工作台")]:
        status, payload, _ = request_json("GET", HTTP_BASE + path, token=token)
        report(f"系统管理：{label}接口", status == 200 and payload.get("code") == 200, f"HTTP {status}")

    role_code = f"accept_role_{RUN_ID}"
    status, role, _ = request_json("POST", HTTP_BASE + "/api/system/roles",
                                   {"name": "验收只读角色", "code": role_code, "dataScope": "SELF", "sort": 99, "status": 1}, token=token)
    role_id = role.get("data", {}).get("id")
    username = f"accept_user_{RUN_ID}"
    status_user, user, _ = request_json("POST", HTTP_BASE + "/api/system/users",
                                        {"username": username, "password": "accept123", "nickname": "验收用户", "deptId": 101, "status": 1,
                                         "roleIds": [role_id] if role_id else [], "postIds": [101]}, token=token)
    report("系统管理：创建角色与用户", status == 200 and status_user == 200 and bool(user.get("data", {}).get("id")), f"HTTP {status}/{status_user}")
    if user.get("data", {}).get("id"):
        low = login_user(username, "accept123")
        if low:
            denied = request_json("GET", HTTP_BASE + "/api/system/users", token=low)[0]
            report("低权限账号访问用户管理返回 403", denied == 403, f"HTTP {denied}")
        request_json("DELETE", HTTP_BASE + f"/api/system/users/{user['data']['id']}", token=token)
    if role_id:
        request_json("DELETE", HTTP_BASE + f"/api/system/roles/{role_id}", token=token)


def login_user(username: str, password: str, tenant_code: str | None = None) -> str | None:
    status, captcha, _ = request_json("GET", HTTP_BASE + "/api/auth/captcha")
    code = captcha.get("data", {}).get("code")
    if status != 200 or not code:
        return None
    payload = {"username": username, "password": password, "captcha": code, "captchaUuid": captcha["data"]["uuid"]}
    if tenant_code:
        payload["tenantCode"] = tenant_code
    status, login, _ = request_json("POST", HTTP_BASE + "/api/auth/login", payload)
    return login.get("data", {}).get("token") if status == 200 else None


def check_tenancy(token: str) -> None:
    tenant_code = f"accept{RUN_ID}"
    admin_name = f"tenant_admin_{RUN_ID}"
    status, created, _ = request_json("POST", HTTP_BASE + "/api/system/tenants",
                                      {"code": tenant_code, "name": f"验收租户{RUN_ID}", "remark": "验收脚本",
                                       "adminUsername": admin_name, "adminNickname": "验收租户管理员", "adminPassword": "tenant123"}, token=token)
    tenant_id = created.get("data", {}).get("tenantId")
    report("租户创建", status == 200 and bool(tenant_id), f"HTTP {status}")
    if not tenant_id:
        return
    status, tenants, _ = request_json("GET", HTTP_BASE + f"/api/system/tenants?pageNum=1&pageSize=10&keyword={tenant_code}", token=token)
    report("租户列表包含新租户", status == 200 and tenants.get("data", {}).get("total") == 1, f"HTTP {status}")

    tenant_token = login_user(admin_name, "tenant123", tenant_code)
    report("租户管理员按租户编码登录", bool(tenant_token))
    if not tenant_token:
        return
    status, me, _ = request_json("GET", HTTP_BASE + "/api/auth/me", token=tenant_token)
    identity = me.get("data", {})
    report("会话携带租户身份", status == 200 and identity.get("tenantCode") == tenant_code and identity.get("platformAdmin") is False, f"HTTP {status}")
    status, users, _ = request_json("GET", HTTP_BASE + f"/api/system/users?pageNum=1&pageSize=10&keyword={admin_name}", token=tenant_token)
    rows = users.get("data", {}).get("rows", [])
    report("租户管理员只见本租户账号", status == 200 and users.get("data", {}).get("total") == 1 and rows and rows[0].get("tenantCode") == tenant_code,
           f"HTTP {status}")
    denied = request_json("POST", HTTP_BASE + "/api/system/tenants", {"code": "nope", "name": "无权限", "adminUsername": "nope", "adminPassword": "nope123"}, token=tenant_token)[0]
    report("租户管理员不能管理租户", denied == 403, f"HTTP {denied}")
    status, global_users, _ = request_json("GET", HTTP_BASE + f"/api/system/users?pageNum=1&pageSize=10&keyword={admin_name}", token=token)
    report("平台管理员可跨租户查账号", status == 200 and global_users.get("data", {}).get("total") == 1, f"HTTP {status}")

    status, _, _ = request_json("PUT", HTTP_BASE + f"/api/system/tenants/{tenant_id}", {"status": 0}, token=token)
    report("租户停用", status == 200, f"HTTP {status}")
    report("停用租户拒绝新登录", login_user(admin_name, "tenant123", tenant_code) is None)
    old_session = request_json("GET", HTTP_BASE + "/api/auth/me", token=tenant_token)[0]
    report("停用租户旧会话失效", old_session == 401, f"HTTP {old_session}")


def create_node(token: str, name: str) -> int | None:
    status, node, _ = request_json("POST", HTTP_BASE + "/api/nodes", {"name": name, "labels": {"acceptance": "true"}, "groupId": 200}, token=token)
    return node.get("data", {}).get("id") if status == 200 else None


def check_nodes(token: str) -> None:
    node_name = f"accept-node-{RUN_ID}"
    node_id = create_node(token, node_name)
    report("节点创建", node_id is not None, str(node_id))
    status, listing, _ = request_json("GET", HTTP_BASE + "/api/nodes?pageNum=1&pageSize=100", token=token)
    names = [row.get("name") for row in listing.get("data", {}).get("rows", [])]
    report("节点列表包含新节点", status == 200 and node_name in names, f"HTTP {status}")
    status, groups, _ = request_json("GET", HTTP_BASE + "/api/node-groups", token=token)
    report("节点分组可读", status == 200 and isinstance(groups.get("data"), list), f"HTTP {status}")
    return node_id


def check_notifications(token: str) -> tuple[ThreadingHTTPServer, threading.Thread, int, str, SmtpServer, threading.Thread, int]:
    server, thread = start_webhook_receiver()
    smtp, smtp_thread = start_smtp_receiver()
    secret = "accept-webhook-secret"
    status, payload, _ = request_json("POST", HTTP_BASE + "/api/system/notifications",
                                      {"name": "验收 Webhook", "type": "webhook",
                                       "endpoint": f"http://127.0.0.1:{server.server_port}/hook",
                                       "secret": secret, "minLevel": "warning", "enabled": True}, token=token)
    channel_id = int(payload.get("data", {}).get("id") or 0)
    report("通知渠道创建", status == 200 and bool(channel_id), f"HTTP {status}")
    status, mail, _ = request_json("POST", HTTP_BASE + "/api/system/notifications",
                                   {"name": "验收邮件", "type": "email",
                                    "endpoint": f"starttls://127.0.0.1:{smtp.server_address[1]}",
                                    "recipients": "ops@example.com", "sender": "edgepulse@example.com",
                                    "caFile": str(CERTS / "ca.pem"),
                                    "username": "mailer", "secret": "mail-pass", "minLevel": "warning", "enabled": True}, token=token)
    mail_id = int(mail.get("data", {}).get("id") or 0)
    report("邮件通知渠道创建", status == 200 and bool(mail_id), f"HTTP {status}")
    status, listing, _ = request_json("GET", HTTP_BASE + "/api/system/notifications?pageNum=1&pageSize=10", token=token)
    rows = listing.get("data", {}).get("rows", [])
    report("通知渠道密钥不回显", status == 200 and rows and all(row.get("secretConfigured") is True and "secret" not in row for row in rows), f"HTTP {status}")
    return server, thread, channel_id, secret, smtp, smtp_thread, mail_id


def verify_notification(token: str, state: tuple[ThreadingHTTPServer, threading.Thread, int, str, SmtpServer, threading.Thread, int]) -> None:
    server, thread, channel_id, secret, smtp, smtp_thread, mail_id = state
    try:
        body, signature = webhook_events.get(timeout=5)
        expected = "sha256=" + hmac.new(secret.encode(), body, hashlib.sha256).hexdigest()
        report("告警 Webhook 收到事件", b'"type":"alarm"' in body, body[:120].decode("utf-8", "replace"))
        report("告警 Webhook 签名正确", hmac.compare_digest(expected, signature), signature)
    except queue.Empty:
        report("告警 Webhook 收到事件", False, "5 秒内没有收到")
    try:
        message = mail_events.get(timeout=5)
        report("告警邮件收到事件", b"Subject:" in message and b"ops@example.com" in message, message[:120].decode("utf-8", "replace"))
    except queue.Empty:
        report("告警邮件收到事件", False, "5 秒内没有收到")
    delivery_total = 0
    for _ in range(10):
        status, page, _ = request_json("GET", HTTP_BASE + "/api/system/notifications/deliveries?pageNum=1&pageSize=10&status=succeeded", token=token)
        delivery_total = page.get("data", {}).get("total", 0)
        if status == 200 and delivery_total >= 2:
            break
        time.sleep(0.2)
    report("通知投递记录落库", delivery_total >= 2, f"success={delivery_total}")
    server.shutdown()
    thread.join(timeout=2)
    smtp.shutdown()
    smtp_thread.join(timeout=2)
    for identifier in (channel_id, mail_id):
        if identifier:
            request_json("DELETE", HTTP_BASE + f"/api/system/notifications/{identifier}", token=token)


def check_telemetry_alarm(token: str, node_id: int) -> None:
    body = {"nodeId": node_id, "samples": [{"metric": "cpu.usage", "value": 99, "unit": "%"}, {"metric": "memory.usage", "value": 20, "unit": "%"}]}
    status, payload, _ = request_json("POST", AGENT_BASE + "/api/agent/telemetry", body, context=agent_context())
    report("Agent 遥测上报", status == 200 and payload.get("data", {}).get("accepted") == 2, f"HTTP {status}")
    started = time.time()
    observed = False
    while time.time() - started < 2.0:
        status, alarms, _ = request_json("GET", HTTP_BASE + f"/api/alarms?pageNum=1&pageSize=20&nodeId={node_id}", token=token)
        if status == 200 and any(row.get("metric") == "cpu.usage" and row.get("status") == "open" for row in (alarms.get("data") or {}).get("rows", [])):
            observed = True
            break
        time.sleep(0.1)
    report("告警 2 秒内可见", observed, f"{time.time() - started:.2f}s")

    status, latest, _ = request_json("GET", HTTP_BASE + f"/api/telemetry/latest?nodeId={node_id}", token=token)
    report("遥测最新值可读", status == 200 and any(point.get("metric") == "cpu.usage" for point in latest.get("data", [])), f"HTTP {status}")
    status, series, _ = request_json("GET", HTTP_BASE + f"/api/telemetry/series?nodeId={node_id}&metric=cpu.usage", token=token)
    report("遥测时间序列可读", status == 200 and len(series.get("data", [])) >= 1, f"HTTP {status}")


def check_sse_alarm(token: str, node_id: int) -> None:
    connection = http.client.HTTPConnection("127.0.0.1", 8080, timeout=6)
    try:
        connection.request("GET", "/api/events/alarms", headers={"Authorization": "Bearer " + token})
        response = connection.getresponse()
        if response.status != 200:
            report("告警 SSE 建立", False, f"HTTP {response.status}")
            return
        report("告警 SSE 建立", True)
        trigger = {"nodeId": node_id, "samples": [{"metric": "memory.usage", "value": 99, "unit": "%"}]}
        request_json("POST", AGENT_BASE + "/api/agent/telemetry", trigger, context=agent_context())
        deadline = time.time() + 3
        received = False
        while time.time() < deadline:
            line = response.readline().decode("utf-8", "replace")
            if line.startswith("event: alarm"):
                received = True
                break
        report("告警 SSE 3 秒内收到事件", received)
    except Exception as failure:  # noqa: BLE001
        report("告警 SSE 建立", False, str(failure))
    finally:
        connection.close()


def check_agent_websocket(token: str) -> None:
    """启动带 --ws 的 Agent 示例，断言命令经长连接推送并由示例回写结果。"""
    fingerprint = (CERTS / "agent-fingerprint.txt").read_text(encoding="utf-8").strip()
    agent = executable(ROOT / "build" / "debug" / "samples" / "edgepulse-agent")
    process = subprocess.Popen(
        [str(agent), "--ca", str(CERTS / "ca.pem"), "--cert", str(CERTS / "agent.pem"), "--key", str(CERTS / "agent-key.pem"),
         "--fingerprint", fingerprint, "--name", f"ws-agent-{RUN_ID}", "--ws"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", bufsize=1,
    )
    lines: queue.Queue[str | None] = queue.Queue()
    collected: list[str] = []

    def pump() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            text = line.rstrip()
            collected.append(text)
            lines.put(text)
        lines.put(None)

    threading.Thread(target=pump, daemon=True).start()
    node_id = 0
    deadline = time.time() + 20.0
    while time.time() < deadline:
        try:
            line = lines.get(timeout=0.5)
        except queue.Empty:
            continue
        if line is None:
            break
        if line.startswith("ws-ready nodeId="):
            try:
                node_id = int(line.split("=", 1)[1])
            except ValueError:
                node_id = 0
            break
    report("Agent WebSocket 完成 hello", node_id > 0, f"nodeId={node_id}")
    if node_id <= 0:
        stop_process(process)
        return

    status, command, _ = request_json("POST", HTTP_BASE + "/api/commands",
                                      {"nodeId": node_id, "type": "ws.echo", "payload": "{}", "timeoutSeconds": 30}, token=token)
    command_id = command.get("data", {}).get("id")
    report("WS 命令创建", status == 200 and bool(command_id), f"HTTP {status}")

    try:
        process.wait(timeout=20)
    except subprocess.TimeoutExpired:
        stop_process(process)
    while True:
        try:
            if lines.get(timeout=0.2) is None:
                break
        except queue.Empty:
            break
    output = "\n".join(collected)
    report("WS Agent 收到命令并回写", process.returncode == 0 and "ws=ok" in output and "commands=1" in output, output.strip() or "无输出")

    status_text = ""
    for _ in range(100):
        status, listing, _ = request_json("GET", HTTP_BASE + f"/api/commands?pageNum=1&pageSize=10&nodeId={node_id}", token=token)
        rows = listing.get("data", {}).get("rows", [])
        if rows:
            status_text = rows[0].get("status", "")
        if status_text == "succeeded":
            break
        time.sleep(0.1)
    report("WS 命令状态为 succeeded", status_text == "succeeded", status_text or f"HTTP {status}")


def check_commands(token: str, node_id: int) -> None:
    status, command, _ = request_json("POST", HTTP_BASE + "/api/commands",
                                      {"nodeId": node_id, "type": "diagnostics.collect", "payload": "{}", "timeoutSeconds": 30}, token=token)
    command_id = command.get("data", {}).get("id")
    report("命令创建", status == 200 and bool(command_id), f"HTTP {status}")
    if not command_id:
        return
    status, next_command, _ = request_json("GET", AGENT_BASE + f"/api/agent/commands/next?nodeId={node_id}", context=agent_context())
    report("Agent 轮询到命令", status == 200 and next_command.get("data", {}).get("id") == command_id, f"HTTP {status}")
    status, finished, _ = request_json("POST", AGENT_BASE + f"/api/agent/commands/{command_id}/result",
                                       {"nodeId": node_id, "status": "succeeded", "result": "{\"ok\":true}", "message": "acceptance"}, context=agent_context())
    report("Agent 回传命令结果", status == 200 and finished.get("data", {}).get("status") == "succeeded", f"HTTP {status}")


def check_firmware(token: str, node_id: int) -> None:
    size = 100 * 1024 * 1024
    chunk = b"EdgePulseFirmware-" * 65536
    chunk = chunk[:1024 * 1024]
    connection = http.client.HTTPConnection("127.0.0.1", 8080, timeout=300)
    connection.putrequest("POST", f"/api/firmware?name=EdgePulse+OS&version=9.{RUN_ID}&fileName=edge-os.bin")
    connection.putheader("Authorization", "Bearer " + token)
    connection.putheader("Content-Type", "application/octet-stream")
    connection.putheader("Content-Length", str(size))
    connection.endheaders()
    import hashlib

    digest = hashlib.sha256()
    for _ in range(100):
        connection.send(chunk)
        digest.update(chunk)
    response = connection.getresponse()
    body = response.read()
    connection.close()
    try:
        firmware = json.loads(body)["data"]
    except Exception:  # noqa: BLE001
        report("100 MiB 固件上传", False, body[:200].decode("utf-8", "replace"))
        return
    report("100 MiB 固件上传", response.status == 200 and firmware.get("sizeBytes") == size and firmware.get("sha256") == digest.hexdigest(), f"HTTP {response.status}")
    firmware_id = firmware["id"]

    download_path = f"/api/agent/firmware/{firmware_id}/download?nodeId={node_id}"
    connection = http.client.HTTPSConnection("127.0.0.1", 9443, context=agent_context(), timeout=300)
    connection.request("GET", download_path, headers={"Range": "bytes=0-1023"})
    range_response = connection.getresponse()
    range_body = range_response.read()
    headers = {key.lower(): value for key, value in range_response.getheaders()}
    connection.close()
    report("固件 Range 下载 206", range_response.status == 206 and len(range_body) == 1024 and headers.get("content-range") == f"bytes 0-1023/{size}",
           f"HTTP {range_response.status}")

    connection = http.client.HTTPSConnection("127.0.0.1", 9443, context=agent_context(), timeout=60)
    connection.request("GET", download_path, headers={"If-None-Match": headers.get("etag", "")})
    cached = connection.getresponse()
    cached.read()
    connection.close()
    report("固件 ETag 命中 304", cached.status == 304, f"HTTP {cached.status}")

    status, deployed, _ = request_json("POST", HTTP_BASE + f"/api/firmware/{firmware_id}/deploy", {"nodeIds": [node_id]}, token=token)
    report("固件下发任务创建", status == 200 and len(deployed.get("data", [])) == 1, f"HTTP {status}")
    status, tasks, _ = request_json("GET", HTTP_BASE + f"/api/firmware/{firmware_id}/tasks", token=token)
    report("固件任务可读", status == 200 and len(tasks.get("data", [])) == 1, f"HTTP {status}")
    request_json("DELETE", HTTP_BASE + f"/api/firmware/{firmware_id}", token=token)


def check_enrollment_tokens(token: str) -> None:
    status, created, _ = request_json("POST", HTTP_BASE + "/api/enrollment-tokens",
                                      {"action": "enroll", "expiresInSeconds": 3600, "nodeName": f"enroll-{RUN_ID}"}, token=token)
    row = created.get("data") or {}
    token_id = row.get("id")
    plaintext = str(row.get("token", ""))
    report("注册令牌创建并只回显一次", status == 200 and plaintext.startswith("ep_enroll_") and bool(token_id), f"HTTP {status}")
    status, listing, _ = request_json("GET", HTTP_BASE + "/api/enrollment-tokens", token=token)
    rows = listing.get("data", [])
    # 列表是数组而不是分页对象；明文与哈希都不得出现在任何一行
    listed = isinstance(rows, list) and any(item.get("id") == token_id for item in rows)
    report("注册令牌列表不回显明文", status == 200 and listed and all("token" not in item and "tokenHash" not in item for item in rows), f"HTTP {status}")
    if token_id:
        status, _, _ = request_json("DELETE", HTTP_BASE + f"/api/enrollment-tokens/{token_id}", token=token)
        report("注册令牌吊销", status == 200, f"HTTP {status}")


def check_agent_identity(agent_node_id: int) -> None:
    status, _, _ = request_json("POST", AGENT_BASE + "/api/agent/heartbeat",
                                {"nodeId": agent_node_id, "agentVersion": "0.1.0"},
                                context=client_context(CERTS / "agent-foreign.pem", CERTS / "agent-foreign-key.pem"))
    report("Agent 冒名心跳被拒", status == 403, f"HTTP {status}")


def check_node_config(token: str, agent_node_id: int) -> None:
    content = {"sample_interval": 30}
    status, config, _ = request_json("PUT", HTTP_BASE + f"/api/nodes/{agent_node_id}/config",
                                     {"content": content, "expectedVersion": 0, "note": "验收"}, token=token)
    report("配置下发", status == 200 and (config.get("data") or {}).get("desired", {}).get("version") == 1, f"HTTP {status}")
    status, fetched, _ = request_json("GET", AGENT_BASE + f"/api/agent/nodes/{agent_node_id}/config", context=agent_context())
    report("Agent 拉取配置", status == 200 and (fetched.get("data") or {}).get("version") == 1 and (fetched.get("data") or {}).get("content") == content,
           f"HTTP {status}")
    status, reported, _ = request_json("POST", AGENT_BASE + f"/api/agent/nodes/{agent_node_id}/config/report",
                                       {"version": 1, "content": content, "status": "applied"}, context=agent_context())
    report("Agent 回报配置", status == 200 and (reported.get("data") or {}).get("inSync") is True, f"HTTP {status}")
    status, admin_config, _ = request_json("GET", HTTP_BASE + f"/api/nodes/{agent_node_id}/config", token=token)
    report("管理端看到配置同步", status == 200 and (admin_config.get("data") or {}).get("inSync") is True, f"HTTP {status}")
    status, heartbeat, _ = request_json("POST", AGENT_BASE + "/api/agent/heartbeat",
                                        {"nodeId": agent_node_id, "agentVersion": "0.1.0"}, context=agent_context())
    interval = (heartbeat.get("data") or {}).get("heartbeatIntervalSeconds")
    report("心跳下发期望配置版本", status == 200 and (heartbeat.get("data") or {}).get("desiredConfigVersion") == 1 and isinstance(interval, int) and interval > 0,
           f"HTTP {status}")


def check_webhook_ssrf(token: str) -> None:
    channel_name = "验收元数据 Webhook"
    status, payload, _ = request_json("POST", HTTP_BASE + "/api/system/notifications",
                                      {"name": channel_name, "type": "webhook",
                                       "endpoint": "http://169.254.169.254/latest/meta-data",
                                       "minLevel": "warning", "enabled": True}, token=token)
    message = str(payload.get("message", ""))
    report("Webhook 元数据地址被拒", status == 400 and "元数据" in message, f"HTTP {status}：{message}")
    data = payload.get("data")
    channel_id = data.get("id") if isinstance(data, dict) else None
    if channel_id:
        # 守卫被绕过时宁可先删掉渠道，也不能把失败改写成通过
        request_json("DELETE", HTTP_BASE + f"/api/system/notifications/{channel_id}", token=token)
        return
    # 错误响应通常带 data=null；守卫若意外放行却又没回显标识，按名字找回渠道再删，避免留下启用的测试渠道
    query = urllib.parse.urlencode({"keyword": channel_name, "pageNum": 1, "pageSize": 50})
    status, listing, _ = request_json("GET", HTTP_BASE + f"/api/system/notifications?{query}", token=token)
    if status != 200:
        return
    for row in (listing.get("data") or {}).get("rows", []):
        if row.get("name") == channel_name and row.get("id") is not None:
            request_json("DELETE", HTTP_BASE + f"/api/system/notifications/{row['id']}", token=token)


def check_signed_firmware(token: str, node_id: int, signing: FirmwareSigning | None, unavailable_detail: str) -> None:
    if signing is None:
        skip("签名固件上传与下载", unavailable_detail or "未生成临时签名密钥")
        return
    query = urllib.parse.urlencode({"name": "EdgePulse OS Signed", "version": f"9.{RUN_ID}-signed", "fileName": "edge-os-signed.bin"})
    connection = http.client.HTTPConnection("127.0.0.1", 8080, timeout=120)
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
    connection.close()
    try:
        firmware = json.loads(body).get("data") or {}
    except (json.JSONDecodeError, AttributeError):
        report("签名固件上传", False, body[:200].decode("utf-8", "replace"))
        return
    firmware_id = firmware.get("id")
    report("签名固件上传", response.status == 200 and firmware.get("signed") is True and firmware.get("signatureKeyId") == signing.key_id
           and firmware.get("signatureAlgorithm") == "ed25519-sha256", f"HTTP {response.status}")
    try:
        if not firmware_id:
            report("签名固件下载回传签名头", False, "上传未返回固件标识")
            return
        connection = http.client.HTTPSConnection("127.0.0.1", 9443, context=agent_context(), timeout=60)
        connection.request("GET", f"/api/agent/firmware/{firmware_id}/download?nodeId={node_id}")
        download = connection.getresponse()
        downloaded = download.read()
        headers = {key.lower(): value for key, value in download.getheaders()}
        connection.close()
        report("签名固件下载回传签名头",
               download.status == 200 and downloaded == signing.payload and headers.get("x-firmware-signature") == signing.signature
               and headers.get("x-firmware-key-id") == signing.key_id and headers.get("x-firmware-signature-algorithm") == "ed25519-sha256",
               f"HTTP {download.status}")
    finally:
        if firmware_id:
            status, _, _ = request_json("DELETE", HTTP_BASE + f"/api/firmware/{firmware_id}", token=token)
            if status != 200:
                print(f"WARN 删除验收签名固件 {firmware_id} 失败：HTTP {status}，请手工清理")


def check_offline(token: str, agent_node_id: int) -> None:
    status, heartbeat, _ = request_json("POST", AGENT_BASE + "/api/agent/heartbeat",
                                        {"nodeId": agent_node_id, "agentVersion": "0.1.0"}, context=agent_context())
    report("离线检查前心跳在线", status == 200 and (heartbeat.get("data") or {}).get("status") == "online", f"HTTP {status}")
    deadline = time.time() + OFFLINE_WAIT_SECONDS
    node_status = ""
    alarm_seen = False
    while time.time() < deadline:
        status, node, _ = request_json("GET", HTTP_BASE + f"/api/nodes/{agent_node_id}", token=token)
        node_status = str((node.get("data") or {}).get("status", "")) if status == 200 else f"HTTP {status}"
        status, alarms, _ = request_json("GET", HTTP_BASE + f"/api/alarms?pageNum=1&pageSize=50&status=open&nodeId={agent_node_id}", token=token)
        alarm_seen = status == 200 and any(row.get("metric") == "node.online" for row in (alarms.get("data") or {}).get("rows", []))
        if node_status == "offline" and alarm_seen:
            break
        time.sleep(1)
    report("心跳超时节点转离线", node_status == "offline", f"{OFFLINE_WAIT_SECONDS}s 内状态 {node_status}")
    report("离线生成系统告警", alarm_seen, "metric=node.online")

    status, heartbeat, _ = request_json("POST", AGENT_BASE + "/api/agent/heartbeat",
                                        {"nodeId": agent_node_id, "agentVersion": "0.1.0"}, context=agent_context())
    report("心跳恢复节点在线", status == 200 and (heartbeat.get("data") or {}).get("status") == "online", f"HTTP {status}")
    resolved = False
    alarm_status = ""
    deadline = time.time() + 5
    while time.time() < deadline:
        status, alarms, _ = request_json("GET", HTTP_BASE + f"/api/alarms?pageNum=1&pageSize=50&nodeId={agent_node_id}", token=token)
        row = next((item for item in (alarms.get("data") or {}).get("rows", []) if item.get("metric") == "node.online"), None)
        alarm_status = str(row.get("status", "")) if row else ""
        if alarm_status == "resolved":
            resolved = True
            break
        time.sleep(0.2)
    report("恢复在线后系统告警销案", resolved, alarm_status or "未找到 node.online 告警")


def check_h3() -> None:
    context = client_context()
    try:
        status, payload, _ = request_json("GET", HTTPS_BASE + "/api/health", context=context)
        report("8443 HTTPS 健康检查", status == 200 and payload.get("data", {}).get("status") == "UP", f"HTTP {status}")
    except Exception as failure:  # noqa: BLE001
        report("8443 HTTPS 健康检查", False, str(failure))
    try:
        status, _, _ = request_json("GET", HTTPS_BASE + "/api/probe/h3", context=context)
        tcp_status = status
    except urllib.error.HTTPError as failure:
        tcp_status = failure.code
    except Exception as failure:  # noqa: BLE001
        report("TCP 侧不存在 h3 专属路径", False, str(failure))
    else:
        report("TCP 侧不存在 h3 专属路径", tcp_status == 404, f"HTTP {tcp_status}")
    probe = executable(ROOT / "build" / "debug" / "samples" / "H3Probe")
    result = subprocess.run([str(probe), "--ca", str(CERTS / "ca.pem")], capture_output=True, text=True, timeout=60, check=False)
    report("H3Probe 真实 h3", result.returncode == 0 and "h3" in result.stdout, result.stdout.strip() or result.stderr.strip())


def check_mtls() -> int | None:
    try:
        request_json("GET", AGENT_BASE + "/api/agent/heartbeat", context=client_context())
        report("9443 无客户端证书被拒", False, "请求意外成功")
    except Exception:  # noqa: BLE001
        report("9443 无客户端证书被拒", True)
    fingerprint = (CERTS / "agent-fingerprint.txt").read_text(encoding="utf-8").strip()
    probe = executable(ROOT / "build" / "debug" / "samples" / "edgepulse-agent")
    result = subprocess.run(
        [str(probe), "--ca", str(CERTS / "ca.pem"), "--cert", str(CERTS / "agent.pem"), "--key", str(CERTS / "agent-key.pem"), "--fingerprint", fingerprint],
        capture_output=True, text=True, timeout=60, check=False,
    )
    report("9443 正确证书注册并心跳", result.returncode == 0 and "heartbeats=3" in result.stdout, result.stdout.strip() or result.stderr.strip())
    # 每次验收用独立节点名，保证设备影子从版本 0 开始；同一张证书可以登记多个节点
    status, registered, _ = request_json("POST", AGENT_BASE + "/api/agent/register",
                                         {"name": f"edgepulse-agent-http-{RUN_ID}", "agentVersion": "0.1.0", "certFingerprint": fingerprint},
                                         context=agent_context())
    agent_node_id = (registered.get("data") or {}).get("id")
    report("9443 正确证书走 HTTP 注册", status == 200 and bool(agent_node_id), f"HTTP {status}")
    return int(agent_node_id) if agent_node_id else None


def check_ops() -> None:
    try:
        status, _, _ = request_json("GET", OPS_BASE + "/metrics")
        report("9100 /metrics 无令牌被拒", status == 401, f"HTTP {status}")
    except urllib.error.HTTPError as failure:
        report("9100 /metrics 无令牌被拒", failure.code == 401, f"HTTP {failure.code}")
    token = OPS_TOKEN_FILE.read_text(encoding="utf-8").strip()
    status, _, _ = request_json("GET", OPS_BASE + "/metrics", token=token)
    report("9100 /metrics 带令牌可读", status == 200, f"HTTP {status}")
    status, ready, _ = request_json("GET", OPS_BASE + "/readyz")
    report("9100 /readyz 就绪", status == 200 and ready.get("status") == "ready", f"HTTP {status}")
    status, payload, _ = request_json("GET", OPS_BASE + "/healthz")
    report("9100 /healthz", status == 200, f"HTTP {status}")


def check_otlp() -> None:
    result = subprocess.run([sys.executable, str(ROOT / "scripts" / "check-otlp.py")], capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=120, check=False)
    detail = (result.stdout or result.stderr).strip().replace("\n", "；")
    report("OTLP mock 收到指标导出", result.returncode == 0, detail)


def check_udp_telemetry() -> None:
    result = subprocess.run([sys.executable, str(ROOT / "scripts" / "check-udp-telemetry.py")], capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=120, check=False)
    detail = (result.stdout or result.stderr).strip().replace("\n", "；")
    report("UDP 遥测数据报签名、落库与回读", result.returncode == 0, detail)


def check_oidc() -> None:
    result = subprocess.run([sys.executable, str(ROOT / "scripts" / "check-oidc.py")], capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=180, check=False)
    detail = (result.stdout or result.stderr).strip().replace("\n", "；")
    report("OIDC 授权码登录与自动建号", result.returncode == 0, detail)


def check_websocket(token: str) -> None:
    script = ROOT / "build" / "gate" / "ws-check.mjs"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text(
        """
const target = process.argv[2];
const token = process.argv[3];
const socket = new WebSocket(target);
const timer = setTimeout(() => { console.error('timeout'); process.exit(1); }, 10000);
socket.onopen = () => socket.send(JSON.stringify({ type: 'auth', token }));
socket.onmessage = event => {
  try {
    const message = JSON.parse(String(event.data));
    if (message.type === 'auth' && message.ok === true) {
      clearTimeout(timer);
      console.log('ws-auth-ok');
      socket.close();
      process.exit(0);
    }
  } catch {}
};
socket.onerror = () => { clearTimeout(timer); console.error('ws-error'); process.exit(1); };
""",
        encoding="utf-8",
    )
    result = subprocess.run(["node", str(script), "ws://127.0.0.1:8080/api/ws", token], capture_output=True, text=True, timeout=30, check=False)
    report("浏览器 WebSocket 首条认证", result.returncode == 0 and "ws-auth-ok" in result.stdout, result.stdout.strip() or result.stderr.strip())


def main() -> int:
    if not CERTS.exists():
        subprocess.run(["bash", "scripts/dev-certs.sh"], cwd=ROOT, check=False)
    process: subprocess.Popen[bytes] | None = None
    signing: FirmwareSigning | None = None
    signing_detail = ""
    already_running = False
    try:
        try:
            request_json("GET", HTTP_BASE + "/api/health", timeout=1.0)
            already_running = True
        except Exception:  # noqa: BLE001
            # 信任键目录只在启动时加载，所以密钥必须在启动之前就位
            signing, signing_detail = prepare_firmware_signing()
            creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
            process = subprocess.Popen(["bash", "scripts/run-dev.sh"], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       creationflags=creation_flags)
            if not wait_for_health(process):
                print("FAIL 开发服务未在时限内就绪")
                return 1
        token = check_authentication()
        check_h3()
        agent_node_id = check_mtls()
        check_ops()
        check_otlp()
        check_udp_telemetry()
        check_oidc()
        if token:
            check_system(token)
            check_tenancy(token)
            check_nodes(token)
            check_websocket(token)
            check_enrollment_tokens(token)
            check_webhook_ssrf(token)
            if agent_node_id:
                notification_state = check_notifications(token)
                check_telemetry_alarm(token, agent_node_id)
                verify_notification(token, notification_state)
                check_sse_alarm(token, agent_node_id)
                check_commands(token, agent_node_id)
                check_agent_websocket(token)
                check_firmware(token, agent_node_id)
                check_signed_firmware(token, agent_node_id, signing, "服务已在运行，信任键目录只在启动时加载" if already_running else signing_detail)
                check_agent_identity(agent_node_id)
                check_node_config(token, agent_node_id)
                # 离线检查会故意让 Agent 节点离线并产生系统告警，必须放在最后
                check_offline(token, agent_node_id)
            else:
                report("Agent 节点检查", False, "mTLS 注册未返回节点标识，无法继续")
        else:
            report("系统、节点与设备域检查", False, "登录失败，无法继续")
    finally:
        if process is not None and not already_running:
            stop_process(process)
        cleanup_firmware_signing(signing)
    print()
    print(f"验收汇总：通过 {passed} 项，失败 {len(failures)} 项，跳过 {len(skips)} 项")
    if failures:
        print(f"验收失败：{len(failures)} 项 -> " + ", ".join(failures))
        return 1
    if skips:
        print("验收通过，跳过项不计入通过：" + ", ".join(skips))
    else:
        print("全量验收全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
