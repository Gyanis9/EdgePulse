# EdgePulse 全量交付设计（P0–P3）

- 日期：2026-10-20
- 作者：Gyanis（实现协作：Qoder）
- 状态：已定稿。用户已预先授权自行裁决，不等待人工确认。

## 1. 目标与边界

在已验收的 P0 纵向切片之上，按原设计文档的 P1–P3 阶段把 EdgePulse 做成可部署、可管理、可观测的完整平台：

- P1：用户/角色/菜单/部门/岗位/字典/参数/日志/CSV 导出、权限矩阵、数据范围、401/403、操作审计；Redis 可选的会话、验证码与登录失败计数。
- P2：节点分组、Agent 遥测、告警规则与告警、固件上传/下载、命令下发、SSE 与 Agent WebSocket。
- P3：工作线程配置、就绪探针、Prometheus、OTLP、配置热重载、容器与 systemd 部署。

P4（边缘反代、UDP 遥测、OIDC、多租户）在原设计里明确为「另行设计」，本次不实现，也不留空壳。

## 2. 关键裁决

| 编号 | 裁决 | 理由 | 代价 |
|---|---|---|---|
| D1 | 引擎固定为 AsynGyanis 重构后的 1.0.0 线，子模块钉在提交 `5a4e9769` | 该版本线把源码重组为 Platform/Base/Core/Net/Database 五库，版本号已重置为 1.0.0；仓库尚未在该线上打新 tag，提交号是唯一可复现的真源 | 子模块状态显示为 v4.0.0 之后的提交；文档必须写明以提交号为准 |
| D2 | 本地 Conan 包版本为 `asyngyanis/1.0.0`，打开 `with_mysql=True` | 生产档需要 MySQL 驱动；Redis 由常驻的 hiredis 支持 | 打包含 libmysqlclient，首次编译较慢；本地运行仍默认 SQLite |
| D3 | 源码按 `Base / Core / App / Main / Plugins` 五块组织 | 用户明确要求；依赖方向固定为 Base ← Core ← Plugins ← App ← Main | 旧目录全部迁移，提交历史需按模块拆开 |
| D4 | `Core` 同时承载领域服务与 HTTP 交付原语（中间件、授权、WebSocket、SSE），`Plugins` 只放控制器与路由注册 | 避免 Core→Plugins 的反向依赖，保持依赖图无环 | Core 比纯领域层宽，需在目录注释里写清分层 |
| D5 | 单进程运行；`threads` 只创建额外事件循环，监听器统一绑定循环 0 | 多监听器分发要同时处理 HTTP/HTTPS/mTLS/QUIC 与 Agent 通道的循环归属，本次不冒这个风险 | `threads>1` 不会提升吞吐；多 worker 监听分发留待后续 |
| D6 | Redis 为可选后端：配置 `redis.enabled=true` 时会话、验证码、登录失败计数走 Redis；否则走进程内存储 | 本机无 Redis 服务，不能伪造集成测试；生产可用时切换同一接口 | 进程重启会丢失内存态；Redis 用例必须由环境变量门控 |
| D7 | 数据库默认 SQLite，生产可配置 MySQL；仓储层只用可移植 SQL，测试不伪造成 MySQL | 本机无 MySQL 服务；P0 已用 SQLite | MySQL 真机用例在提供 `EDGEPULSE_TEST_MYSQL_*` 时运行，否则 SKIP |
| D8 | 实时告警同时提供 WebSocket 与 SSE；SSE 用 fetch 流式读取，不把 token 放进 URL | 浏览器 EventSource 不能带 Authorization 头；长 token 不进 URL 是硬约束 | 前端需自写 SSE 解析器，不能直接用 EventSource |
| D9 | 固件文件落 `var/firmware/`，数据库只存元数据与 SHA-256；下载支持 Range/ETag | 100 MiB 固件不能整块读进内存；引擎提供内存映射响应 | 需要额外的文件生命周期清理 |
| D10 | 不实现多 worker 监听分发与 POSIX 监听句柄交接；换代采用优雅排空 + 滚动重启 | 四类监听器同时交接超出本次范围；优雅排空与就绪探针已能满足滚动发布 | 单机无法在不依赖负载均衡的情况下做到零停机；文档写清部署方式 |

## 3. 目录与依赖

