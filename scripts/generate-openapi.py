#!/usr/bin/env python3
"""从源码路由字面量生成 OpenAPI 3.1 契约。

只依赖 Python 标准库。路由来源是 src 下 router.get/post/put/del/patch 以及流式变体的
字符串字面量；摘要、请求/响应 schema 与安全要求来自 docs/openapi-notes.json。生成物写入
frontend/public/openapi.json，用 --check 与现有文件逐字节比较，供 CI 拦截漂移。
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = REPO_ROOT / "src"
NOTES_PATH = REPO_ROOT / "docs" / "openapi-notes.json"
OUTPUT_PATH = REPO_ROOT / "frontend" / "public" / "openapi.json"

ROUTE_PATTERN = re.compile(r'router\.(get|post|put|del|patch|postStreaming|putStreaming)\(\s*"([^"]+)"')

METHODS = {
    "get": "get",
    "post": "post",
    "put": "put",
    "del": "delete",
    "patch": "patch",
    "postStreaming": "post",
    "putStreaming": "put",
}

# 浏览器与自动化 API 的契约不含 Agent 通道、WebSocket、SSE 与探针；通配路由没有可枚举的路径
SKIP_PREFIXES = ("/api/agent", "/api/ws", "/api/events", "/api/proxy")
SKIP_EXACT = {"/healthz", "/readyz"}

# 前缀到 tag 的映射；未命中的路径直接报错，避免新前缀悄悄落进错误的分类
TAG_RULES = (
    ("/api/auth", "auth"),
    ("/api/system", "system"),
    ("/api/health", "system"),
    ("/api/openapi.json", "system"),
    ("/api/nodes", "device"),
    ("/api/node-groups", "device"),
    ("/api/enrollment-tokens", "device"),
    ("/api/telemetry", "fleet"),
    ("/api/alarms", "fleet"),
    ("/api/alarm-rules", "fleet"),
    ("/api/commands", "fleet"),
    ("/api/firmware", "fleet"),
    ("/api/monitor", "monitor"),
    ("/api/dashboard", "dashboard"),
    ("/api/ops", "ops"),
)

TAGS = (
    {"name": "auth", "description": "登录、会话、单点登录与两步验证"},
    {"name": "system", "description": "系统管理、API 令牌、健康检查与契约文档"},
    {"name": "device", "description": "节点、节点分组、注册令牌与设备影子"},
    {"name": "fleet", "description": "遥测、告警、命令与固件"},
    {"name": "monitor", "description": "操作日志、登录日志与统计"},
    {"name": "dashboard", "description": "工作台汇总"},
    {"name": "ops", "description": "运维总览与协议状态"},
)

UTC_TIME_DESCRIPTION = "UTC 时间，格式 YYYY-MM-DD HH:MM:SS"


class GeneratorError(RuntimeError):
    """生成过程中的可报告错误。"""


SCHEMAS = {
    "Envelope": {
        "type": "object",
        "description": "统一响应包络；data 的形状由具体接口决定",
        "required": ["code", "message", "data", "traceId"],
        "properties": {
            "code": {"type": "integer", "format": "int32", "description": "业务码；成功为 200，失败与 HTTP 状态一致"},
            "message": {"type": "string", "description": "面向调用方的说明"},
            "data": {"description": "载荷；失败时固定为 null"},
            "traceId": {"type": "string", "description": "请求标识，与响应头 x-request-id 一致"},
        },
        "additionalProperties": False,
    },
    "Error": {
        "type": "object",
        "description": "失败响应包络；data 固定为 null",
        "required": ["code", "message", "data", "traceId"],
        "properties": {
            "code": {"type": "integer", "format": "int32", "description": "业务码，与 HTTP 状态一致"},
            "message": {"type": "string", "description": "面向调用方的说明"},
            "data": {"type": "null", "description": "失败响应固定为 null"},
            "traceId": {"type": "string", "description": "请求标识，与响应头 x-request-id 一致"},
        },
        "additionalProperties": False,
    },
    "PageResult": {
        "type": "object",
        "description": "分页载荷；rows 的元素形状由具体接口决定",
        "required": ["pageNum", "pageSize", "total", "rows"],
        "properties": {
            "pageNum": {"type": "integer", "format": "int32", "minimum": 1},
            "pageSize": {"type": "integer", "format": "int32", "minimum": 1, "maximum": 100},
            "total": {"type": "integer", "format": "int64", "minimum": 0},
            "rows": {"type": "array", "items": {}},
        },
    },
    "LoginRequest": {
        "type": "object",
        "required": ["username", "password", "captcha", "captchaUuid"],
        "properties": {
            "username": {"type": "string"},
            "password": {"type": "string", "format": "password"},
            "captcha": {"type": "string", "description": "图形验证码答案"},
            "captchaUuid": {"type": "string", "description": "图形验证码标识"},
            "tenantCode": {"type": "string", "default": "default", "description": "租户编码，缺省为 default"},
        },
        "additionalProperties": False,
    },
    "LoginResponse": {
        "description": "登录结果；启用两步验证时先返回挑战，完成验证后再返回会话令牌",
        "oneOf": [
            {
                "type": "object",
                "required": ["token", "expiresIn"],
                "properties": {
                    "token": {"type": "string", "description": "会话令牌"},
                    "expiresIn": {"type": "integer", "format": "int64", "description": "令牌有效秒数"},
                },
                "additionalProperties": False,
            },
            {"$ref": "#/components/schemas/MfaChallenge"},
        ],
    },
    "MfaChallenge": {
        "type": "object",
        "description": "两步验证挑战；拿到 challengeToken 后调用 /api/auth/mfa/verify 换取会话令牌",
        "required": ["mfaRequired", "challengeToken", "expiresIn"],
        "properties": {
            "mfaRequired": {"const": True},
            "challengeToken": {"type": "string", "description": "一次性挑战令牌"},
            "expiresIn": {"type": "integer", "format": "int64", "description": "挑战有效秒数"},
        },
        "additionalProperties": False,
    },
    "MfaStatus": {
        "type": "object",
        "required": ["enabled", "recoveryCodesRemaining"],
        "properties": {
            "enabled": {"type": "boolean"},
            "recoveryCodesRemaining": {"type": "integer", "format": "int32", "minimum": 0},
        },
        "additionalProperties": False,
    },
    "MfaSetupRequest": {
        "type": "object",
        "required": ["password"],
        "properties": {"password": {"type": "string", "format": "password", "description": "当前登录口令"}},
        "additionalProperties": False,
    },
    "MfaSetupResponse": {
        "type": "object",
        "required": ["secret", "otpauthUri"],
        "properties": {
            "secret": {"type": "string", "description": "TOTP 共享密钥，只在本响应出现"},
            "otpauthUri": {"type": "string", "description": "供验证器扫描的 otpauth URI"},
        },
        "additionalProperties": False,
    },
    "MfaEnableRequest": {
        "type": "object",
        "required": ["password", "code"],
        "properties": {
            "password": {"type": "string", "format": "password", "description": "当前登录口令"},
            "code": {"type": "string", "description": "验证器生成的 6 位动态码"},
        },
        "additionalProperties": False,
    },
    "MfaVerifyRequest": {
        "type": "object",
        "required": ["challengeToken", "code"],
        "properties": {
            "challengeToken": {"type": "string", "description": "登录响应里的挑战令牌"},
            "code": {"type": "string", "description": "动态码或恢复码"},
        },
        "additionalProperties": False,
    },
    "MfaRecoveryCodes": {
        "type": "object",
        "required": ["recoveryCodes"],
        "properties": {
            "recoveryCodes": {"type": "array", "items": {"type": "string"}, "description": "恢复码明文，只在本响应出现"},
        },
        "additionalProperties": False,
    },
    "PasswordChangeRequest": {
        "type": "object",
        "required": ["oldPassword", "newPassword"],
        "properties": {
            "oldPassword": {"type": "string", "format": "password"},
            "newPassword": {"type": "string", "format": "password"},
        },
        "additionalProperties": False,
    },
    "CaptchaChallenge": {
        "type": "object",
        "required": ["uuid", "imageSvg"],
        "properties": {
            "uuid": {"type": "string", "description": "验证码标识，登录时回传为 captchaUuid"},
            "imageSvg": {"type": "string", "description": "SVG 图片内容"},
            "code": {"type": "string", "description": "仅开发档返回的答案"},
        },
        "additionalProperties": False,
    },
    "ApiToken": {
        "type": "object",
        "description": "API 令牌摘要；明文 token 只在创建或轮换响应中出现一次",
        "required": ["id", "name", "prefix", "scopes", "expiresAt", "lastUsedAt", "revokedAt", "createdAt"],
        "properties": {
            "id": {"type": "integer", "format": "int64"},
            "name": {"type": "string"},
            "prefix": {"type": "string", "description": "明文前缀，供列表辨识"},
            "scopes": {"type": "array", "items": {"type": "string"}, "description": "权限码列表"},
            "expiresAt": {"type": "string", "description": UTC_TIME_DESCRIPTION},
            "lastUsedAt": {"type": "string", "description": UTC_TIME_DESCRIPTION + "；从未使用为空串"},
            "revokedAt": {"type": "string", "description": UTC_TIME_DESCRIPTION + "；未吊销为空串"},
            "createdAt": {"type": "string", "description": UTC_TIME_DESCRIPTION},
            "token": {"type": "string", "description": "完整明文令牌，只在创建或轮换响应中出现一次"},
            "previousValidUntil": {"type": "string", "description": "轮换后旧令牌仍可用的截止时间"},
        },
        "additionalProperties": False,
    },
    "ApiTokenList": {"type": "array", "items": {"$ref": "#/components/schemas/ApiToken"}},
    "ApiTokenCreateRequest": {
        "type": "object",
        "required": ["name", "scopes"],
        "properties": {
            "name": {"type": "string", "minLength": 1, "maxLength": 64},
            "scopes": {"type": "array", "minItems": 1, "items": {"type": "string"}, "description": "不得超出当前用户的权限"},
            "expiresInSeconds": {
                "type": "integer",
                "format": "int32",
                "minimum": 300,
                "maximum": 31536000,
                "default": 2592000,
                "description": "有效期秒数，缺省 30 天",
            },
        },
        "additionalProperties": False,
    },
    "Node": {
        "type": "object",
        "required": ["id", "tenantId", "name", "status", "agentVersion", "version"],
        "properties": {
            "id": {"type": "integer", "format": "int64"},
            "tenantId": {"type": "integer", "format": "int64"},
            "name": {"type": "string"},
            "status": {"type": "string", "enum": ["online", "offline"]},
            "agentVersion": {"type": "string"},
            "certFingerprint": {"type": "string", "description": "Agent 叶证书 SHA-256；未登记为空串"},
            "certNotAfter": {"type": "string", "description": UTC_TIME_DESCRIPTION + "；可空"},
            "certRevokedAt": {"type": "string", "description": "证书吊销时间；未吊销为空串"},
            "labels": {"type": ["object", "null"], "additionalProperties": {"type": "string"}},
            "lastSeen": {"type": "string", "description": "最近心跳时间；从未心跳为空串"},
            "groupId": {"type": "integer", "format": "int64"},
            "groupName": {"type": "string"},
            "version": {"type": "integer", "format": "int64", "description": "乐观锁版本"},
            "desiredConfigVersion": {"type": "integer", "format": "int64", "description": "仅节点详情返回"},
            "reportedConfigVersion": {"type": "integer", "format": "int64", "description": "仅节点详情返回"},
            "configInSync": {"type": "boolean", "description": "仅节点详情返回"},
        },
        "additionalProperties": False,
    },
    "NodePage": {
        "allOf": [
            {"$ref": "#/components/schemas/PageResult"},
            {
                "type": "object",
                "properties": {"rows": {"type": "array", "items": {"$ref": "#/components/schemas/Node"}}},
            },
        ]
    },
    "NodeCreateRequest": {
        "type": "object",
        "required": ["name"],
        "properties": {
            "name": {"type": "string"},
            "labels": {"type": ["object", "string"], "description": "标签对象，或标签对象的 JSON 文本"},
            "groupId": {"type": "integer", "format": "int64", "default": 200},
            "tenantId": {"type": "integer", "format": "int64", "description": "仅平台管理员可指定"},
        },
        "additionalProperties": False,
    },
    "NodeUpdateRequest": {
        "type": "object",
        "required": ["version"],
        "properties": {
            "name": {"type": "string"},
            "labels": {"type": ["object", "string"], "description": "标签对象，或标签对象的 JSON 文本"},
            "groupId": {"type": "integer", "format": "int64"},
            "version": {"type": "integer", "format": "int64", "description": "当前乐观锁版本；name、labels 与 groupId 至少给一个"},
        },
        "additionalProperties": False,
    },
    "TelemetryPoint": {
        "type": "object",
        "required": ["metric", "value", "unit", "collectedAt"],
        "properties": {
            "metric": {"type": "string"},
            "value": {"type": "number", "format": "double"},
            "unit": {"type": "string"},
            "collectedAt": {"type": "string", "description": UTC_TIME_DESCRIPTION},
        },
        "additionalProperties": False,
    },
    "TelemetryPointList": {"type": "array", "items": {"$ref": "#/components/schemas/TelemetryPoint"}},
    "Alarm": {
        "type": "object",
        "required": ["id", "ruleId", "nodeId", "nodeName", "level", "metric", "value", "threshold", "status", "message", "triggeredAt"],
        "properties": {
            "id": {"type": "integer", "format": "int64"},
            "ruleId": {"type": "integer", "format": "int64"},
            "nodeId": {"type": "integer", "format": "int64"},
            "nodeName": {"type": "string"},
            "level": {"type": "string", "enum": ["info", "warning", "critical"]},
            "metric": {"type": "string"},
            "value": {"type": "number", "format": "double"},
            "threshold": {"type": "number", "format": "double"},
            "status": {"type": "string", "enum": ["open", "acknowledged", "resolved"]},
            "message": {"type": "string"},
            "triggeredAt": {"type": "string", "description": UTC_TIME_DESCRIPTION},
            "acknowledgedAt": {"type": "string", "description": "可空"},
            "resolvedAt": {"type": "string", "description": "可空"},
        },
        "additionalProperties": False,
    },
    "AlarmPage": {
        "allOf": [
            {"$ref": "#/components/schemas/PageResult"},
            {
                "type": "object",
                "required": ["openCount"],
                "properties": {
                    "rows": {"type": "array", "items": {"$ref": "#/components/schemas/Alarm"}},
                    "openCount": {"type": "integer", "format": "int64", "description": "未恢复告警总数"},
                },
            },
        ]
    },
    "AlarmRuleList": {"type": "array", "items": {"$ref": "#/components/schemas/AlarmRule"}},
    "AlarmRule": {
        "type": "object",
        "required": ["id", "name", "nodeId", "metric", "comparison", "threshold", "level", "enabled", "cooldownSeconds"],
        "properties": {
            "id": {"type": "integer", "format": "int64"},
            "name": {"type": "string"},
            "nodeId": {"type": "integer", "format": "int64", "description": "0 表示全部节点"},
            "metric": {"type": "string"},
            "comparison": {"type": "string", "enum": [">", ">=", "<", "<=", "=="]},
            "threshold": {"type": "number", "format": "double"},
            "level": {"type": "string", "enum": ["info", "warning", "critical"]},
            "enabled": {"type": "boolean"},
            "cooldownSeconds": {"type": "integer", "format": "int32"},
            "createTime": {"type": "string", "description": UTC_TIME_DESCRIPTION},
        },
        "additionalProperties": False,
    },
    "Command": {
        "type": "object",
        "required": ["id", "nodeId", "type", "payload", "status", "timeoutSeconds", "createdAt"],
        "properties": {
            "id": {"type": "integer", "format": "int64"},
            "nodeId": {"type": "integer", "format": "int64"},
            "nodeName": {"type": "string"},
            "type": {"type": "string"},
            "payload": {"type": "string", "description": "载荷 JSON 文本"},
            "status": {"type": "string", "enum": ["pending", "running", "succeeded", "failed", "timeout", "canceled"]},
            "result": {"type": "string", "description": "结果 JSON 文本"},
            "message": {"type": "string"},
            "createdAt": {"type": "string", "description": UTC_TIME_DESCRIPTION},
            "dispatchedAt": {"type": "string", "description": "可空"},
            "finishedAt": {"type": "string", "description": "可空"},
            "timeoutSeconds": {"type": "integer", "format": "int32"},
            "scheduledAt": {"type": "string", "description": "计划执行时间；空表示立即"},
            "batchId": {"type": "integer", "format": "int64", "description": "0 表示单条命令"},
        },
        "additionalProperties": False,
    },
    "CommandPage": {
        "allOf": [
            {"$ref": "#/components/schemas/PageResult"},
            {
                "type": "object",
                "properties": {"rows": {"type": "array", "items": {"$ref": "#/components/schemas/Command"}}},
            },
        ]
    },
    "CommandCreateRequest": {
        "type": "object",
        "required": ["nodeId", "type"],
        "properties": {
            "nodeId": {"type": "integer", "format": "int64"},
            "type": {"type": "string"},
            "payload": {"type": "string", "description": "载荷 JSON 文本，缺省 {}"},
            "timeoutSeconds": {"type": "integer", "format": "int32", "minimum": 1, "maximum": 3600, "default": 60},
        },
        "additionalProperties": False,
    },
    "CommandBatchRequest": {
        "type": "object",
        "required": ["nodeIds", "type"],
        "properties": {
            "nodeIds": {"type": "array", "minItems": 1, "maxItems": 100, "items": {"type": "integer", "format": "int64"}},
            "type": {"type": "string"},
            "payload": {"type": "string", "description": "载荷 JSON 文本"},
            "timeoutSeconds": {"type": "integer", "format": "int32", "minimum": 1, "maximum": 3600, "default": 60},
            "scheduledAt": {"type": "string", "description": UTC_TIME_DESCRIPTION + "；省略表示立即"},
        },
        "additionalProperties": False,
    },
    "CommandBatchResponse": {
        "type": "object",
        "required": ["batchId", "commands"],
        "properties": {
            "batchId": {"type": "integer", "format": "int64"},
            "commands": {"type": "array", "items": {"$ref": "#/components/schemas/Command"}},
        },
        "additionalProperties": False,
    },
    "Firmware": {
        "type": "object",
        "required": ["id", "tenantId", "name", "version", "fileName", "sizeBytes", "sha256", "signed", "createdAt"],
        "properties": {
            "id": {"type": "integer", "format": "int64"},
            "tenantId": {"type": "integer", "format": "int64"},
            "name": {"type": "string"},
            "version": {"type": "string"},
            "fileName": {"type": "string"},
            "contentType": {"type": "string"},
            "sizeBytes": {"type": "integer", "format": "int64"},
            "sha256": {"type": "string", "description": "十六进制摘要"},
            "signed": {"type": "boolean"},
            "signatureKeyId": {"type": "string"},
            "signatureAlgorithm": {"type": "string"},
            "createdAt": {"type": "string", "description": UTC_TIME_DESCRIPTION},
            "createdBy": {"type": "integer", "format": "int64"},
        },
        "additionalProperties": False,
    },
    "FirmwarePage": {
        "allOf": [
            {"$ref": "#/components/schemas/PageResult"},
            {
                "type": "object",
                "properties": {"rows": {"type": "array", "items": {"$ref": "#/components/schemas/Firmware"}}},
            },
        ]
    },
    "FirmwarePolicy": {
        "type": "object",
        "required": ["requireSignature", "trustedKeyIds"],
        "properties": {
            "requireSignature": {"type": "boolean"},
            "trustedKeyIds": {"type": "array", "items": {"type": "string"}},
        },
        "additionalProperties": False,
    },
    "OpenApiDocument": {
        "type": "object",
        "description": "OpenAPI 3.1 文档本身，形状由 OpenAPI 规范定义",
        "additionalProperties": True,
    },
}


def strip_comments(source: str) -> str:
    """去掉 C++ 行注释与块注释，同时保留字符串字面量里的 // 与 /*。"""
    result: list[str] = []
    index = 0
    length = len(source)
    while index < length:
        character = source[index]
        raw_prefix = next((prefix for prefix in ('u8R"', 'uR"', 'UR"', 'LR"', 'R"') if source.startswith(prefix, index)), None)
        if raw_prefix is not None:
            delimiter_end = index + len(raw_prefix)
            opening = source.find("(", delimiter_end)
            delimiter = source[delimiter_end:opening] if opening >= 0 else ""
            if opening >= 0 and len(delimiter) <= 16 and not any(mark in delimiter for mark in " ()\\\t\r\n"):
                closing = source.find(")" + delimiter + '"', opening + 1)
                if closing >= 0:
                    end = closing + len(delimiter) + 2
                    result.append(source[index:end])
                    index = end
                    continue
        if character in {"\"", "'"}:
            quote = character
            result.append(character)
            index += 1
            while index < length:
                current = source[index]
                result.append(current)
                if current == "\\" and index + 1 < length:
                    result.append(source[index + 1])
                    index += 2
                    continue
                index += 1
                if current == quote:
                    break
            continue
        if character == "/" and index + 1 < length and source[index + 1] == "/":
            while index < length and source[index] != "\n":
                index += 1
            continue
        if character == "/" and index + 1 < length and source[index + 1] == "*":
            index += 2
            while index + 1 < length and not (source[index] == "*" and source[index + 1] == "/"):
                index += 1
            index = min(index + 2, length)
            continue
        result.append(character)
        index += 1
    return "".join(result)


