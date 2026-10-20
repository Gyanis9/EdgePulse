# EdgePulse 架构设计与 P0 纵向切片规范

- 日期：2026-10-20
- 作者：Gyanis（实现协作：Qoder）
- 状态：已定稿，按用户「不要问我、自行判断」的预先授权直接进入实现

## 1. 需求理解与边界

EdgePulse 是前后端分离的边缘节点与设备运维平台，分两个平面：

- 控制面：Vue 3 管理后台，负责登录、权限、节点/设备、遥测、告警、固件、命令、审计、系统管理。
- 数据面：运行在边缘节点上的 `edgepulse-agent`，负责 mTLS 注册、心跳、遥测、命令执行、固件下载。

本项目从零搭建，后端是 C++23 + AsynGyanis 引擎，前端 fork 自 `scaffold-vue`，接口契约兼容 `scaffold-system` 的 `/api` 形态。引擎以 Conan 包消费，不修改、不复制、不 `add_subdirectory`。

本次交付只做 P0 纵向切片。P1–P4 的模块与表结构不在本次一次性生成，避免留下空模块和 TODO 桩。

### 1.1 本次必须交付

1. 引擎 submodule 固定在 `v4.0.0`，本地 Conan 包可用，工程可配置、可编译、可测。
2. 后端单进程可运行：配置、日志、事件循环、SQLite 迁移、8080 HTTP。
3. 认证闭环：验证码、登录、登出、`me`、`routes`、改密；不透明会话 token；PBKDF2 密码。
4. 一条业务纵向：节点列表 + 节点写入 + 节点状态 WebSocket + mTLS Agent 注册/心跳 + H3 探针 + `/metrics`。
5. 前端 fork 完成，登录页适配 `imageSvg`，节点页可打开，WS 实时刷新。
6. Playwright 登录与节点页通过；H3 探针打印真实 h3；mTLS 无证书被拒、正确证书通过。
7. 文档：README、架构、API、部署、验收、覆盖率。
8. 分批次提交，提交信息遵循仓库的约定式提交规范。

### 1.2 明确不做（本次）

- 不实现 P1 的系统管理域（用户/角色/菜单/部门/配置/日志/导出）的完整接口；只做认证所需的表与最小查询。
- 不实现遥测、告警、固件、命令、SSE、Agent WebSocket、零停机、OTLP、Prometheus 之外的扩展。
- 不做代码生成器；不搬 Java 后端；不做 OAuth/OIDC、边缘反向代理、UDP 遥测、多租户。
- 不维护 MySQL/PostgreSQL/SQLite 三套运行配置；MySQL 生产、SQLite 测试与本地快速档、PostgreSQL 可选 CI。
- 不把长期 token 放进 URL；不 mock 数据库集成测试；不编造性能数字。

## 2. 关键裁决（含与原始提示词冲突处）

用户已入睡并授权自行判断，以下裁决随本文件生效。每条都给出代价，便于醒后复核。