```text
src/
├─ Base/                 # 技术基座：无业务、无 HTTP
│  ├─ Api/              # 统一响应包络、错误码、分页、CSV
│  ├─ Database/         # 连接池封装、迁移执行器
│  ├─ Security/         # PBKDF2、随机数、恒定时间比较
│  ├─ Runtime/          # 环境变量、ID、时间
│  └─ Json/             # ConfigValue 字段读取与序列化
├─ Core/                 # 领域核心与交付原语
│  ├─ Auth/             # 会话、验证码、认证服务、登录审计、权限与数据范围
│  ├─ System/           # 用户/角色/菜单/部门/岗位/字典/参数/日志仓储与服务
│  ├─ Device/           # 节点、节点分组、Agent 注册与心跳
│  ├─ Telemetry/        # 遥测写入与查询
│  ├─ Alarm/            # 规则、评估、去重、生命周期
│  ├─ Firmware/         # 固件元数据、文件存储、Range/ETag
│  ├─ Command/          # 命令队列、派发、结果
│  ├─ Ops/              # 概览、协议统计
│  ├─ Cache/            # KV 抽象：内存与 Redis 两种实现
│  ├─ Http/             # 认证/审计/安全头中间件、SSE、WebSocket、静态站点
│  └─ Plugin/           # IHttpPlugin 契约与装配上下文
├─ Plugins/              # 每个功能一个编译期插件：控制器 + 路由注册
│  ├─ AuthPlugin/
│  ├─ SystemPlugin/
│  ├─ MonitorPlugin/
│  ├─ DashboardPlugin/
│  ├─ DevicePlugin/
│  ├─ TelemetryPlugin/
│  ├─ AlarmPlugin/
│  ├─ FirmwarePlugin/
│  ├─ CommandPlugin/
│  └─ OpsPlugin/
├─ App/                  # 配置、生命周期、监听器装配、工作线程
└─ Main/                 # main()
```

依赖方向：`Base` ← `Core` ← `Plugins` ← `App` ← `Main`。`Core` 不包含具体插件；`App` 只知道 `IHttpPlugin` 列表与 `ApplicationConfiguration`。

## 4. 数据模型增量

在现有 V1–V3 之后新增迁移：

- V4：`sys_dept`、`sys_post`、`sys_user_post`、`sys_role_dept`、`sys_dict_type`、`sys_dict_data`、`sys_config`、`sys_oper_log`、`biz_node_group`、`biz_node.group_id/owner_dept_id`。
- V5：`biz_telemetry`、`biz_alarm_rule`、`biz_alarm`。
- V6：`biz_firmware`、`biz_firmware_task`、`biz_command`。

字段与索引：

- 软删除：`sys_user`、`sys_role`、`sys_dept`、`sys_post`、`sys_dict_type`、`sys_dict_data`、`sys_config`、`biz_node`、`biz_node_group`。
- 唯一：`sys_dept.name(parent_id)`、`sys_post.post_code`、`sys_dict_type.dict_type`、`sys_config.config_key`、`biz_node.name`、`biz_node_group.name(parent_id)`。
- 遥测：`(node_id, metric, collected_at)`；保留期配置 `telemetry.retention_days`。
- 告警：`dedup_key = rule_id + node_id`，同一条告警在恢复前不重复插入；`status` 取 `open/acknowledged/resolved`。
- 命令：`status` 取 `pending/running/succeeded/failed/timeout/canceled`。

## 5. API 契约

所有响应保持 `{code,message,data,traceId}`；分页保持 `{pageNum,pageSize,total,rows}`；`pageSize` 上限 100。CSV 导出为 UTF-8 BOM，文件名由前端指定。

### 5.1 认证与健康

沿用 P0：`/api/health`、`/api/auth/captcha|login|logout|me|routes|password`。

新增：

| 方法 | 路径 | 权限 | 说明 |
|---|---|---|---|

### 5.2 系统管理

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
| GET | `/api/system/roles/export` | `system:role:export` |
| GET | `/api/system/permissions/graph` | `system:role:list` |
| GET | `/api/system/menus/tree` | `system:menu:list` |
| POST/PUT/DELETE | `/api/system/menus[/{id}]` | `system:menu:add/edit/remove` |
| GET | `/api/system/menus/export` | `system:menu:export` |
| GET | `/api/system/depts/tree` | `system:dept:list` |
| POST/PUT/DELETE | `/api/system/depts[/{id}]` | `system:dept:add/edit/remove` |
| GET | `/api/system/depts/export` | `system:dept:export` |
| GET/POST | `/api/system/posts` | `system:post:list/add` |
| PUT/DELETE | `/api/system/posts/{id}` | `system:post:edit/remove` |
| GET | `/api/system/posts/export` | `system:post:export` |
| GET/POST | `/api/system/dict/types` | `system:dict:list/add` |
| PUT/DELETE | `/api/system/dict/types/{id}` | `system:dict:edit/remove` |
| GET | `/api/system/dict/types/export` | `system:dict:export` |
| GET | `/api/system/dict/data/{dictType}` | `system:dict:list` |
| GET | `/api/system/dict/options/{dictType}` | 登录 |
| POST/PUT/DELETE | `/api/system/dict/data[/{id}]` | `system:dict:add/edit/remove` |
| GET/POST | `/api/system/configs` | `system:config:list/add` |
| PUT/DELETE | `/api/system/configs/{id}` | `system:config:edit/remove` |
| GET | `/api/system/configs/export` | `system:config:export` |

