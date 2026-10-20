# EdgePulse API（P0）

- 版本：P0（2026-10-20）
- 基址：开发档 `http://127.0.0.1:8080/api`；控制台 HTTPS `https://<host>:8443/api`
- Agent 接口只在 `9443` mTLS 监听器上提供，不出现在 8080/8443

## 1. 统一约定

成功：

```json
{ "code": 200, "message": "ok", "data": {}, "traceId": "0001-0000000000000001" }
```

失败：

```json
{ "code": 401, "message": "用户名或密码错误", "data": null, "traceId": "0001-0000000000000002" }
```

- HTTP 状态码与 `code` 同值；`data` 失败时为 `null`。
- 响应回写 `x-request-id`；`traceId` 取自该请求的标识。
- 分页：`{ "pageNum": 1, "pageSize": 10, "total": 0, "rows": [] }`；`pageSize` 上限 100。
- 认证：`Authorization: Bearer <token>`。
- 中文错误文案不包含驱动内部原文、口令、token 或文件路径。

## 2. 认证

### GET /auth/captcha

匿名。开发档：

```json
{ "uuid": "…", "imageSvg": "<svg …>", "code": "A7K2" }
```

生产档不返回 `code`。`uuid` 一次性，2 分钟有效；按 IP 每分钟最多 30 次，超限 429。

### POST /auth/login

```json
{ "username": "admin", "password": "admin123", "captcha": "A7K2", "captchaUuid": "…" }
```

成功：

```json
{ "token": "<64 位十六进制>", "expiresIn": 7200 }
```

失败：验证码错误 400；用户名/口令错误 401；限流 429。

### POST /auth/logout

需要 Bearer。成功 `data: null`；删除会话。

### GET /auth/me

需要 Bearer。返回：

```json
{
  "id": 1,
  "userId": 1,
  "username": "admin",
  "nickname": "超级管理员",
  "deptId": 0,
  "deptName": "",
  "roles": ["super_admin"],
  "permissions": ["device:node:list", "device:node:write"]
}
```

### GET /auth/routes

需要 Bearer。返回当前用户可见菜单树：

```json
{
  "routes": [
    {
      "id": 1000,
      "parentId": 0,
      "name": "边缘运维",
      "type": "dir",
      "path": "/device",
      "component": "",
      "icon": "layers",
      "permission": "",
      "children": [
        {
          "id": 1001,
          "parentId": 1000,
          "name": "节点",
          "type": "menu",
          "path": "/device/nodes",
          "component": "device/Nodes",
          "icon": "server",
          "permission": "device:node:list",
          "children": []
        }
      ]
    }
  ]
}
```

### PUT /auth/password

需要 Bearer：

```json
{ "oldPassword": "admin123", "newPassword": "new-pass-123" }
```

新口令长度 6..64；原口令错误 400；成功 `data: null`。

## 3. 节点

权限：列表 `device:node:list`；创建/更新/删除 `device:node:write`。匿名 401，无权限 403。

### GET /nodes

查询：`pageNum`、`pageSize`、`keyword`、`status`（`online` / `offline`）。返回分页；行形状：

```json
{
  "id": 1879577769666412544,
  "name": "edgepulse-agent",
  "status": "online",
  "agentVersion": "0.1.0",
  "certFingerprint": "<64 位小写十六进制>",
  "labels": { "probe": "true" },
  "lastSeen": "2026-10-20T14:02:11Z",
  "version": 3
}
```

### POST /nodes

```json
{ "name": "edge-sh-01", "labels": { "site": "shanghai" } }
```

`name` 1..128 字符且唯一；`labels` 可省略或为 JSON 对象。成功返回节点；重名 409。

### PUT /nodes/{id}

```json
{ "name": "edge-sh-01b", "version": 1 }
```

`version` 必填；`name` 与 `labels` 至少给一个。版本不匹配 409；节点不存在 404。

### DELETE /nodes/{id}

软删除；不存在或已删除 404。

## 4. Agent（仅 9443 mTLS）

TLS 要求客户端证书由开发/生产 CA 签发；无证书或陌生 CA 在握手期被拒。注册时 `certFingerprint` 还必须命中服务端白名单（白名单为空时只做 CA 链校验）。

### POST /agent/register

```json
{ "name": "edgepulse-agent", "agentVersion": "0.1.0", "certFingerprint": "<sha256>", "labels": { "zone": "edge" } }
```

按名称更新或新建节点，状态置 `online`，返回节点 JSON。指纹不在白名单 403；字段不合法 400。

### POST /agent/heartbeat

```json
{ "nodeId": 1879577769666412544, "agentVersion": "0.1.0" }
```

更新 `last_seen`、`status=online`、`version+1`；节点不存在 404。注册与心跳都会发布 `node.status` 到 WebSocket 的 `nodes` 主题。

## 5. 浏览器 WebSocket

`GET /api/ws`（Upgrade）。Origin 必须在配置白名单内（空列表只允许同源）；非浏览器客户端可不带 Origin。

首条消息必须是一次性认证：

```json
{ "type": "auth", "token": "<会话 token>" }
```

服务端 5 秒内未收到有效认证会以 1008 关闭；成功后：

```json
{ "type": "auth", "ok": true }
```

随后服务端推送：

```json
{ "type": "node.status", "data": { "id": 1, "name": "edgepulse-agent", "status": "online", "version": 4, "...": "..." } }
```

客户端每 25 秒发 `{ "type": "ping" }` 保活；服务端读取但不回业务数据。

## 6. 运维

运维口 `9100` 只监听 `127.0.0.1`：

| 方法 | 路径 | 认证 | 说明 |
|---|---|---|---|
| GET | `/metrics` | Bearer | Prometheus 文本（引擎进程指标 + HTTP 统计） |
| GET | `/healthz` | 匿名 | 进程存活 |
| GET | `/readyz` | 匿名 | 就绪；依赖不可用时 503 |
| GET | `/debug/loops`、`/debug/connections`、`/debug/log-level` | Bearer | 引擎调试端点 |

Bearer 令牌来自 `EDGEPULSE_OPS_BEARER_TOKEN` 或未跟踪的 `var/ops-token`；缺失时服务拒绝启动。

## 7. 健康

`GET /api/health` 匿名：

```json
{ "status": "UP", "time": "2026-10-20T14:02:11Z" }
```

## 8. 错误码

| HTTP / code | 含义 |
|---|---|
| 400 | 请求字段缺失或格式不合法 |
| 401 | 未登录、会话过期、用户名或口令错误 |
| 403 | 已登录但缺少权限，或 Agent 指纹被拒、Origin 被拒 |
| 404 | 资源不存在或已删除 |
| 409 | 节点名重复或乐观锁版本冲突 |
| 429 | 验证码签发或登录限流 |
| 500 | 内部错误（详情只进日志） |
| 503 | 就绪探针依赖不可用 |

## 9. 未实现（P1+）

`/api/system/*`、`/api/monitor/*`、`/api/dashboard/summary`、`/api/devices*`、`/api/telemetry/*`、`/api/alarms*`、`/api/firmware*`、`/api/commands*`、`/api/ops/summary`、`/api/ops/protocols`、`/api/events/logs`（SSE）、Agent WebSocket 与 Agent 遥测/固件接口。