| # | 裁决 | 理由 | 代价 |
|---|---|---|---|
| R1 | 引擎路径用 `third_party/AsynGyanis` | 提示词目录树与验收命令都写 `third_party`，仅硬约束文字写 `3rd`；两者取多数与命令可执行者 | 与硬约束字面不完全一致，需在 README 标注 |
| R2 | 可执行体用 `edgepulse-server`、`edgepulse-agent` | 提示词验收命令用小写可执行名，CMake 目标名同样小写；产品名仍写 EdgePulse | 与「Edgepulse-Server」的描述性写法不同 |
| R3 | P0 本地档用 SQLite + 进程内会话/验证码 | 本机 Docker daemon 未运行、无 MySQL/Redis 服务；提示词允许 SQLite 作本地快速档，且 P1 才要求 Redis 会话 | P0 不能验证跨进程会话；P1 需迁移到 Redis |
| R4 | 构建预设 `debug` 使用 RelWithDebInfo + `/MD`；不建 Debug CRT 变体 | 引擎包是 Release 单档，MSVC 混用 `/MDd` 与 `/MD` 会 LNK2038；只为调试版再打一套引擎包违反「不为架构约束维护第二套构建」 | 没有调试版 CRT 断言与 `_ITERATOR_DEBUG_LEVEL=2`；需要这些时另起完整包，另行决策 |
| R5 | h3 用「只挂在 QUIC 路由器上的探针路径」判定 | 引擎 `HttpClient` 只能「先试 h3、失败回落 TCP」，没有强制开关；路径只存在于 h3 路由器时，200 即证明走了 h3 | 需要在 h3 路由器上重复注册公开路由 |
| R6 | mTLS 指纹身份采用「证书白名单 + Agent 自报指纹」 | 引擎在握手期校验证书与白名单，但 HTTP 处理器读不到对端证书（引擎缺口，已记录）；应用层把 Agent 上报的指纹与白名单比对并落库 | 同一白名单内证书的持有者可自报成另一个已登记指纹；真正的逐请求身份绑定要等引擎开放接口，或改为每节点独立 CA（P2 再评估） |
| R7 | P0 会话与验证码放进程内；P1 换 Redis | 与提示词阶段划分一致（P1 明确「Redis 会话、验证码、登录失败计数」） | P0 单进程有效，重启即失效；不假装多 worker 可用 |
| R8 | 前端保留 `scaffold.*` 存储键 | 提示词要求保留 `http.ts` 的 token/401 契约；改键会让既有测试与页面全部失效 | 品牌与内部键名不一致，需在代码注释说明这是兼容契约 |
| R9 | 二进制名、npm 包名、CMake 项目名分别取 `edgepulse-server`、`edgepulse-web`、`EdgePulse` | npm 名必须全小写；CMake/C++ 命名空间用 PascalCase | 三处大小写不同，文档统一列明 |
| R10 | 删除 scaffold 的代码生成页与其入口，不保留置灰死页 | 提示词明确「Generator 页面先移除或置灰」；入口不存在时保留文件就是死代码 | 若 P4 重启生成器，需要从上游 scaffold 重新取回 |

## 3. 完整目录树（目标形态）

```text
EdgePulse/
├─ CMakeLists.txt
├─ CMakePresets.json
├─ conanfile.py
├─ .clang-format
├─ .clang-tidy
├─ .gitignore
├─ .gitattributes
├─ .gitmodules
├─ README.md
├─ CONTRIBUTING.md
├─ cmake/
│  └─ EdgePulseWarnings.cmake
├─ config/
│  ├─ dev.yaml
│  ├─ prod.yaml.example
│  └─ logging.yaml
├─ migrations/
│  ├─ V1__schema.sql
│  ├─ V2__seed.sql
│  └─ V3__device_domain.sql
├─ third_party/
│  └─ AsynGyanis/              # git submodule，只读，固定 v4.0.0
├─ src/
│  ├─ CMakeLists.txt
│  ├─ App/
│  │  ├─ Main.cpp
│  │  ├─ Application.h/.cpp
│  │  ├─ ApplicationConfiguration.h/.cpp
│  │  ├─ ListenerAssembly.h/.cpp
│  │  └─ WorkerAssembly.h/.cpp
│  ├─ Common/
│  │  ├─ ApiResponse.h/.cpp
│  │  ├─ Pagination.h/.cpp
│  │  ├─ ErrorCatalog.h/.cpp
│  │  ├─ RequestContext.h/.cpp
│  │  ├─ PasswordHash.h/.cpp
│  │  ├─ RandomToken.h/.cpp
│  │  ├─ TimeFormat.h/.cpp
│  │  ├─ Database.h/.cpp
│  │  └─ JsonFields.h/.cpp
│  ├─ Web/
│  │  ├─ RouterSetup.h/.cpp
│  │  ├─ MiddlewareSetup.h/.cpp
│  │  ├─ StaticSite.h/.cpp
│  │  ├─ WebSocketEndpoints.h/.cpp
│  │  ├─ RequestId.h/.cpp
│  │  └─ ProtocolStats.h/.cpp
│  ├─ Auth/
│  │  ├─ AuthController.h/.cpp
│  │  ├─ AuthService.h/.cpp
│  │  ├─ CaptchaService.h/.cpp
│  │  ├─ SessionStore.h/.cpp
│  │  ├─ PermissionService.h/.cpp
│  │  └─ LoginAudit.h/.cpp
│  ├─ System/
│  │  ├─ UserRepository.h/.cpp
│  │  ├─ MenuRepository.h/.cpp
│  │  └─ MigrationRunner.h/.cpp
│  ├─ Device/
│  │  ├─ NodeService.h/.cpp
│  │  ├─ NodeRepository.h/.cpp
│  │  ├─ AgentRegistry.h/.cpp
│  │  └─ AgentController.h/.cpp
│  ├─ Telemetry/               # P2
│  ├─ Alarm/                   # P2
│  ├─ Firmware/                # P2
│  └─ Ops/
│     ├─ OpsController.h/.cpp
│     ├─ MetricsSummary.h/.cpp
│     └─ ReadinessProbe.h/.cpp
├─ samples/
│  ├─ DeviceAgent/             # edgepulse-agent 探针
│  ├─ H3Probe/
│  └─ LoadProbe/               # P3
├─ tests/
│  ├─ Unit/
│  ├─ Integration/
│  ├─ Protocol/
│  └─ TestSupport/
├─ frontend/                   # scaffold-vue 的 fork
├─ deploy/
│  ├─ docker/
│  ├─ systemd/
│  └─ prometheus/
├─ scripts/
│  ├─ dev-certs.sh
│  ├─ gen-agent-certs.sh
│  ├─ run-dev.sh
│  └─ check-acceptance.py
├─ docs/
│  ├─ 架构.md
│  ├─ API.md
│  ├─ 部署.md
│  ├─ 验收.md
│  ├─ 覆盖率.md
│  └─ superpowers/
│     ├─ specs/2026-10-20-edgepulse-design.md
│     └─ plans/2026-10-20-edgepulse-p0.md
├─ assets/
└─ packaging/
```

