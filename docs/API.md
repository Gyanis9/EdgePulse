# EdgePulse API

- 基础路径：`/api`
- 版本前缀：当前没有 `/v1`，`/api` 直接承载现行契约；版本化前缀仍在 Roadmap 中
- 契约：`GET /api/openapi.json` 返回 OpenAPI 3.1 文档，生成与校验方式见第 9 节
- 认证：除匿名端点外，`Authorization: Bearer <token>`
- 响应包络：`{ "code": 200, "message": "ok", "data": ..., "traceId": "..." }`
- 分页：`{ "pageNum": 1, "pageSize": 10, "total": 0, "rows": [] }`；`pageSize` 上限 100
- 失败时 HTTP 状态为 4xx/5xx，`code` 为同一业务码，`data` 为 `null`
- 请求标识：响应头回写 `x-request-id`
- 时间：请求与响应里的 DATETIME 使用 `YYYY-MM-DD HH:MM:SS`（UTC）

## 1. 健康与认证

| 方法 | 路径 | 权限 | 说明 |
|---|---|---|---|
| GET | `/api/health` | 匿名 | `{status:"UP",time}` |
| GET | `/api/auth/captcha` | 匿名 | `{uuid,imageSvg,code?}`；开发档带 code |
| POST | `/api/auth/login` | 匿名 | `{username,password,captcha,captchaUuid,tenantCode?}` → `{token,expiresIn}`；tenantCode 缺省 `default` |
| POST | `/api/auth/logout` | 登录 | 删除当前会话 |
| GET | `/api/auth/me` | 登录 | `{id,userId,username,nickname,deptId,deptName,tenantId,tenantCode,tenantName,platformAdmin,roles,permissions}` |
| GET | `/api/auth/routes` | 登录 | 动态菜单树；节点形状 `{id,parentId,name,type,path,component,icon,permission,children}` |
| PUT | `/api/auth/password` | 登录 | `{oldPassword,newPassword}` |
| GET | `/api/auth/oidc/enabled` | 匿名 | `{enabled,label}` |
| GET | `/api/auth/oidc/start?return=/path` | 匿名 | 302 到 IdP；return 只接受本站绝对路径 |
| GET | `/api/auth/oidc/callback?code=&state=` | 匿名 | 302 回前端，带一次性 `oidcCode` 或 `oidcError` |
| POST | `/api/auth/oidc/exchange` | 匿名 | `{code}` → `{token,expiresIn,returnPath}`；code 一次性 |

登录失败按 IP 计数，超过 10 次/分钟返回 429；账号在窗口内连续失败达到阈值后锁定，锁定期间返回 423，文案给出配置的锁定时长（分钟，向上取整），不是剩余时间，成功登录或管理员重置会清空失败计数与锁定。验证码一次性、2 分钟过期。新建、重置与改密共用口令策略（最小长度 8–64，可要求大小写、数字与符号）。

## 2. 系统管理

所有写接口要求对应权限码；列表支持 `keyword`、`pageNum`、`pageSize`，导出返回 UTF-8 BOM 的 CSV（`content-type: text/csv; charset=utf-8`，契约里是 `text/csv` 200，不是 JSON 包络）。