def normalize_path(path: str) -> str:
    """把 :name 参数段归一为 OpenAPI 的 {name}。"""
    return re.sub(r"(?<=/):([A-Za-z_][A-Za-z0-9_]*)", r"{\1}", path)


def should_skip(path: str) -> bool:
    """判定不属于浏览器契约的路由。"""
    if "*" in path or path in SKIP_EXACT:
        return True
    return any(path == prefix or path.startswith(prefix + "/") for prefix in SKIP_PREFIXES)


def tag_for(path: str) -> str:
    """按前缀取 tag；未登记的前缀是生成器错误。"""
    for prefix, tag in TAG_RULES:
        if path == prefix or path.startswith(prefix + "/"):
            return tag
    raise GeneratorError(f"路由 {path} 没有对应的 tag，请在 TAG_RULES 中补前缀")


def scan_routes() -> dict[str, dict[str, dict]]:
    """扫描源码，返回 path -> method -> note 的骨架。"""
    routes: dict[str, dict[str, dict]] = {}
    for source in sorted(SOURCE_DIR.rglob("*.cpp")):
        text = strip_comments(source.read_text(encoding="utf-8"))
        for match in ROUTE_PATTERN.finditer(text):
            method = METHODS[match.group(1)]
            path = normalize_path(match.group(2).split("?", 1)[0])
            if should_skip(path):
                continue
            if not path.startswith("/api/"):
                raise GeneratorError(f"{source.relative_to(REPO_ROOT)} 的路由 {path} 不在 /api 下")
            routes.setdefault(path, {}).setdefault(method, {"source": source.relative_to(REPO_ROOT).as_posix()})
    if not routes:
        raise GeneratorError(f"{SOURCE_DIR} 下没有扫描到任何路由字面量")
    return routes


