#!/usr/bin/env python3
"""全量验收：启动开发服务，逐项验证认证、系统管理、节点、遥测告警、固件、命令、WebSocket、h3、mTLS 与运维口。"""

from __future__ import annotations

import hashlib
import hmac
import http.client
import json
import os
import queue
import socketserver
import ssl
import subprocess
import sys
import threading
import time
import urllib.error
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
failures: list[str] = []
webhook_events: queue.Queue = queue.Queue()
mail_events: queue.Queue = queue.Queue()


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
    print(("PASS " if ok else "FAIL ") + name + (("：" + detail) if detail else ""))
    if not ok:
        failures.append(name)


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
                                    "endpoint": f"smtp://127.0.0.1:{smtp.server_address[1]}",
                                    "recipients": "ops@example.com", "sender": "edgepulse@example.com",
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
    finally:
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
        if status == 200 and any(row.get("metric") == "cpu.usage" and row.get("status") == "open" for row in alarms.get("data", {}).get("rows", [])):
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

    connection = http.client.HTTPSConnection("127.0.0.1", 9443, context=agent_context(), timeout=300)
    connection.request("GET", f"/api/agent/firmware/{firmware_id}/download", headers={"Range": "bytes=0-1023"})
    range_response = connection.getresponse()
    range_body = range_response.read()
    headers = {key.lower(): value for key, value in range_response.getheaders()}
    connection.close()
    report("固件 Range 下载 206", range_response.status == 206 and len(range_body) == 1024 and headers.get("content-range") == f"bytes 0-1023/{size}",
           f"HTTP {range_response.status}")

    connection = http.client.HTTPSConnection("127.0.0.1", 9443, context=agent_context(), timeout=60)
    connection.request("GET", f"/api/agent/firmware/{firmware_id}/download", headers={"If-None-Match": headers.get("etag", "")})
    cached = connection.getresponse()
    cached.read()
    connection.close()
    report("固件 ETag 命中 304", cached.status == 304, f"HTTP {cached.status}")

    status, deployed, _ = request_json("POST", HTTP_BASE + f"/api/firmware/{firmware_id}/deploy", {"nodeIds": [node_id]}, token=token)
    report("固件下发任务创建", status == 200 and len(deployed.get("data", [])) == 1, f"HTTP {status}")
    status, tasks, _ = request_json("GET", HTTP_BASE + f"/api/firmware/{firmware_id}/tasks", token=token)
    report("固件任务可读", status == 200 and len(tasks.get("data", [])) == 1, f"HTTP {status}")
    request_json("DELETE", HTTP_BASE + f"/api/firmware/{firmware_id}", token=token)


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


def check_mtls() -> None:
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
    status, _, _ = request_json("POST", AGENT_BASE + "/api/agent/register",
                                {"name": "edgepulse-agent-http", "agentVersion": "0.1.0", "certFingerprint": fingerprint}, context=agent_context())
    report("9443 正确证书走 HTTP 注册", status == 200, f"HTTP {status}")


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
    try:
        request_json("GET", HTTP_BASE + "/api/health", timeout=1.0)
        already_running = True
    except Exception:  # noqa: BLE001
        already_running = False
        creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        process = subprocess.Popen(["bash", "scripts/run-dev.sh"], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   creationflags=creation_flags)
        if not wait_for_health(process):
            print("FAIL 开发服务未在时限内就绪")
            return 1
    try:
        token = check_authentication()
        check_h3()
        check_mtls()
        check_ops()
        check_otlp()
        check_udp_telemetry()
        check_oidc()
        if token:
            check_system(token)
            check_tenancy(token)
            node_id = check_nodes(token)
            check_websocket(token)
            if node_id:
                notification_state = check_notifications(token)
                check_telemetry_alarm(token, node_id)
                verify_notification(token, notification_state)
                check_sse_alarm(token, node_id)
                check_commands(token, node_id)
                check_agent_websocket(token)
                check_firmware(token, node_id)
        else:
            report("系统、节点与设备域检查", False, "登录失败，无法继续")
    finally:
        if process is not None and not already_running:
            stop_process(process)
    print()
    if failures:
        print(f"验收失败：{len(failures)} 项 -> " + ", ".join(failures))
        return 1
    print("全量验收全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