| 方法 | 路径 | 权限 |
|---|---|---|
| GET/POST | `/api/system/users` | `system:user:list` / `system:user:add` |
| PUT/DELETE | `/api/system/users/{id}` | `system:user:edit` / `system:user:remove` |
| GET | `/api/system/users/options` | `system:user:list` |
| GET | `/api/system/users/export` | `system:user:export` |
| PUT | `/api/system/users/{id}/status` | `system:user:edit` |
| PUT | `/api/system/users/{id}/password` | `system:user:reset` |
| GET/POST | `/api/system/roles` | `system:role:list` / `system:role:add` |
| PUT/DELETE | `/api/system/roles/{id}` | `system:role:edit` / `system:role:remove` |
| GET | `/api/system/roles/{id}/menus` | `system:role:list` |
| GET | `/api/system/roles/{id}/depts` | `system:role:list`，自定义数据范围的部门 |
| GET | `/api/system/roles/export` | `system:role:export` |
| GET | `/api/system/permissions/graph` | `system:role:list` |
| GET/POST | `/api/system/menus/tree`、`/api/system/menus` | `system:menu:list` / `system:menu:add` |
| PUT/DELETE | `/api/system/menus/{id}` | `system:menu:edit` / `system:menu:remove` |
| GET | `/api/system/menus/export` | `system:menu:export` |
| GET/POST | `/api/system/depts/tree`、`/api/system/depts` | `system:dept:list` / `system:dept:add` |
| PUT/DELETE | `/api/system/depts/{id}` | `system:dept:edit` / `system:dept:remove` |
| GET | `/api/system/depts/export` | `system:dept:export` |
| GET/POST | `/api/system/posts` | `system:post:list` / `system:post:add` |
| PUT/DELETE | `/api/system/posts/{id}` | `system:post:edit` / `system:post:remove` |
| GET | `/api/system/posts/export` | `system:post:export` |
| GET/POST | `/api/system/dict/types` | `system:dict:list` / `system:dict:add` |
| PUT/DELETE | `/api/system/dict/types/{id}` | `system:dict:edit` / `system:dict:remove` |
| GET | `/api/system/dict/types/export` | `system:dict:export` |
| GET | `/api/system/dict/data/{dictType}` | `system:dict:list` |
| GET | `/api/system/dict/options/{dictType}` | 登录 |
| POST/PUT/DELETE | `/api/system/dict/data[/{id}]` | `system:dict:add/edit/remove` |
| GET/POST | `/api/system/configs` | `system:config:list` / `system:config:add` |
| PUT/DELETE | `/api/system/configs/{id}` | `system:config:edit` / `system:config:remove` |
| GET | `/api/system/configs/export` | `system:config:export` |
| GET/POST | `/api/system/tenants` | `system:tenant:list` / `system:tenant:add`，仅平台管理员 |
| PUT | `/api/system/tenants/{id}` | `system:tenant:edit`，仅平台管理员；停用用 `{status:0}`，默认租户不可停用 |

用户写入字段：`username,nickname,password(创建时),phone,email,deptId,status,roleIds,postIds`；平台管理员创建时可用 `tenantId` 指定归属，租户管理员固定落在本租户。用户列表项带 `tenantId/tenantCode/tenantName/platformAdmin` 与 `lockedUntil`；`lockedUntil` 为 UTC 的 `YYYY-MM-DD HH:MM:SS`，数据库会保留过期的截止时刻，只有大于当前 UTC 时间才表示账号仍处于锁定中，成功登录或管理员重置后清空。角色写入字段：`name,code,dataScope,sort,status,remark,menuIds,deptIds`。数据范围取 `ALL/DEPT_AND_CHILD/DEPT/SELF/CUSTOM`；取 `CUSTOM` 时必须给出非空 `deptIds`。

租户写入字段：创建 `{code,name,remark?,adminUsername,adminNickname?,adminPassword}`，同时创建租户根部门、租户管理员角色与管理员账号；更新 `{name?,status?,remark?}`。租户编码为小写字母开头、仅含小写字母/数字/下划线，最长 32 位且全局唯一。

租户隔离范围：用户、节点、节点分组、遥测、告警、固件与任务、命令、操作/登录日志、运维总览、角色、部门、岗位、字典。菜单与参数配置为平台级共享。平台管理员跨租户读写；租户管理员只能访问本租户数据，创建角色时只能授予自己已有的权限，不能管理租户或平台级配置。

通知渠道：`GET/POST /api/system/notifications`、`PUT/DELETE /api/system/notifications/{id}`，权限码 `system:notification:list/add/edit/remove`。写入 `{name,type('webhook'|'email'),endpoint,recipients?,username?,sender?,secret?,minLevel,enabled}`；Webhook 地址为 http(s)，邮件为 `smtp://`、`smtps://` 或 `starttls://`；私有 CA 用 `caFile` 指定 PEM。`secret` 只写不回显，响应仅给 `secretConfigured`。投递记录：`GET /api/system/notifications/deliveries?status=&channelId=`，返回渠道、告警、目标、尝试次数与失败原因。

Webhook 地址在创建与修改时做 SSRF 校验：解析到私网、回环、CGNAT、组播、保留或文档示例段默认拒绝；云元数据主机名、已知元数据端点与 IPv4/IPv6 链路本地段始终拒绝。`allow_private_webhooks=true` 会跳过整个私网/保留段分类，普通私网、回环、CGNAT、组播、保留与文档示例段（含 IPv6 唯一本地地址）都会放行，云元数据与链路本地地址仍拒绝；它只适合受信的自托管网关，不是只放行 RFC1918 的窄开关。命中校验返回 400，错误文案说明命中的地址或主机。

命令批次取消：`PUT /api/commands/batch/{batchId}/cancel`，取消批次内 pending/running 命令并返回被取消列表。

