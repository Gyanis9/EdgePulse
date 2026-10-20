#!/usr/bin/env python3
"""P0 验收：启动开发服务，逐项验证认证、节点、h3、mTLS 与运维口。"""

from __future__ import annotations

import json
import os
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CERTS = ROOT / "var" / "certs"
OPS_TOKEN_FILE = ROOT / "var" / "ops-token"
HTTP_BASE = "http://127.0.0.1:8080"
HTTPS_BASE = "https://127.0.0.1:8443"
AGENT_BASE = "https://127.0.0.1:9443"
OPS_BASE = "http://127.0.0.1:9100"

failures: list[str] = []


def report(name: str, ok: bool, detail: str = "") -> None:
    print(("PASS " if ok else "FAIL ") + name + (("：" + detail) if detail else ""))
    if not ok:
        failures.append(name)


def request_json(method: str, url: str, payload: dict | None = None, token: str | None = None, context: ssl.SSLContext | None = None,
                 timeout: float = 10.0) -> tuple[int, dict, dict]:
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
                # /metrics 是 Prometheus 文本；调用方只看状态码
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
    report("8080 验证码签发", status == 200 and bool(captcha.get("data", {}).get("uuid")) and bool(captcha.get("data", {}).get("imageSvg")),
           f"HTTP {status}")
    if not code:
        report("开发档验证码暴露 code", False, "缺少 code，Playwright 无法登录")
        return None
    status, login, headers = request_json(
        "POST",
        HTTP_BASE + "/api/auth/login",
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
    component = tree[0]["children"][0]["component"] if tree and tree[0].get("children") else ""
    report("routes 返回节点菜单", status == 200 and component == "device/Nodes", component)
    return token


def check_nodes(token: str) -> None:
    status, listing, _ = request_json("GET", HTTP_BASE + "/api/nodes?pageNum=1&pageSize=50", token=token)
    names = [row.get("name") for row in listing.get("data", {}).get("rows", [])]
    report("节点列表可读", status == 200 and isinstance(names, list), f"HTTP {status}")
    report("Agent 注册的节点出现在列表", "edgepulse-agent" in names, ", ".join(str(name) for name in names))


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
        [str(probe), "--ca", str(CERTS / "ca.pem"), "--cert", str(CERTS / "agent.pem"), "--key", str(CERTS / "agent-key.pem"),
         "--fingerprint", fingerprint],
        capture_output=True, text=True, timeout=60, check=False,
    )
    report("9443 正确证书注册并心跳", result.returncode == 0 and "heartbeats=3" in result.stdout, result.stdout.strip() or result.stderr.strip())

    status, register, _ = request_json(
        "POST",
        AGENT_BASE + "/api/agent/register",
        {"name": "edgepulse-agent-http", "agentVersion": "0.1.0", "certFingerprint": fingerprint},
        context=client_context(CERTS / "agent.pem", CERTS / "agent-key.pem"),
    )
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
    status, payload, _ = request_json("GET", OPS_BASE + "/healthz")
    report("9100 /healthz", status == 200, f"HTTP {status}")


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
        # Agent 探针会注册 edgepulse-agent，节点列表检查放在它之后
        check_mtls()
        if token:
            check_nodes(token)
            check_websocket(token)
        else:
            report("节点与 WebSocket 检查", False, "登录失败，无法继续")
        check_ops()
    finally:
        if process is not None and not already_running:
            stop_process(process)
    print()
    if failures:
        print(f"验收失败：{len(failures)} 项 -> " + ", ".join(failures))
        return 1
    print("P0 验收全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
