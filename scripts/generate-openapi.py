#!/usr/bin/env python3
"""从源码路由字面量生成 OpenAPI 3.1 契约。

只依赖 Python 标准库。路由来源是 src 下 router.get/post/put/del/patch 以及流式变体的
字符串字面量；摘要、请求/响应 schema、状态码、媒体类型与安全要求来自
docs/openapi-notes.json。生成物写入 frontend/public/openapi.json，用 --check 与现有文件
逐字节比较，供 CI 拦截漂移。

路径里的 {name} 一律生成 required 的 path 参数；非字面量路由、重复 path+method、
notes 未知键或未知安全方案都让生成失败，只有反向代理的动态前缀在允许清单内。
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

# 同一批入口的非字面量形式也要拦住：只有反向代理的动态前缀允许例外
ROUTER_CALL_PATTERN = re.compile(r"router\.(get|post|put|del|patch|postStreaming|putStreaming|any)\s*\(")
DYNAMIC_ROUTE_ALLOWLIST = {
    ("src/Core/Http/ReverseProxy.cpp", "any", "configurationHandle->prefix"),
}

METHODS = {
    "get": "get",
    "post": "post",
    "put": "put",
    "del": "delete",
    "patch": "patch",
    "postStreaming": "post",
    "putStreaming": "put",
}

PATH_PARAMETER_PATTERN = re.compile(r"\{([^{}]+)\}")
PATH_PARAMETER_NAME_PATTERN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
INTEGER_PATH_PARAMETERS = {"id", "batchId"}

# notes 只接受这些键；写错键名会让契约悄悄缺字段，因此直接失败
NOTE_ROOT_KEYS = {"_comment", "paths"}
NOTE_KEYS = {
    "summary",
    "description",
    "requestSchema",
    "responseSchema",
    "security",
    "envelope",
    "responseStatus",
    "responseContentType",
    "responseDescription",
}
SECURITY_SCHEMES = {"bearerAuth"}
RESPONSE_CONTENT_TYPES = {"application/json", "text/csv"}


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


def path_parameters(path: str) -> list[dict[str, object]]:
    """把路径里的每个 {name} 都声明为必填的 path 参数。"""
    names = PATH_PARAMETER_PATTERN.findall(path)
    if len(names) != len(set(names)):
        raise GeneratorError(f"路由 {path} 重复出现同名路径参数")
    parameters: list[dict[str, object]] = []
    for name in names:
        if not PATH_PARAMETER_NAME_PATTERN.fullmatch(name):
            raise GeneratorError(f"路由 {path} 的路径参数 {name} 不是合法标识符")
        schema: dict[str, object] = {"type": "string"}
        if name in INTEGER_PATH_PARAMETERS:
            schema = {"type": "integer", "format": "int64"}
        parameters.append({"name": name, "in": "path", "required": True, "schema": schema})
    return parameters


def validate_path_parameters(path: str, parameters: list[dict[str, object]]) -> None:
    """生成期自检：{...} 与 required path 参数必须一一对应。"""
    placeholders = set(PATH_PARAMETER_PATTERN.findall(path))
    declared = {str(parameter["name"]) for parameter in parameters if parameter.get("in") == "path" and parameter.get("required") is True}
    if placeholders != declared:
        missing = sorted(placeholders - declared)
        extra = sorted(declared - placeholders)
        raise GeneratorError(f"路由 {path} 的路径参数声明不完整：缺少 {missing}，多出 {extra}")


def reject_non_literal_router_calls(source: Path, text: str) -> None:
    """非字面量路由会让契约漏项；只有反向代理前缀在允许清单里。"""
    source_name = source.relative_to(REPO_ROOT).as_posix()
    for match in ROUTER_CALL_PATTERN.finditer(text):
        argument_start = match.end()
        while argument_start < len(text) and text[argument_start].isspace():
            argument_start += 1
        if argument_start < len(text) and text[argument_start] == '"':
            continue
        argument_end = text.find(",", argument_start)
        if argument_end < 0:
            argument_end = len(text)
        argument = " ".join(text[argument_start:argument_end].split())
        method = match.group(1)
        allowed = next(
            (prefix for file_name, allowed_method, prefix in DYNAMIC_ROUTE_ALLOWLIST if file_name == source_name and allowed_method == method and argument.startswith(prefix)),
            None,
        )
        if allowed is not None:
            continue
        line = text.count("\n", 0, match.start()) + 1
        raise GeneratorError(
            f"{source_name}:{line} 的 router.{method} 路径不是字符串字面量（{argument[:80]}）；"
            "生成器无法静态枚举动态路由，只有反向代理的 configurationHandle->prefix 允许例外"
        )


def scan_routes() -> dict[str, dict[str, dict]]:
    """扫描源码，返回 path -> method -> note 的骨架。"""
    routes: dict[str, dict[str, dict]] = {}
    for source in sorted(SOURCE_DIR.rglob("*.cpp")):
        text = strip_comments(source.read_text(encoding="utf-8"))
        reject_non_literal_router_calls(source, text)
        source_name = source.relative_to(REPO_ROOT).as_posix()
        for match in ROUTE_PATTERN.finditer(text):
            method = METHODS[match.group(1)]
            path = normalize_path(match.group(2).split("?", 1)[0])
            if should_skip(path):
                continue
            if not path.startswith("/api/"):
                raise GeneratorError(f"{source_name} 的路由 {path} 不在 /api 下")
            existing = routes.setdefault(path, {}).get(method)
            if existing is not None:
                raise GeneratorError(f"{method.upper()} {path} 在 {existing['source']} 与 {source_name} 重复注册；同一 path+method 只能有一条字面量路由")
            routes[path][method] = {"source": source_name}
    if not routes:
        raise GeneratorError(f"{SOURCE_DIR} 下没有扫描到任何路由字面量")
    return routes


def load_notes() -> dict[str, dict[str, dict]]:
    """读取并校验手写摘要；未知键、未知方法与未知安全方案都让生成失败。"""
    try:
        data = json.loads(NOTES_PATH.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise GeneratorError(f"缺少 {NOTES_PATH.relative_to(REPO_ROOT)}") from error
    except json.JSONDecodeError as error:
        raise GeneratorError(f"{NOTES_PATH.relative_to(REPO_ROOT)} 不是合法 JSON：{error}") from error
    if not isinstance(data, dict):
        raise GeneratorError(f"{NOTES_PATH.relative_to(REPO_ROOT)} 的顶层必须是对象")
    unknown_root = set(data) - NOTE_ROOT_KEYS
    if unknown_root:
        raise GeneratorError(f"openapi-notes 含未知顶层键：{sorted(unknown_root)}")
    paths = data.get("paths")
    if not isinstance(paths, dict):
        raise GeneratorError(f"{NOTES_PATH.relative_to(REPO_ROOT)} 的顶层必须是包含 paths 的对象")
    for path, methods in paths.items():
        if not isinstance(methods, dict):
            raise GeneratorError(f"openapi-notes 的 {path} 必须是方法到摘要的对象")
        for method, note in methods.items():
            location = f"openapi-notes 的 {method.upper()} {path}"
            if method not in {"get", "post", "put", "delete", "patch"}:
                raise GeneratorError(f"{location} 含未知方法")
            if not isinstance(note, dict):
                raise GeneratorError(f"{location} 必须是对象")
            unknown_keys = set(note) - NOTE_KEYS
            if unknown_keys:
                raise GeneratorError(f"{location} 含未知键：{sorted(unknown_keys)}")
            for key in ("summary", "description", "responseContentType", "responseDescription"):
                if key in note and not isinstance(note[key], str):
                    raise GeneratorError(f"{location} 的 {key} 必须是字符串")
            for key in ("requestSchema", "responseSchema"):
                if key in note and not isinstance(note[key], str):
                    raise GeneratorError(f"{location} 的 {key} 必须是字符串")
            if "envelope" in note and not isinstance(note["envelope"], bool):
                raise GeneratorError(f"{location} 的 envelope 必须是布尔值")
            status = note.get("responseStatus", 200)
            if not isinstance(status, int) or isinstance(status, bool) or not 100 <= status <= 599:
                raise GeneratorError(f"{location} 的 responseStatus 必须是 100 到 599 的整数")
            content_type = note.get("responseContentType")
            if content_type is not None and content_type not in RESPONSE_CONTENT_TYPES:
                raise GeneratorError(f"{location} 的 responseContentType 必须是 {sorted(RESPONSE_CONTENT_TYPES)} 之一")
            if content_type is not None and content_type != "application/json" and "responseSchema" in note:
                raise GeneratorError(f"{location} 的 responseSchema 只适用于 application/json；{content_type} 不能套用 JSON schema")
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
            if item not in SECURITY_SCHEMES:
                raise GeneratorError(f"{location} 引用了未知安全方案 {item}；可选值：{sorted(SECURITY_SCHEMES)}")
            requirements.append({item: []})
        elif isinstance(item, dict):
            for name, scopes in item.items():
                if name not in SECURITY_SCHEMES:
                    raise GeneratorError(f"{location} 引用了未知安全方案 {name}；可选值：{sorted(SECURITY_SCHEMES)}")
                if not isinstance(scopes, list) or not all(isinstance(scope, str) for scope in scopes):
                    raise GeneratorError(f"{location} 的安全方案 {name} 的 scopes 必须是字符串数组")
            requirements.append(item)
        else:
            raise GeneratorError(f"{location} 的 security 元素必须是方案名或安全要求对象")
    return requirements


def response_body_schema(response_schema: str | None, note: dict) -> dict:
    """合成 JSON 响应体。

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