命令批量与计划：`POST /api/commands/batch`，正文 `{nodeIds[1-100],type,payload,timeoutSeconds,scheduledAt?}`，返回 `{batchId,commands}`；`scheduledAt` 为 `YYYY-MM-DD HH:MM:SS`（UTC），到点前 Agent 轮询取不到。

固件灰度与回滚：`POST /api/firmware/{id}/deploy` 的正文可带 `batchSize`，返回 `{deployed,deferredNodeIds}`；不带 `batchSize` 时仍返回旧的数组形状。`POST /api/firmware/{id}/rollback` 正文 `{nodeIds}`，按任务记录的上一版本重新下发。

## 2.1 边缘反向代理

配置 `edgepulse.reverse_proxy.enabled=true` 后，`prefix`（默认 `/edge`）与 `prefix/*` 转发到 `upstream`。默认剥离前缀，例如 `GET /edge/api/status?a=1` 转发为 `GET <upstream>/api/status?a=1`。

- 默认要求 EdgePulse 会话；`require_session: false` 可公开，`permission` 可再加权限码。
- 只转发白名单请求头，不透传 Authorization/Cookie；响应状态、白名单响应头与正文原样返回。
- `maximum_request_bytes` / `maximum_response_bytes` / `timeout_ms` 限制单次转发；超过请求上限返回 413，上游失败返回 502。
- 不代理 WebSocket 升级与流式大响应。

## 3. 监控与工作台

| 方法 | 路径 | 权限/说明 |
|---|---|---|
| GET | `/api/monitor/oper-logs` | `monitor:operlog:list`；`keyword,result` |
| GET | `/api/monitor/oper-logs/stats` | `monitor:operlog:list`；`range=24h/7d/30d` |
| GET | `/api/monitor/oper-logs/timeline` | `monitor:operlog:list`；`date=YYYY-MM-DD` |
| GET | `/api/monitor/oper-logs/export` | `monitor:operlog:export` |
| DELETE | `/api/monitor/oper-logs/{id}`、`/api/monitor/oper-logs` | `monitor:operlog:remove` |
| GET | `/api/monitor/login-logs` | `monitor:loginlog:list`；`keyword,status` |
| GET | `/api/monitor/login-logs/stats` | `monitor:loginlog:list` |
| GET | `/api/monitor/login-logs/heatmap` | `monitor:loginlog:list`；`days=7/30` |
| GET | `/api/monitor/login-logs/export` | `monitor:loginlog:export` |
| DELETE | `/api/monitor/login-logs/{id}`、`/api/monitor/login-logs` | `monitor:loginlog:remove` |
| GET | `/api/dashboard/summary` | 登录；`range=24h/7d/30d` |

操作统计返回 `total,failed,successRate,avgCost,p95Cost,activeUsers,series`；登录统计返回 `total,failed,successRate,users,ips,series`；登录热力返回 `cells`（星期×小时）与 `anomalies`（失败聚合）。

## 4. 节点与分组

| 方法 | 路径 | 权限/说明 |
|---|---|---|
| GET/POST | `/api/nodes` | `device:node:list` / `device:node:write` |
| GET | `/api/nodes/{id}` | `device:node:list`；节点字段另带 `desiredConfigVersion`、`reportedConfigVersion`、`configInSync` |
| PUT/DELETE | `/api/nodes/{id}` | `device:node:write`；更新需 `version`，冲突 409 |
| POST | `/api/nodes/{id}/certificate/revoke` | `device:node:write`；清空指纹、置离线、销案系统离线告警，并以 1008 关闭该节点的 Agent WebSocket |
| GET/POST | `/api/node-groups` | `device:node:list` / `device:node:write` |
| PUT/DELETE | `/api/node-groups/{id}` | `device:node:write`；仍有子分组或节点时 409 |
| GET | `/api/ws` | WebSocket；升级后首条消息认证 |

节点字段：`id,name,status,agentVersion,certFingerprint,certNotAfter,certRevokedAt,labels,lastSeen,groupId,groupName,version`。`status` 为 `online` 或 `offline`；证书字段可空，吊销后指纹为空且 `certRevokedAt` 有值。

节点生命周期：超过 `edgepulse.agent.heartbeat_timeout_seconds` 未心跳的在线节点会被后台扫描置为 `offline`，并生成 `metric=node.online`、级别 `critical` 的系统告警；恢复心跳后节点回到 `online`，告警自动置为 `resolved`。手工建节点在首次心跳前不参与离线扫描。

### 4.1 注册令牌