def load_notes() -> dict[str, dict[str, dict]]:
    """读取并校验手写摘要；notes 里出现未注册的方法说明文件已过期。"""
    try:
        data = json.loads(NOTES_PATH.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise GeneratorError(f"缺少 {NOTES_PATH.relative_to(REPO_ROOT)}") from error
    except json.JSONDecodeError as error:
        raise GeneratorError(f"{NOTES_PATH.relative_to(REPO_ROOT)} 不是合法 JSON：{error}") from error
    paths = data.get("paths") if isinstance(data, dict) else None
    if not isinstance(paths, dict):
        raise GeneratorError(f"{NOTES_PATH.relative_to(REPO_ROOT)} 的顶层必须是包含 paths 的对象")
    for path, methods in paths.items():
        if not isinstance(methods, dict):
            raise GeneratorError(f"openapi-notes 的 {path} 必须是方法到摘要的对象")
        for method in methods:
            if method not in {"get", "post", "put", "delete", "patch"}:
                raise GeneratorError(f"openapi-notes 的 {path} 含未知方法 {method}")
    return paths


def operation_security(note: dict, location: str) -> list[dict]:
    """把 notes 里的方案名列表转成 OpenAPI security 数组；缺省要求 Bearer。"""
    configured = note.get("security")
    if configured is None:
        return [{"bearerAuth": []}]
    if not isinstance(configured, list):
        raise GeneratorError(f"{location} 的 security 必须是数组")
    requirements: list[dict] = []
    for item in configured:
        if isinstance(item, str):
            requirements.append({item: []})
        elif isinstance(item, dict):
            requirements.append(item)
        else:
            raise GeneratorError(f"{location} 的 security 元素必须是方案名或安全要求对象")
    return requirements


def response_body_schema(response_schema: str | None, note: dict) -> dict:
    """合成 200 响应体。

    responseSchema 描述的是包络里的 data；省略或写 Envelope 时直接复用统一包络。
    envelope 为 false 表示整个响应体就是该 schema，例如 /api/openapi.json 返回的原始文档。
    """
    if response_schema is None or response_schema == "Envelope":
        return {"$ref": "#/components/schemas/Envelope"}
    reference = {"$ref": f"#/components/schemas/{response_schema}"}
    if note.get("envelope", True) is False:
        return reference
    return {
        "allOf": [
            {"$ref": "#/components/schemas/Envelope"},
            {"type": "object", "properties": {"data": reference}},
        ]
    }


def build_paths(routes: dict[str, dict[str, dict]], notes: dict[str, dict[str, dict]]) -> dict[str, dict[str, dict]]:
    """合成按 path、method 排序的 paths 对象。"""
    paths: dict[str, dict[str, dict]] = {}
    for path in sorted(routes):
        tag = tag_for(path)
        for method in sorted(routes[path]):
            note = notes.get(path, {}).get(method, {})
            request_schema = note.get("requestSchema")
            response_schema = note.get("responseSchema")
            if request_schema is not None and request_schema not in SCHEMAS:
                raise GeneratorError(f"{method.upper()} {path} 引用了未知请求 schema {request_schema}")
            if response_schema is not None and response_schema not in SCHEMAS:
                raise GeneratorError(f"{method.upper()} {path} 引用了未知响应 schema {response_schema}")
            operation: dict[str, object] = {
                "tags": [tag],
                "summary": note.get("summary") or f"{tag} {method.upper()} {path}",
                "security": operation_security(note, f"{method.upper()} {path}"),
            }
            if note.get("description"):
                operation["description"] = note["description"]
            if request_schema is not None:
                operation["requestBody"] = {
                    "required": True,
                    "content": {"application/json": {"schema": {"$ref": f"#/components/schemas/{request_schema}"}}},
                }
            operation["responses"] = {
                "200": {
                    "description": "成功",
                    "content": {"application/json": {"schema": response_body_schema(response_schema, note)}},
                },
                "default": {
                    "description": "统一错误包络",
                    "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Error"}}},
                },
            }
            paths.setdefault(path, {})[method] = operation
    return paths