`GET /api/system/users/options` 返回 `{depts,roles,posts,initPassword}`；`initPassword` 只回显部署方配置的初始口令提示，不返回真实凭据。

### 5.3 监控与工作台

| 方法 | 路径 | 权限 |
|---|---|---|
| GET | `/api/monitor/oper-logs`、`/stats`、`/timeline`、`/export` | `monitor:oper:list/export` |
| DELETE | `/api/monitor/oper-logs/{id}`、`/api/monitor/oper-logs` | `monitor:oper:remove` |
| GET | `/api/monitor/login-logs`、`/stats`、`/heatmap`、`/export` | `monitor:login:list/export` |
| DELETE | `/api/monitor/login-logs/{id}`、`/api/monitor/login-logs` | `monitor:login:remove` |
| GET | `/api/dashboard/summary` | 登录 |

日志统计在应用层聚合，避免 SQLite/MySQL 的日期函数分叉；查询窗口最大 30 天。

### 5.4 设备与 Agent

| 方法 | 路径 | 权限/通道 |
|---|---|---|
| GET/POST | `/api/nodes` | `device:node:list/add` |
| PUT/DELETE | `/api/nodes/{id}` | `device:node:edit/remove` |
| GET/POST | `/api/node-groups` | `device:group:list/add` |
| PUT/DELETE | `/api/node-groups/{id}` | `device:group:edit/remove` |
| POST | `/api/agent/register`、`/api/agent/heartbeat` | mTLS |
| POST | `/api/agent/telemetry` | mTLS |
| GET | `/api/agent/commands/next` | mTLS |
| POST | `/api/agent/commands/{id}/result` | mTLS |
| GET | `/api/agent/firmware/{id}/download` | mTLS |
| POST | `/api/agent/firmware/{id}/status` | mTLS |
| GET | `/api/agent/ws` | mTLS，WebSocket |
| GET | `/api/ws` | 浏览器，首条消息认证 |

### 5.5 遥测、告警、固件、命令

| 方法 | 路径 | 权限 |
|---|---|---|
| GET | `/api/telemetry/latest?nodeId=` | `device:telemetry:view` |
| GET | `/api/telemetry/series?nodeId=&metric=&from=&to=` | `device:telemetry:view` |
| GET | `/api/alarms`、`/api/alarms/{id}` | `device:alarm:list` |
| PUT | `/api/alarms/{id}/ack`、`/resolve` | `device:alarm:handle` |
| GET/POST | `/api/alarm-rules` | `device:alarm:list/add` |
| PUT/DELETE | `/api/alarm-rules/{id}` | `device:alarm:edit/remove` |
| GET | `/api/events/alarms` | 登录，SSE |
| GET/POST | `/api/firmware`（multipart 上传） | `device:firmware:list/upload` |
| DELETE | `/api/firmware/{id}` | `device:firmware:remove` |
| GET | `/api/commands` | `device:command:list` |
| POST | `/api/commands` | `device:command:send` |
| PUT | `/api/commands/{id}/cancel` | `device:command:send` |
| GET | `/api/ops/summary`、`/api/ops/protocols` | `monitor:operlog:list` |

## 6. 前端页面

在现有 scaffold 页面上补齐：

- 节点：分组筛选与维护、节点详情抽屉（遥测最新值、最近命令）。
- 遥测：节点 + 指标选择，时间序列折线；数据来自 `/api/telemetry/series`。
- 告警中心：列表、确认、恢复、规则维护；实时刷新走 WebSocket `alarms` 主题或 SSE。
- 固件管理：上传、列表、删除、下发到节点、任务进度。
- 命令中心：选择节点与命令类型，查看状态与结果。
- 运维总览：工作台补充节点在线率、告警数、最近活动、近 7 日操作趋势。
- 系统管理：现有用户/角色/菜单/部门/岗位/字典/参数页全部接通真实接口。
- 监控：操作日志与登录日志的统计、时间线、热力图接通真实接口。

所有页面沿用 scaffold 的 `ListPage`、`SearchForm`、`FormDialog`、`PermissionButton`、`StatusTag` 与现有图表组件；不新增 UI 或图表库。

## 7. 安全