| 方法 | 路径 | 权限/说明 |
|---|---|---|
| GET | `/api/enrollment-tokens?status=` | `device:node:write`；返回数组（不分页），`status` 可取 `active/used/expired/revoked` |
| POST | `/api/enrollment-tokens` | `device:node:write`；`{action:'enroll'|'rebind',nodeName?,expiresInSeconds?,tenantId?}` |
| DELETE | `/api/enrollment-tokens/{id}` | `device:node:write`；吊销未使用的令牌；不存在、已使用或已吊销返回 404 |

签发响应的 `data` 为令牌记录并额外带明文 `token`（以 `ep_enroll_` 开头）；明文只出现这一次，列表行只有 `id,tenantId,nodeName,action,expiresAt,usedAt,revokedAt,createdBy,createdAt,status`，不含 `token` 或 `tokenHash`。`expiresInSeconds` 默认 3600，允许 300 到 2592000。平台管理员可用 `tenantId` 指定归属租户；租户管理员只能为本租户签发，显式指定别家租户返回 403。

### 4.2 节点配置（设备影子）

| 方法 | 路径 | 权限/说明 |
|---|---|---|
| GET | `/api/nodes/{id}/config` | `device:node:list`；`{desired{version,content,hash,updatedAt,updatedBy},reported{version,content,hash,status,message,at},inSync}` |
| PUT | `/api/nodes/{id}/config` | `device:node:write`；`{content:对象或JSON字符串,expectedVersion,note?}`；版本冲突 409 |
| GET | `/api/nodes/{id}/config/revisions` | `device:node:list`；分页，按版本倒序保留不可变修订 |
| POST | `/api/nodes/{id}/config/rollback` | `device:node:write`；`{revisionId,note?}`，把历史修订作为新版本重新下发 |

`expectedVersion` 必须与实际 `desired.version` 一致，首次下发用 0；成功后版本加一。服务端把 `content` 规范化为紧凑 JSON 并重算 SHA-256，`inSync` 只有在期望/上报版本、摘要与 `applied` 状态同时相符时为 true。下发与回滚都会创建一条 `config.apply` 命令，在线 Agent 走 WebSocket 直推，离线 Agent 轮询取回。

## 5. 遥测、告警、固件、命令

| 方法 | 路径 | 权限/说明 |
|---|---|---|
| GET | `/api/telemetry/latest?nodeId=` | `device:telemetry:view` |
| GET | `/api/telemetry/series?nodeId=&metric=&from=&to=` | `device:telemetry:view`；最多 2000 点 |
| GET | `/api/alarms` | `device:alarm:list`；`status,level,nodeId`；返回 `openCount` |
| GET | `/api/alarms/{id}` | `device:alarm:list` |
| PUT | `/api/alarms/{id}/ack` | `device:alarm:handle` |
| PUT | `/api/alarms/{id}/resolve` | `device:alarm:handle` |
| GET/POST | `/api/alarm-rules` | `device:alarm:list` / `device:alarm:add` |
| PUT/DELETE | `/api/alarm-rules/{id}` | `device:alarm:edit` / `device:alarm:remove` |
| GET | `/api/events/alarms` | 登录；SSE，事件名 `alarm` 与 `ready` |
| GET | `/api/firmware` | `device:firmware:list` |
| GET | `/api/firmware/policy` | `device:firmware:list`；`{requireSignature,trustedKeyIds}` |
| POST | `/api/firmware?name=&version=&fileName=` | `device:firmware:upload`；流式二进制正文；可带固件签名头 |
| GET | `/api/firmware/{id}/tasks` | `device:firmware:list` |
| POST | `/api/firmware/{id}/deploy` | `device:firmware:upload`；`{nodeIds:[...],batchSize?}` |
| POST | `/api/firmware/{id}/rollback` | `device:firmware:upload`；`{nodeIds:[...]}` |
| DELETE | `/api/firmware/{id}` | `device:firmware:remove` |
| GET | `/api/commands` | `device:command:list`；`status,nodeId` |
| POST | `/api/commands` | `device:command:send`；`{nodeId,type,payload,timeoutSeconds}` |
| PUT | `/api/commands/{id}/cancel` | `device:command:send` |
| GET | `/api/ops/summary` | `monitor:operlog:list`；节点/告警/遥测/命令读数 |
| GET | `/api/ops/protocols` | `monitor:operlog:list`；监听与协议开关 |

上传响应带 `signed`、`signatureKeyId`、`signatureAlgorithm`；未签名固件三者分别为 `false`、空串、空串。