P0 实际落地的目录是上树的子集；`Telemetry/`、`Alarm/`、`Firmware/`、`LoadProbe` 不建空目录、不写空文件，P2 再创建。

## 4. 模块划分与依赖方向

```text
App
 ├─ Web（HTTP 原语、静态站点、WS hub、协议统计；不访问数据库）
 ├─ Auth（验证码、登录、会话、权限校验、登录审计）──> System
 ├─ System（用户、菜单、迁移执行器）
 ├─ Device（节点、Agent 注册表）──> Web（WS hub 发布）
 ├─ Ops（运维摘要、协议统计读口）
 └─ Common（响应、分页、错误码、密码、随机数、时间、数据库封装）
```

约束：

- 所有业务模块只依赖 `Common` 与引擎；`System` 不依赖 `Web`/`Auth`。
- 同层内只允许 `Auth -> System` 与 `Device -> Web` 两条边（前者读用户/菜单，后者发 WS 状态）；其余同层互不依赖。`Web` 不反向依赖任何业务模块。
- 路由注册由各模块暴露 `registerRoutes(Router&, Services&)`，由 `App::ListenerAssembly` 调用；`Web` 只提供通用中间件与静态站点。
- `Auth`、`Device`、`Ops` 的控制器不直接拼 JSON 字符串；统一走 `Common::ApiResponse`。
- 所有跨线程阻塞数据库调用经引擎 `AsyncExecutor::shared()->submit(loop, work)` 回到事件循环；事务对象不跨协程共享。

## 5. 数据模型

### 5.1 P0 表

| 表 | 关键列 | 索引/约束 |
|---|---|---|
| `sys_user` | id, username, password, nickname, phone, email, dept_id, status, last_login_time, deleted, created_at, updated_at, created_by, updated_by | PK id；UNIQUE(username) |
| `sys_role` | id, role_name, role_code, data_scope, builtin, status, deleted, 审计四列 | PK id；UNIQUE(role_code) |
| `sys_menu` | id, parent_id, menu_name, menu_type, icon, path, component, permission, sort, visible, status, 审计四列 | PK id；INDEX(parent_id) |
| `sys_user_role` | user_id, role_id | PK(user_id, role_id) |
| `sys_role_menu` | role_id, menu_id | PK(role_id, menu_id) |
| `sys_login_log` | id, username, ip, status, message, trace_id, login_time | INDEX(login_time) |
| `biz_node` | id, name, status, agent_version, cert_fingerprint, labels JSON, last_seen, version, deleted, 审计四列 | PK id；UNIQUE(name)；INDEX(status)；INDEX(last_seen) |
| `schema_migrations` | version, description, checksum, applied_at | PK(version) |