def validate_notes(routes: dict[str, dict[str, dict]], notes: dict[str, dict[str, dict]]) -> None:
    """notes 中出现源码里不存在的路由时直接失败，避免契约条目悬空。"""
    for path, methods in notes.items():
        for method in methods:
            if path not in routes or method not in routes[path]:
                raise GeneratorError(f"openapi-notes 中的 {method.upper()} {path} 在源码里找不到注册")


def render() -> str:
    """生成完整文档文本；同一份源码与 notes 总是得到同一串字节。"""
    routes = scan_routes()
    notes = load_notes()
    validate_notes(routes, notes)
    paths = build_paths(routes, notes)
    document = {
        "openapi": "3.1.0",
        "info": {
            "title": "EdgePulse API",
            "version": "1.0.0",
            "description": "由 scripts/generate-openapi.py 从 src 路由字面量与 docs/openapi-notes.json 生成；不要手工编辑 frontend/public/openapi.json。",
        },
        "servers": [{"url": "/"}],
        "tags": list(TAGS),
        "paths": paths,
        "components": {
            "securitySchemes": {
                "bearerAuth": {
                    "type": "http",
                    "scheme": "bearer",
                    "description": "浏览器会话令牌或 API 令牌；以 Authorization: Bearer <token> 发送",
                }
            },
            "schemas": SCHEMAS,
        },
    }
    return json.dumps(document, ensure_ascii=False, indent=2) + "\n"