- 密码 PBKDF2-HMAC-SHA256（210000 次、16 字节盐、32 字节输出），比较恒定时间。
- 浏览器 token 为 32 字节 CSPRNG 的不透明值，TTL 2 小时滑动续期；支持 Redis 多实例共享。
- 验证码一次性、2 分钟 TTL、按 IP 30 次/分钟；登录失败按 IP + 账号计数，10 次/分钟锁定。
- 所有非匿名 `/api` 请求先过认证中间件；控制器逐个校验权限码；无权限返回 403，无会话返回 401。
- 数据范围：`ALL/DEPT_AND_CHILD/DEPT/SELF/CUSTOM`，应用于用户、节点与日志列表。
- Agent 通道只开在 mTLS 监听器；证书白名单在握手层，注册时校对 Agent 自报指纹。
- 运维口只听回环；`/metrics` 与调试端点需 Bearer；`/healthz`、`/readyz` 匿名。
- 所有口令、token、私钥、MySQL/Redis 凭据只从环境变量或未跟踪文件读取。
- 安全响应头由中间件统一添加；TLS 上追加 HSTS。

## 8. 实时与一致性

- 浏览器 WebSocket：升级后 5 秒内必须发 `{"type":"auth","token":...}`；认证成功订阅 `nodes`、`alarms`、`commands` 主题；心跳 25 秒。
- Agent WebSocket：mTLS 握手后首条消息 `{"type":"hello","nodeId":...}`；服务端在命令创建时推送 `command` 消息；Agent 回 `result`。
- Agent 轮询兜底：`/api/agent/commands/next` 原子地把最早的 `pending` 命令置为 `running`。
- 告警评估：遥测写入后同步评估规则；同一 `rule_id + node_id` 在恢复前只保留一条；恢复时置 `resolved` 并广播。
- SSE：每 15 秒发注释保活；前端用 fetch + ReadableStream 解析，断线指数退避重连。

## 9. 可观测与部署

- 引擎 `HttpServer::enableMetricsEndpoint` 输出 `asyn_http_*` 与进程读数；业务侧注册 `edgepulse_nodes_total`、`edgepulse_alarms_open`、`edgepulse_telemetry_samples_total`、`edgepulse_commands_total`。
- 配置 `metrics.export` 打开时按引擎 `MetricsExportConfiguration` 建 OTLP 出口。
- `/readyz` 检查数据库连接与（启用时）Redis；停机排空期间返回 503。
- 配置热重载：`edgepulse.hot_reload` 打开时，日志等级、告警阈值、验证码/会话 TTL 从 ConfigManager 重新读取；不可热改的监听地址写日志提示重启。
- 部署产物：`deploy/docker/Dockerfile`、`docker-compose.yml`（MySQL、Redis、EdgePulse、Prometheus）、`deploy/systemd/edgepulse.service`、`deploy/prometheus/prometheus.yml`。
- 工作线程：`edgepulse.threads` 只创建额外循环；监听器固定循环 0。
- 优雅停机：先让 `/readyz` 返回 503，再停止接受新连接，排空在途请求，最后关闭数据库与执行器。

## 10. 测试与验收

- 单元：KV、CSV、数据范围、告警规则、Range 解析、权限码、密码、会话、验证码。
- 集成：SQLite 迁移 V1–V6、系统管理 CRUD、节点/分组、遥测与告警、固件元数据、命令生命周期、审计落库。
- 协议：401/403 矩阵、系统接口、WS 认证与广播、SSE、mTLS 拒绝/通过、h3 真实连接、Range 206、ETag 304。
- 前端：新增 API 模块单测、SSE 解析、Playwright 登录/节点/系统用户/告警。
- 验收脚本：登录、系统页、节点、遥测、告警 2 秒内到达、100 MiB 固件哈希一致、Range/ETag、WS 1008、mTLS、h3、metrics、readyz。
- MySQL/Redis 真机验证取决于部署环境；本机没有常驻服务时只验证 SQLite + 内存档，不伪造。

## 11. 实施顺序与提交

1. 引擎 1.0.0 升级与源码五块重组，P0 回归全绿。
2. Base/Core 基础（KV、CSV、分页、权限、数据范围、审计、迁移 V4–V6）。
3. P1 后端与前端系统管理、监控、工作台。
4. P2 后端与前端设备、遥测、告警、固件、命令。
5. P3 可观测、热重载、部署产物与验收。
6. 文档、覆盖率与最终验收。

每个提交只讲一件事，按模块拆分；提交前构建与相关用例必须通过。

## 12. 已知限制

- 进程内会话在未启用 Redis 时重启即失效；监听器共享同一份内存状态。
- MySQL 真机与 Redis 真机验证取决于部署环境，未提供时只能跑 SQLite/内存档。
- SSE 使用轮询取队列，延迟上限 250ms；监听器固定在单循环。
- 边缘反向代理、UDP 遥测、OIDC、多租户不在本次范围。
- 固件下发是「Agent 主动下载 + 回报状态」，不做断点续传之外的 P2P 分发。
