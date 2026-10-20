#!/usr/bin/env python3
"""OIDC 授权码登录验收：本地 mock IdP、PKCE、ID Token 与会话签发。"""

from __future__ import annotations

import base64
import hashlib
import http.client
import json
import os
import secrets
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / "build" / "gate"
CONFIG_PATH = GATE / "oidc-check.yaml"
SERVER_LOG = GATE / "oidc-server.log"
DATABASE_PATH = GATE / "oidc-check.db"
APP = "http://127.0.0.1:18080"
IDP = "http://127.0.0.1:14320"
CLIENT_ID = "edgepulse"
CLIENT_SECRET = "oidc-test-secret"
REDIRECT_URI = APP + "/api/auth/oidc/callback"

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa

KEY = rsa.generate_private_key(public_exponent=65537, key_size=2048)
KEY_ID = "oidc-check-key"
codes: dict[str, dict] = {}
codes_lock = threading.Lock()


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def b64url_int(value: int) -> str:
    return b64url(value.to_bytes((value.bit_length() + 7) // 8, "big"))


def sign_id_token(nonce: str) -> str:
    header = {"alg": "RS256", "typ": "JWT", "kid": KEY_ID}
    payload = {
        "iss": IDP,
        "aud": CLIENT_ID,
        "sub": "oidc-subject-1",
        "email": "oidc.user@example.com",
        "preferred_username": "oidcuser",
        "name": "OIDC User",
        "nonce": nonce,
        "iat": int(time.time()),
        "exp": int(time.time()) + 300,
    }
    signing_input = b64url(json.dumps(header, separators=(",", ":")).encode()) + "." + b64url(json.dumps(payload, separators=(",", ":")).encode())
    signature = KEY.sign(signing_input.encode(), padding.PKCS1v15(), hashes.SHA256())
    return signing_input + "." + b64url(signature)


class IdpHandler(BaseHTTPRequestHandler):
    def send_json(self, payload: dict) -> None:
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/.well-known/openid-configuration":
            self.send_json(
                {
                    "issuer": IDP,
                    "authorization_endpoint": IDP + "/authorize",
                    "token_endpoint": IDP + "/token",
                    "jwks_uri": IDP + "/jwks",
                }
            )
            return
        if parsed.path == "/jwks":
            numbers = KEY.public_key().public_numbers()
            self.send_json({"keys": [{"kty": "RSA", "kid": KEY_ID, "use": "sig", "alg": "RS256",
                                      "n": b64url_int(numbers.n), "e": b64url_int(numbers.e)}]})
            return
        if parsed.path == "/authorize":
            query = urllib.parse.parse_qs(parsed.query)
            code = secrets.token_hex(16)
            with codes_lock:
                codes[code] = {
                    "challenge": query.get("code_challenge", [""])[0],
                    "redirect_uri": query.get("redirect_uri", [""])[0],
                    "state": query.get("state", [""])[0],
                    "nonce": query.get("nonce", [""])[0],
                }
            location = query.get("redirect_uri", [""])[0] + "?" + urllib.parse.urlencode({"code": code, "state": query.get("state", [""])[0]})
            self.send_response(302)
            self.send_header("Location", location)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(404)
        self.end_headers()

    def do_POST(self) -> None:  # noqa: N802
        if self.path != "/token":
            self.send_response(404)
            self.end_headers()
            return
        size = int(self.headers.get("Content-Length", "0"))
        form = urllib.parse.parse_qs(self.rfile.read(size).decode())
        code = form.get("code", [""])[0]
        verifier = form.get("code_verifier", [""])[0]
        with codes_lock:
            entry = codes.pop(code, None)
        if entry is None or form.get("client_id", [""])[0] != CLIENT_ID or form.get("client_secret", [""])[0] != CLIENT_SECRET:
            self.send_response(400)
            self.end_headers()
            return
        challenge = b64url(hashlib.sha256(verifier.encode()).digest())
        if challenge != entry["challenge"] or form.get("redirect_uri", [""])[0] != entry["redirect_uri"]:
            self.send_response(400)
            self.end_headers()
            return
        self.send_json({"access_token": "mock-access-token", "token_type": "Bearer", "id_token": sign_id_token(entry["nonce"])})

    def log_message(self, format: str, *args: object) -> None:
        pass


def write_configuration() -> None:
    CONFIG_PATH.write_text(
        f"""edgepulse:
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
    path: build/gate/oidc-check.db
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
  oidc:
    enabled: true
    issuer: {IDP}
    client_id: {CLIENT_ID}
    redirect_uri: {REDIRECT_URI}
    frontend_redirect: /login
    scopes: openid profile email
    allowed_email_domains: example.com
    auto_provision: true
    default_role_id: 2
    default_dept_id: 101

metrics:
  enabled: false
""",
        encoding="utf-8",
    )


def no_redirect_get(url: str) -> tuple[int, str]:
    parsed = urllib.parse.urlparse(url)
    connection = http.client.HTTPConnection(parsed.hostname or "127.0.0.1", parsed.port or 80, timeout=10)
    connection.request("GET", parsed.path + ("?" + parsed.query if parsed.query else ""))
    response = connection.getresponse()
    location = response.getheader("Location", "")
    response.read()
    connection.close()
    return response.status, location


def request(method: str, url: str, payload: dict | None = None, token: str | None = None) -> tuple[int, dict]:
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {"Content-Type": "application/json"} if data is not None else {}
    if token:
        headers["Authorization"] = "Bearer " + token
    try:
        with urllib.request.urlopen(urllib.request.Request(url, data=data, headers=headers, method=method), timeout=10) as response:
            body = response.read().decode()
            return response.status, json.loads(body) if body else {}
    except urllib.error.HTTPError as failure:
        body = failure.read().decode()
        try:
            return failure.code, json.loads(body) if body else {}
        except json.JSONDecodeError:
            return failure.code, {}


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
    idp = ThreadingHTTPServer(("127.0.0.1", 14320), IdpHandler)
    threading.Thread(target=idp.serve_forever, daemon=True).start()
    process: subprocess.Popen[bytes] | None = None
    try:
        with SERVER_LOG.open("wb") as log:
            process = subprocess.Popen([str(server_binary()), "--config", str(CONFIG_PATH)], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                       env={**os.environ, "EDGEPULSE_OIDC_CLIENT_SECRET": CLIENT_SECRET})
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                status, _ = request("GET", APP + "/api/health")
                if status == 200:
                    break
            except (urllib.error.URLError, TimeoutError, ConnectionError):
                pass
            if process.poll() is not None:
                print("FAIL OIDC 服务启动")
                return 1
            time.sleep(0.2)
        else:
            print("FAIL OIDC 服务未在时限内就绪")
            return 1

        status, enabled = request("GET", APP + "/api/auth/oidc/enabled")
        print(("PASS " if status == 200 and enabled.get("data", {}).get("enabled") else "FAIL ") + "OIDC 启用查询")
        status, authorize = no_redirect_get(APP + "/api/auth/oidc/start?return=/device/nodes")
        print(("PASS " if status == 302 and authorize.startswith(IDP + "/authorize") else "FAIL ") + "OIDC 授权跳转：" + authorize)
        status, callback = no_redirect_get(authorize)
        print(("PASS " if status == 302 and callback.startswith(REDIRECT_URI) else "FAIL ") + "mock IdP 回调")
        status, frontend = no_redirect_get(callback)
        query = urllib.parse.parse_qs(urllib.parse.urlparse(frontend).query)
        code = query.get("oidcCode", [""])[0]
        print(("PASS " if status == 302 and frontend.startswith("/login?") and code else "FAIL ") + "OIDC 回到前端并带一次性 code：" + frontend)
        if not code:
            return 1

        status, exchanged = request("POST", APP + "/api/auth/oidc/exchange", {"code": code})
        token = exchanged.get("data", {}).get("token", "")
        print(("PASS " if status == 200 and len(token) == 64 else "FAIL ") + "OIDC 一次性 code 换会话")
        status, me = request("GET", APP + "/api/auth/me", token=token)
        username = me.get("data", {}).get("username", "")
        print(("PASS " if status == 200 and username.startswith("oidcuser") else "FAIL ") + "OIDC 会话可读身份：" + username)
        status, replay = request("POST", APP + "/api/auth/oidc/exchange", {"code": code})
        print(("PASS " if status == 401 else "FAIL ") + "OIDC 一次性 code 重放被拒")
        return 0 if status == 401 and username.startswith("oidcuser") else 1
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
        idp.shutdown()
        idp.server_close()
        if DATABASE_PATH.exists():
            import sqlite3

            try:
                connection = sqlite3.connect(DATABASE_PATH)
                rows = connection.execute("SELECT username, email, dept_id FROM sys_user WHERE email = 'oidc.user@example.com'").fetchall()
                print(("PASS " if rows else "FAIL ") + "OIDC 自动建号落库：" + json.dumps(rows, ensure_ascii=False))
                connection.close()
            finally:
                DATABASE_PATH.unlink()


if __name__ == "__main__":
    sys.exit(main())