def success_response(note: dict, response_schema: str | None, location: str) -> tuple[str, dict]:
    """按 notes 合成成功响应；状态与媒体类型缺省为 200 application/json。"""
    status = int(note.get("responseStatus", 200))
    content_type = note.get("responseContentType")
    if content_type is None:
        if 300 <= status < 400 or status in (204, 304):
            content_type = None
        else:
            content_type = "application/json"
    if 300 <= status < 400:
        response: dict[str, object] = {
            "description": note.get("responseDescription") or "重定向",
            "headers": {"Location": {"required": True, "schema": {"type": "string"}, "description": "重定向目标"}},
        }
    else:
        response = {"description": note.get("responseDescription") or "成功"}
    if content_type == "application/json":
        response["content"] = {"application/json": {"schema": response_body_schema(response_schema, note)}}
    elif content_type == "text/csv":
        response["content"] = {"text/csv": {"schema": {"type": "string", "description": "CSV 文本，UTF-8 BOM"}}}
    elif content_type is not None:
        raise GeneratorError(f"{location} 的 responseContentType {content_type} 不受支持")
    elif response_schema is not None:
        raise GeneratorError(f"{location} 无响应正文时不能声明 responseSchema")
    return str(status), response


def build_paths(routes: dict[str, dict[str, dict]], notes: dict[str, dict[str, dict]]) -> dict[str, dict[str, dict]]:
    """合成按 path、method 排序的 paths 对象。"""
    paths: dict[str, dict[str, dict]] = {}
    for path in sorted(routes):
        tag = tag_for(path)
        parameters = path_parameters(path)
        validate_path_parameters(path, parameters)
        for method in sorted(routes[path]):
            note = notes.get(path, {}).get(method, {})
            location = f"{method.upper()} {path}"
            request_schema = note.get("requestSchema")
            response_schema = note.get("responseSchema")
            if request_schema is not None and request_schema not in SCHEMAS:
                raise GeneratorError(f"{location} 引用了未知请求 schema {request_schema}")
            if response_schema is not None and response_schema not in SCHEMAS:
                raise GeneratorError(f"{location} 引用了未知响应 schema {response_schema}")
            operation: dict[str, object] = {
                "tags": [tag],
                "summary": note.get("summary") or f"{tag} {method.upper()} {path}",
                "security": operation_security(note, location),
            }
            if note.get("description"):
                operation["description"] = note["description"]
            operation["parameters"] = parameters
            if request_schema is not None:
                operation["requestBody"] = {
                    "required": True,
                    "content": {"application/json": {"schema": {"$ref": f"#/components/schemas/{request_schema}"}}},
                }
            status, response = success_response(note, response_schema, location)
            operation["responses"] = {
                status: response,
                "default": {
                    "description": "统一错误包络",
                    "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Error"}}},
                },
            }
            paths.setdefault(path, {})[method] = operation
    return paths


def validate_generated_paths(paths: dict[str, dict[str, dict]]) -> None:
    """成品自检：每个 {name} 段都必须有对应的 required path 参数。"""
    for path, methods in paths.items():
        placeholders = set(PATH_PARAMETER_PATTERN.findall(path))
        for method, operation in methods.items():
            parameters = operation.get("parameters")
            if not isinstance(parameters, list):
                raise GeneratorError(f"{method.upper()} {path} 缺少 parameters 数组")
            validate_path_parameters(path, parameters)
            if not placeholders:
                continue
            declared = {parameter.get("name") for parameter in parameters if isinstance(parameter, dict)}
            if not placeholders.issubset(declared):
                raise GeneratorError(f"{method.upper()} {path} 的路径参数声明不完整")


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
    validate_generated_paths(paths)
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