固件签名请求头：

| 头 | 说明 |
|---|---|
| `X-Firmware-Signature` | 对文件 SHA-256 的 32 字节原始摘要做 Ed25519 签名后的标准 Base64 |
| `X-Firmware-Key-Id` | 信任键目录里公钥文件的 stem，例如 `prod-2026` |

两个头必须成对出现。`requireSignature=true` 时缺任一头在收正文前返回 400；key id 未加载、摘要不符或签名格式错误在写完文件后返回 400 并删除成品。`requireSignature=true` 时未签名固件也不能部署或回滚。历史未签名固件仍可被 Agent 下载，但下载不带签名头。

告警规则字段：`name,nodeId(0=全部),metric,comparison,threshold,level,enabled,cooldownSeconds`。

## 6. Agent 通道（只挂 9443 mTLS）

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/agent/register` | `{name,agentVersion,certFingerprint,labels?,tenantCode?,enrollmentToken?}`；tenantCode 缺省 `default`；`certFingerprint` 必须与这条 mTLS 连接出示的叶证书一致 |
| POST | `/api/agent/heartbeat` | `{nodeId,agentVersion}`；响应是节点记录并加 `desiredConfigVersion` 与 `heartbeatIntervalSeconds` |
| POST | `/api/agent/telemetry` | `{nodeId,samples:[{metric,value,unit?,collectedAt?,labels?}]}`，1–500 条 |
| GET | `/api/agent/commands/next?nodeId=` | 取最早待执行命令并置 running；无命令时 data 为 null |
| POST | `/api/agent/commands/{id}/result` | `{nodeId,status,result,message}` |
| GET | `/api/agent/nodes/{id}/config` | `{version,content,hash}`；从未下发时为 `{version:0,content:{},hash:""}` |
| POST | `/api/agent/nodes/{id}/config/report` | `{version,content?,status:'applied'|'failed'|'pending',message?}`；返回完整配置视图与 `inSync` |
| GET | `/api/agent/firmware/{id}/download?nodeId=` | 必须带 `nodeId`；支持 `ETag`/`If-None-Match` 与单段 `Range`；签名固件回传三个 `X-Firmware-*` 头 |
| POST | `/api/agent/firmware/{id}/status` | `{nodeId,status,progress,message}` |
| GET | `/api/agent/ws` | WebSocket；首条 `{"type":"hello","nodeId":...}`；接收 `command.result` |

注册模式由 `edgepulse.agent.enrollment_mode` 决定：`required` 时新节点必须带 `enroll` 令牌、已有节点换证书必须带 `rebind` 令牌，同指纹重注册不需要令牌；`off` 只校验 mTLS CA 链与可选指纹白名单。令牌条件还校验租户、动作与 `nodeName`，缺令牌或令牌不可用统一返回 403，不区分过期、已用或名字不符。

除 `/api/agent/register` 这条注册入口外，所有 `/api/agent/*` 请求都要求连接叶证书指纹等于节点登记的 `certFingerprint`；注册接口本身没有可比的登记指纹，改为校验正文 `certFingerprint` 与本次连接出示的叶证书一致。指纹不符或证书已吊销返回 403，节点不存在返回 404。固件下载还要求固件与节点同租户。注册响应带 `certNotAfter`，心跳响应带节点状态；节点超过 `heartbeat_timeout_seconds` 未心跳会被判离线并产生 `node.online` 系统告警。

Agent 请求没有会话：命令、固件任务、遥测都按目标节点/固件的租户归属落库，跨租户标识会被拒绝。

### UDP 遥测数据报

默认关闭；启用后向 `edgepulse.udp_telemetry.port` 发送一条 JSON 数据报：

```json
{"nodeId":123,"timestamp":1760000000,"nonce":"8-64 字符","body":{"samples":[{"metric":"cpu.usage","value":42.5,"unit":"%"}]},"signature":"64 位十六进制"}
```

签名覆盖 `v1
<nodeId>
<timestamp>
<nonce>
<body.dump()>`，密钥为环境变量 `EDGEPULSE_UDP_TELEMETRY_SECRET`，算法 HMAC-SHA256。时间戳与服务器相差超过 120 秒、同一节点同一随机串重复、或样本不合法（1–500 条）都会被拒。应答是 `{"accepted":N,"alarms":M}` 或 `{"error":"..."}`。

## 7. WebSocket、SSE 与运维端点

浏览器 `/api/ws`：

- 首条消息 `{"type":"auth","token":"..."}`；5 秒内未认证关闭 1008。
- 成功回 `{"type":"auth","ok":true}`，并订阅 `nodes`、`alarms`、`commands`。
- 推送：`{"type":"node.status","data":...}`、`{"type":"alarm","data":...}`、`{"type":"command.status","data":...}`。
- 客户端每 25 秒发 `{"type":"ping"}`；服务端不要求 pong。

Agent `/api/agent/ws`：首条消息 `{"type":"hello","nodeId":N,"agentVersion":"..."}`；连接证书的叶证书指纹必须与节点登记的 `certFingerprint` 一致，否则按 1008 关闭。

SSE `/api/events/alarms`：连接后先收到 `event: ready`；告警变更以 `event: alarm` 发送，每 15 秒注释保活。浏览器用 fetch 流式读取并在请求头带 Bearer。

运维口（9100）：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/metrics` | Prometheus 文本；需 Bearer |
| GET | `/healthz` | 进程存活；匿名 |
| GET | `/readyz` | 就绪；停机排空期间 503 |
| GET | `/debug/loops`、`/debug/connections` | 循环与连接诊断；需 Bearer |
| POST | `/debug/log-level` | 改日志等级；需 Bearer |

## 8. 错误码

| HTTP | 场景 |
|---|---|
| 400 | 参数缺失、JSON 不合法、配置正文不合法；Webhook 地址被 SSRF 校验拒绝；缺固件签名头、验签失败；固件下载缺 `nodeId` |
| 401 | 未登录、会话过期、Redis/数据库不可用导致的认证失败 |
| 403 | 已登录但缺权限；Agent 连接证书与节点登记指纹不一致或证书已吊销；`required` 模式缺注册/改绑令牌；命令或固件不属于该节点/租户；租户管理员跨租户签发令牌 |
| 404 | 资源不存在；注册令牌不存在、已使用或已吊销；配置修订或节点不存在；固件文件缺失；`/api/*` 未命中 |
| 409 | 用户名/角色编码/岗位编码/字典类型/配置键冲突；节点或配置 `expectedVersion` 乐观锁冲突；并发注册冲突；仍被占用的删除 |
| 413 | 固件超过上传上限 |
| 416 | Range 越界 |
| 423 | 账号因窗口内连续登录失败被锁定；提示中的分钟数为配置的锁定时长 |
| 429 | 验证码签发过频、登录失败过频、公开 API 全局限流 |
| 500 | 数据库/文件/未知内部故障 |
| 503 | 服务未就绪或停机排空 |

## 9. OpenAPI 契约

`GET /api/openapi.json` 返回当前控制面的 OpenAPI 3.1 文档，响应头为 `content-type: application/json` 与 `cache-control: no-cache`。该接口匿名可读；生成物缺失、为空或不是以 `{` 开头的 JSON 对象时返回统一包络的 JSON 404，并带 `x-openapi-error: missing`，不会返回空 200。

生产部署必须先执行 `scripts/build-web.sh`（或等价的前端构建），它会把 `frontend/public/openapi.json` 随前端产物复制进后端的 `web/`；只部署后端二进制时该接口会按缺失返回 404。

契约的事实来源是 `src/**/*.cpp` 里的 `router.get/post/put/del/patch` 与流式变体字面量，摘要、请求/响应 schema、状态码与安全要求由 `docs/openapi-notes.json` 补齐；`frontend/public/openapi.json` 是生成物，不要手工编辑。

- 路径里的每个 `{name}` 都会生成一条 `in: path`、`required: true` 的参数；`id` 与 `batchId` 为 `integer/int64`，其余为 `string`。
- 响应默认是 `200 application/json`；导出端点在 notes 里标为 `200 text/csv`，OIDC 的 `/api/auth/oidc/start` 与 `/api/auth/oidc/callback` 标为 `302` 并声明 `Location` 响应头。
- 生成器只接受字面量路由路径；唯一例外是反向代理的 `configurationHandle->prefix` 动态前缀。同一 path+method 重复注册、notes 出现未知键或未知安全方案都会让生成失败。

```bash
# 重新生成（Python 3，仅标准库）
python scripts/generate-openapi.py

# 只校验，不写文件；与生成物逐字节不一致时退出 1
python scripts/generate-openapi.py --check
```

CI 的后端任务在构建前执行 `--check`，路由或 notes 改动后忘记重新生成会让任务失败。契约覆盖浏览器与自动化 API；Agent 通道、`/api/ws`、SSE（`/api/events/*`）、健康探针与反向代理通配前缀不在其中。
