#!/usr/bin/env python3
"""全量验收：启动开发服务，逐项验证认证、系统管理、节点、遥测告警、固件、命令、WebSocket、h3、mTLS 与运维口。"""

from __future__ import annotations

import http.client
import json
import os
import queue
import ssl
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
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


def login_user(username: str, password: str) -> str | None:
    status, captcha, _ = request_json("GET", HTTP_BASE + "/api/auth/captcha")
    code = captcha.get("data", {}).get("code")
    if status != 200 or not code:
        return None
    status, login, _ = request_json("POST", HTTP_BASE + "/api/auth/login",
                                    {"username": username, "password": password, "captcha": code, "captchaUuid": captcha["data"]["uuid"]})
    return login.get("data", {}).get("token") if status == 200 else None


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
        if token:
            check_system(token)
            node_id = check_nodes(token)
            check_websocket(token)
            if node_id:
                check_telemetry_alarm(token, node_id)
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