- 时间列统一 `DATETIME`，由引擎 `Timestamp`/`LocalDateTime` 映射；SQLite 同名列以 TEXT 存储，读取时由引擎转换。
- 软删除只用于 `sys_user`、`sys_role`、`biz_node`；所有查询带 `deleted = 0`。
- ID 由应用生成（64 位时间有序：毫秒左移 20 位 + 进程内序列），避免 MySQL/SQLite 自增语法分叉；种子行使用固定小整数。
- JSON 列在两种方言里都声明为 `JSON`，SQLite 按 TEXT 亲和存储。
- `biz_node.version` 用于乐观锁：更新 `WHERE id=? AND version=?`，影响行数为 0 时返回 409。

### 5.2 迁移

- `migrations/V*.sql` 按版本号升序执行；`schema_migrations` 记录版本、说明与 SHA-256 校验和。
- 单条语句以行尾分号切分，跳过 `--` 注释；不接受存储过程与 `DELIMITER`。
- 迁移在启动时由 `System::MigrationRunner` 在单连接上执行；已应用的版本校验和不一致时启动失败。
- MySQL 的 DDL 隐式提交无法包在一个事务里；失败时按版本表记录定位，不假装原子。

## 6. API 契约

统一响应：

```json
{ "code": 200, "message": "ok", "data": {}, "traceId": "..." }
```

- 失败时 HTTP 状态为 4xx/5xx，`code` 为业务码；`data` 为 `null`。
- 分页：`{ "pageNum": 1, "pageSize": 10, "total": 0, "rows": [] }`，`pageSize` 上限 100。
- `traceId` 取引擎请求 ID，并回写 `x-request-id`。
- 成功 `message` 为 `ok`（与 scaffold Java 的「操作成功」不同，前端只读 `data`，不依赖文案）。

### 6.1 P0 已实现

| 方法 | 路径 | 权限 | 说明 |
|---|---|---|---|
| GET | `/api/health` | 匿名 | `{status:"UP",time}` |
| GET | `/api/auth/captcha` | 匿名 | `{uuid, imageSvg, code?}`；dev 档带 `code` |
| POST | `/api/auth/login` | 匿名 | `{username,password,captcha,captchaUuid}` → `{token,expiresIn}` |
| POST | `/api/auth/logout` | 登录 | 删除会话 |
| GET | `/api/auth/me` | 登录 | `{id,userId,username,nickname,deptId,deptName,roles,permissions}` |
| GET | `/api/auth/routes` | 登录 | `{routes:[{id,parentId,name,path,component,icon,permission,children}]}` |
| PUT | `/api/auth/password` | 登录 | `{oldPassword,newPassword}` |
| GET | `/api/nodes` | `device:node:list` | 分页 `pageNum,pageSize,keyword,status` |
| POST | `/api/nodes` | `device:node:write` | `{name,labels?}` |
| PUT | `/api/nodes/{id}` | `device:node:write` | `{name?,labels?,version}`；冲突 409 |
| DELETE | `/api/nodes/{id}` | `device:node:write` | 软删除 |
| GET | `/api/probe/h3` | 匿名，仅 h3 路由器 | 固定 200 `{"proto":"h3"}`；TCP 路由器返回 404 |
| POST | `/api/agent/register` | mTLS 监听器 | `{name,agentVersion,certFingerprint,labels?}` → 节点 |
| POST | `/api/agent/heartbeat` | mTLS 监听器 | `{nodeId,agentVersion}` → 节点；广播状态 |
| GET | `/api/ws` | 升级后首条消息认证 | 浏览器节点状态推送 |
| GET | `/healthz`、`/readyz` | 匿名，仅 9100 | 存活/就绪 |
| GET | `/metrics` | Bearer，仅 9100 | Prometheus 文本 |