def check(rendered: str) -> int:
    """逐字节比较生成物；只报首个差异位置，不打印大段 diff。"""
    relative = OUTPUT_PATH.relative_to(REPO_ROOT).as_posix()
    if not OUTPUT_PATH.exists():
        print(f"OpenAPI 契约漂移：缺少 {relative}；请运行 python scripts/generate-openapi.py", file=sys.stderr)
        return 1
    actual = OUTPUT_PATH.read_bytes()
    expected = rendered.encode("utf-8")
    if actual == expected:
        print(f"OpenAPI 契约无漂移：{relative}")
        return 0
    actual_lines = actual.decode("utf-8", errors="replace").splitlines()
    expected_lines = rendered.splitlines()
    for index in range(max(len(actual_lines), len(expected_lines))):
        actual_line = actual_lines[index] if index < len(actual_lines) else "<文件结束>"
        expected_line = expected_lines[index] if index < len(expected_lines) else "<文件结束>"
        if actual_line != expected_line:
            print(f"OpenAPI 契约漂移：{relative}:{index + 1}", file=sys.stderr)
            print(f"  期望：{expected_line.strip()[:120]}", file=sys.stderr)
            print(f"  实际：{actual_line.strip()[:120]}", file=sys.stderr)
            return 1
    print(f"OpenAPI 契约漂移：{relative}:文件末尾（换行或编码不一致）", file=sys.stderr)
    return 1


def configure_stdio() -> None:
    """Windows 的重定向输出默认不是 UTF-8，中文提示在 CI 日志里会直接抛编码错误。"""
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    parser = argparse.ArgumentParser(description="从源码路由字面量生成 OpenAPI 3.1 契约")
    parser.add_argument("--check", action="store_true", help="只校验 frontend/public/openapi.json 是否与源码一致，不写文件")
    arguments = parser.parse_args(argv)
    try:
        rendered = render()
    except GeneratorError as error:
        print(f"生成 OpenAPI 失败：{error}", file=sys.stderr)
        return 2
    if arguments.check:
        return check(rendered)
    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_PATH.write_text(rendered, encoding="utf-8", newline="\n")
    document = json.loads(rendered)
    operation_count = sum(len(methods) for methods in document["paths"].values())
    print(f"已生成 {OUTPUT_PATH.relative_to(REPO_ROOT).as_posix()}：{len(document['paths'])} 个路径，{operation_count} 个操作")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