WS 协议：

- 服务端升级后 5 秒内等首条消息 `{"type":"auth","token":"<token>"}`；超时或 token 无效关 1008。
- 成功回 `{"type":"auth","ok":true}`；随后推送 `{"type":"node.status","data":{...}}`。
- 心跳：服务端每 25 秒发 `{"type":"ping"}`，客户端回 `{"type":"pong"}`；连续两个周期无 pong 关闭。

### 6.2 推迟到后续阶段

`/api/system/*`、`/api/monitor/*`、`/api/dashboard/summary`、`/api/devices*`、`/api/telemetry/*`、`/api/alarms*`、`/api/firmware*`、`/api/commands*`、`/api/ops/summary`、`/api/ops/protocols`、`/api/events/logs`（SSE）、Agent WS、Agent 遥测/固件接口。

## 7. 网络与安全

### 7.1 监听器

| 端口 | 协议 | 用途 |
|---|---|---|
| 8080/TCP | HTTP/1.1 | 开发档 API + WS + 静态 SPA；Vite 代理目标 |
| 8443/TCP | HTTPS，ALPN h2/http1.1 | 控制台；挂 `altSvcMiddleware(8443)` |
| 8443/UDP | HTTP/3 | 控制台 h3；独立 QUIC 路由器，多一条 h3 专属探针 |
| 9443/TCP | HTTPS + mTLS | Agent 专用；要求客户端证书并启用指纹白名单 |
| 9100/TCP | HTTP/1.1，仅 127.0.0.1 | 运维面：`/metrics`、`/healthz`、`/readyz`、`/debug/loops` |

### 7.2 认证与授权

- 浏览器 token：32 字节 CSPRNG 的十六进制，不透明；进程内会话 TTL 2 小时，活跃请求滑动续期；登出立即删除。
- 密码：PBKDF2-HMAC-SHA256，16 字节盐，210000 次，32 字节输出，格式 `pbkdf2_sha256$210000$<salt_b64>$<hash_b64>`；比较用恒定时间。
- 验证码：4 位大写字母+数字，2 分钟 TTL，一次性消费，按 IP 限流（30 次/分钟）；dev 档返回 `code`，prod 只返回 `imageSvg`。
- 登录限流：按 IP 每分钟 10 次；失败计数在 P1 接 Redis。
- 每个写接口校验权限码；P0 只有管理员种子用户，权限列表返回全量。数据范围在 P1 接入 SQL。
- 安全响应头：`X-Content-Type-Options: nosniff`、`X-Frame-Options: DENY`、`Referrer-Policy: no-referrer`、CSP；HSTS 只在 TLS 上发。
- Agent 侧仅 mTLS 监听器开放；指纹白名单从配置读取，Agent 自报指纹必须与白名单一致才允许注册（见 R6）。
- 运维口只监听回环，`/metrics` 与调试端点需 Bearer；`/healthz`、`/readyz` 不挡。

### 7.3 秘密

- 所有口令、token、私钥只从环境变量或未跟踪文件读取；仓库零明文。
- dev 启动脚本生成 `var/ops-token` 与 `var/certs/*`，两处都在 `.gitignore` 中。
- `config/dev.yaml` 只放非敏感项；`prod.yaml.example` 只给键名与说明。

## 8. 引擎能力边界与应用层绕过

经源码核对，以下能力引擎不提供，必须在应用层解决，且不得修改引擎：

| 缺口 | 应用层做法 |
|---|---|
| 无 SPA `index.html` 回退 | `Web::StaticSite` 自行解析 `web/`；未命中的 GET 回 `index.html`，`/api/*` 仍 404 |
| 无每请求客户端证书指纹 | mTLS 白名单在握手层；Agent 注册自报 SHA-256 指纹并与白名单比对（R6） |
| HttpClient 不能强制 h3 | h3 专属探针路由；同一路径走 TCP 为 404，回落不会被误判成 h3（R5） |
| 无 SSE 自动心跳 | P2 用 `SseStream::sendComment()` 每 15 秒发一次 |
| 无 PBKDF2/会话存储 | `Common::PasswordHash` 直接用 OpenSSL `PKCS5_PBKDF2_HMAC`；`Auth::SessionStore` 自研 |
| 无每路由中间件 | 全局中间件按路径前缀分派，或处理器内调用权限校验 |

## 9. 分阶段实现计划（本次只做 P0）

| 阶段 | 内容 | 验收 |
|---|---|---|
| P0 | 本文第 1.1 节 | 后端构建/单测、前端构建、Playwright、h3 真实连接、mTLS 拒绝/通过 |
| P1 | 用户/角色/菜单/部门/配置/日志/CSV 导出、Redis 会话与验证码、数据范围、401/403 矩阵、审计中间件 | scaffold session-regression、低权限 403、导出不越权 |
| P2 | 设备、遥测、告警、固件、SSE、Agent WS | 100 MiB 固件哈希一致、Range 206、ETag 304、WS 1008、告警 2 秒内到达 |
| P3 | 多 worker、零停机、热重载、OTLP、Prometheus、容器与 systemd | 换代 0 个 5xx、readyz 503→恢复、指标族存在 |
| P4 | 边缘反代、UDP 遥测、OIDC、多租户 | 另行设计 |

## 10. P0 验收命令

```bash
# 引擎包（已固定在 v4.0.0）
git submodule update --init --recursive
conan create third_party/AsynGyanis/packaging/conan --build=missing \
  -s compiler.cppstd=23 -s build_type=Release -o "&:with_mysql=True"

# 后端
conan install . --build=missing -s compiler.cppstd=23 -s build_type=Release
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure

# 前端
cd frontend
npm ci
npm run test:unit
npm run build
npm run test:e2e

# 运行与协议验收
bash scripts/dev-certs.sh
bash scripts/run-dev.sh
python3 scripts/check-acceptance.py
```

`scripts/check-acceptance.py` 逐项执行并打印证据：

1. `GET /api/health` 200。
2. 获取验证码 → 登录 → `me` → `routes` → 节点列表。
3. `H3Probe` 请求 `/api/probe/h3`，断言 200 且 `proto=h3`；同时断言 TCP `/api/probe/h3` 为 404。
4. `AgentProbe` 用客户端证书注册并心跳；`curl --cacert` 无客户端证书访问 9443 失败；错误指纹证书失败；正确证书成功。
5. `/metrics` 带 Bearer 200，不带 Bearer 401。
6. WS 未认证 1008；认证后收到节点状态。

## 11. 测试策略

- 单元测试：密码格式与校验、验证码 TTL/一次性、会话滑动过期/登出、分页上限、JSON 响应包络、ID 生成、迁移切分器、权限校验。
- 集成测试：临时 SQLite 文件应用 V1–V3、种子登录、节点 CRUD、乐观锁冲突、登录审计落库。
- 协议测试：8080 上的认证/节点矩阵；WS 1008/认证/广播；mTLS 三种证书形态；h3 专属路径。
- 前端：现有 `node:test` 契约测试 + 新增 API 模块测试；Playwright 用系统 Edge（`channel: 'msedge'`），不下载浏览器。
- 真实 MySQL/Redis 用例一律用 `EDGEPULSE_TEST_MYSQL_*` / `EDGEPULSE_TEST_REDIS_*` 门控，缺凭据 `GTEST_SKIP`；P0 不伪造这些用例。

## 12. 编码与提交规范

- C++23；类/文件 PascalCase；函数/成员 camelCase；成员 `m_` 前缀；Doxygen 中文注释；注释不用 Markdown 排版记号、不引用计划编号。
- RAII；禁止裸 new/delete；常驻协程 `Task` 必须持有到收口；阻塞数据库调用走 `AsyncExecutor`。
- 错误：启动期配置错误直接失败并写清原因；请求期错误回统一 envelope；不把 `what()` 原样回给客户端。
- 提交：`<类型>(<范围>): <中文描述>`，首行 ≤ 72 列，正文写动机；按模块分批次提交；提交前本地构建与相关用例通过。
