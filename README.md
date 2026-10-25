# EdgePulse

> 边缘节点与设备运维平台：C++23 控制面 + Vue 3 管理后台 + mTLS Agent 数据面。

[![CI](https://github.com/Gyanis9/EdgePulse/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Gyanis9/EdgePulse/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Vue](https://img.shields.io/badge/Vue-3-42b883.svg)](https://vuejs.org/)

## 目录

- [为什么是 EdgePulse](#为什么是-edgepulse)
- [特性](#特性)
- [架构](#架构)
- [快速开始](#快速开始)
- [配置](#配置)
- [运行与端口](#运行与端口)
- [API 与实时通道](#api-与实时通道)
- [安全模型](#安全模型)
- [测试与验收](#测试与验收)
- [部署](#部署)
- [高可用部署](#高可用部署)
- [Agent](#agent)
- [项目结构](#项目结构)
- [Roadmap](#roadmap)
- [贡献](#贡献)
- [许可证](#许可证)

## 为什么是 EdgePulse

边缘节点分散、网络不稳定、证书与固件容易失去追踪。EdgePulse 把这些环节放进同一套控制面：

- 设备身份是一等公民：Agent 走 mTLS，注册与每次数据面上报都绑定叶证书指纹；一次性注册/改绑令牌负责准入，证书到期与吊销可查、可断链。
- 运维动作形成闭环：心跳超时自动离线并生成系统告警，期望配置有版本、漂移检测、修订历史与回滚，固件在 Ed25519 验签通过后才允许下发。
- 部署形态克制：控制面是单个 C++23 可执行体，开发档用 SQLite，生产档用 MySQL，Redis 承担跨实例会话、验证码挑战与限流计数、登录失败计数、租户状态缓存与实时扇出，前端是可由后端直接托管的 Vue 3 静态站点。
- 默认值偏安全：生产样例要求注册令牌、固件签名、强口令与登录锁定；运维口代码默认只监听回环，生产样例与 Compose 为容器抓取显式绑 0.0.0.0；Webhook 出站默认拒绝私网与云元数据地址。

## 特性

| 领域 | 能力 |
|---|---|
| 认证与权限 | 图形验证码、PBKDF2 登录、OIDC 授权码 + PKCE、不透明 Bearer 会话、动态菜单、权限码、数据范围、操作与登录审计、口令策略、账号锁定、全局限流 |
| 企业身份 | API Token（明文只显示一次、只存 SHA-256、scope 与 owner 当前权限取交集、轮换一小时重叠、吊销保留记录）、TOTP 两步验证（加密秘密、一次性恢复码、登录挑战、管理员重置） |
| API 契约与幂等 | OpenAPI 3.1 契约由源码路由生成并在 CI 校验；写请求支持 `Idempotency-Key`，24 小时内重放已完成响应并对内容冲突返回 409 |
| 多租户 | 租户编码登录、平台管理员跨租户、租户级 RBAC/部门/岗位/字典、节点与遥测/告警/固件/命令按租户隔离、停用租户阻断新登录、实时主题按租户分区 |
| 设备生命周期 | Agent mTLS 注册、叶证书指纹绑定、一次性 `enroll`/`rebind` 令牌、内部 CA 按 CSR 自动签发与续期（旧指纹重叠窗口）、证书到期与吊销、心跳超时离线、系统离线告警、节点详情与分组 |
| 遥测与告警 | HTTP 与 UDP 遥测（HMAC-SHA256 防重放）、告警规则与确认/恢复生命周期、Webhook（HMAC 签名）与 SMTP 通知、投递记录与失败原因 |
| 固件与命令 | 流式上传、ETag 与单段 Range 下载、Ed25519 签名与验签门禁、分批灰度与回滚、命令批量/计划/取消、Agent WebSocket 推送与轮询兜底 |
| 实时 | 浏览器 WebSocket 的节点/告警/命令主题、告警 SSE、Redis 跨实例事件扇出、Agent 命令长连接 |
| 高可用 | 双实例共享 MySQL/Redis/固件卷，nginx 统一 8080/8443/9443；`sys_lease` 租约与 fence 单飞、MySQL 迁移锁、跨实例 Agent 命令路由、`/readyz` 五项依赖、优雅停机与崩溃切换 |
| 可观测 | Prometheus HTTP 与业务指标、`/readyz`、运维总览、OTLP 指标出口、操作/登录统计与热力图 |
| 交付与运维 | 设备影子配置（期望/实际/漂移/修订/回滚）、备份与恢复工具、Docker Compose、systemd、生产静态站点、Windows/Linux/前端/密钥扫描 CI |

## 架构

```mermaid
flowchart LR
    Browser["浏览器<br/>Vue 3 管理后台"]
    Agent["edgepulse-agent<br/>边缘节点"]
    Plugins["Plugins<br/>控制器、路由与权限校验"]
    Core["Core<br/>认证、设备、遥测、告警、固件、命令"]
    Base["Base<br/>数据库、密码、JSON、响应、时间"]

    Browser -->|"8080 HTTP<br/>8443 HTTPS + h2 / h3"| Plugins
    Plugins --> Core
    Agent -->|"9443 mTLS<br/>叶证书指纹绑定"| Core
    Core --> Base
    Core --> Database[("SQLite / MySQL")]
    Core --> Cache[("Redis（可选）")]
    Core --> Storage[("固件与信任键目录")]
    Core --> Ops["9100 运维口<br/>默认回环 + Bearer"]
```

依赖方向固定为 `Base ← Core ← Plugins ← App ← Main`：`Base` 不碰业务，`Core` 承载领域服务与 HTTP 交付原语，`Plugins` 只做参数解析、权限校验与响应组装，`App` 负责监听器与工作线程装配，`Main` 只剩进程入口。引擎 `3rd/AsynGyanis` 只读，以 Conan 包消费。

更细的模块、存储表与协议说明见 [docs/架构.md](docs/架构.md)。

## 快速开始

### 前置条件

- Windows + MSVC ≥ 19.40，或 Linux + GCC ≥ 13 / Clang ≥ 17
- CMake ≥ 3.25、Ninja、Conan 2、Python 3、Node.js ≥ 20
- Git（含 submodule）、OpenSSL（开发证书与固件签名）

### 构建后端

```bash
git submodule update --init --recursive

conan create 3rd/AsynGyanis/packaging/conan --build=missing \
  -s compiler.cppstd=23 -s build_type=Release -o "&:with_mysql=True"

conan install . --build=missing -s compiler.cppstd=23 -s build_type=Release
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

`debug` 预设使用 RelWithDebInfo；Windows/MSVC 下强制 `/MD` 运行期库，与 Release 引擎包保持同一 CRT，混用 `/MDd` 会在链接期报 LNK2038。

### 启动开发实例

```bash
bash scripts/dev-certs.sh     # CA、服务器、Agent 与对照证书，写入未跟踪的 var/certs
bash scripts/run-dev.sh       # 生成/读取运维令牌并前台启动 edgepulse-server
```

打开 `http://127.0.0.1:8080`。开发种子账号为 `admin` / `admin123`，只存在于开发迁移，部署后必须改密。

### 前端开发

```bash
cd frontend
npm ci
npm run dev      # http://127.0.0.1:5173，/api 与 /api/ws 代理到 8080
```

生产静态站点：

```bash
bash scripts/build-web.sh
```

脚本把 `frontend/dist` 刷新到未跟踪的 `web/`。后端发现 `web/index.html` 后会自动托管静态文件，并为非 API 深链回退到 SPA 入口。

### 备份与恢复

SQLite 档可在服务运行中取一致快照；恢复前必须停止服务：

```bash
python scripts/backup.py --driver sqlite --db-path var/edgepulse.db \
  --firmware-dir var/firmware --config-dir config \
  --output backups/edgepulse.tar.gz

python scripts/restore.py --archive backups/edgepulse.tar.gz --driver sqlite \
  --db-path var/edgepulse.db --firmware-dir var/firmware --config-dir config \
  --force --dry-run
```

去掉 `--dry-run` 才会真正写入。归档内的 `manifest.json` 会逐文件核对 SHA-256；MySQL 档与更多参数见 [docs/部署.md](docs/部署.md#11-备份与恢复)。

## 配置

`config/dev.yaml` 只放非敏感项；生产从 `config/prod.yaml.example` 复制一份未跟踪的 `config/prod.yaml`。口令与密钥只从环境变量读取：

| 变量 | 用途 |
|---|---|
| `EDGEPULSE_DB_PASSWORD` | MySQL 口令 |
| `EDGEPULSE_REDIS_PASSWORD` | Redis 口令 |
| `EDGEPULSE_OPS_BEARER_TOKEN` | 运维口 Bearer |
| `EDGEPULSE_AGENT_FINGERPRINT` | 可选的单个 Agent 证书固定指纹；为空时只校验证书链 |
| `EDGEPULSE_INITIAL_PASSWORD` | 新建用户与重置密码表单的默认初始口令（预填、可改；不落库、不写日志） |
| `EDGEPULSE_UDP_TELEMETRY_SECRET` | UDP 遥测 HMAC 密钥；启用时至少 16 字节 |
| `EDGEPULSE_OIDC_CLIENT_SECRET` | OIDC 客户端密钥；公共客户端可留空 |
| `EDGEPULSE_MFA_KEY` | 两步验证主密钥，base64 编码的 32 字节；未配置时设置、启用、关闭与换发恢复码返回 503，已启用账号的登录校验也会失败，只能先恢复同一密钥或由管理员重置 |

关键配置项（默认值按开发档 / 生产样例列出）：

| 配置键 | 开发档 | 生产样例 | 说明 |
|---|---|---|---|
| `edgepulse.instance_id` | 空（按 `hostname-pid-random` 生成） | 空；HA 必须显式唯一 | 实例标识，1–128 字符且不含空白；进入日志、租约 holder 与跨实例信封，同时存活的实例必须唯一 |
| `edgepulse.ha.enabled` | `false` | `false`（多实例可改 `true`） | 只影响启动日志的 HA 标记；租约与单飞不依赖它 |
| `edgepulse.agent.enrollment_mode` | `off` | `required` | `required` 时新节点必须持 `enroll` 令牌，换证书必须持 `rebind` 令牌 |
| `edgepulse.agent.heartbeat_interval_seconds` | `10` | `30` | Agent 心跳周期 |
| `edgepulse.agent.heartbeat_timeout_seconds` | `30` | `90` | 超过该时长未心跳即转离线 |
| `edgepulse.agent.offline_sweep_seconds` | `5` | `30` | 离线扫描周期，必须小于心跳超时 |
| `edgepulse.agent_ca.enabled` | `true` | `false` | 启用控制面内部 CA，Agent 可用 `POST /api/agent/certificate/renew` 提交 CSR 换证 |
| `edgepulse.agent_ca.certificate_file` | `var/certs/ca.pem` | `/etc/edgepulse/tls/ca.pem` | 签发 CA；9443 监听开启时必须就是它的信任锚（或包含在拼接信任包里），启动按 SHA-256 指纹核对；监听关闭时不做这项比对 |
| `edgepulse.agent_ca.key_file` | `var/certs/ca-key.pem` | `/etc/edgepulse/agent-ca-key.pem` | CA 私钥独立路径，POSIX 上要求 0600，不要放进公开证书卷、镜像或版本库 |
| `edgepulse.agent_ca.validity_days` / `overlap_hours` | `30` / `24` | `30` / `24` | 新证书有效期（1–3650 天）与旧指纹重叠窗口（1–168 小时）；Agent 配置固定 `renew_before_days` 时必须小于有效期 |
| `edgepulse.firmware.require_signature` | `false` | `true` | 开启后未签名固件不能上传、部署或回滚 |
| `edgepulse.firmware.trusted_keys_dir` | `config/firmware-keys` | `/etc/edgepulse/firmware-keys` | 只放公钥；启用签名时不能为空，生产样例使用绝对路径 |
| `edgepulse.security.password_min_length` | `8` | `10` | 建号、重置与改密共用 |
| `edgepulse.security.password_require_complexity` | `false` | `true` | 要求同时包含大写、小写、数字与符号 |
| `edgepulse.security.login_failure_window_seconds` | `900` | `900` | 账号失败计数窗口 |
| `edgepulse.security.login_max_failures` | `5` | `5` | 窗口内达到该次数后锁定 |
| `edgepulse.security.login_lock_seconds` | `900` | `900` | 锁定时长 |
| `edgepulse.security.allow_private_webhooks` | `true` | `false` | 打开后跳过整个私网/保留段分类，仅云元数据主机名/地址与链路本地元数据段（169.254/16、fe80::/10）仍拒绝；CGNAT、文档/基准测试段、组播、240/4、IPv6 ULA 等也会放行，只用于受信的自托管网关 |
| `server.rate_limit.requests_per_second` | `0.0` | `0.0`（建议按容量设置） | 全局限流，整数或小数均可，`0` 关闭 |
| `server.rate_limit.burst_capacity` | `1.0` | `1.0`（建议覆盖并发突发） | 令牌桶容量，必须不小于 `1` |
| `server.quic.idle_timeout_seconds` | `30` | `30` | h3 传输层空闲超时（秒），1–3600；`server.limits.idle_timeout_ms` 不控制 h3 连接空闲 |

固件签名密钥只把公钥放进信任键目录，私钥留在签名工作站：

```bash
bash scripts/firmware-signing-keygen.sh prod-2026
bash scripts/sign-firmware.sh var/firmware-signing-key.pem <固件> prod-2026
```

上传时带 `X-Firmware-Signature` 与 `X-Firmware-Key-Id`。启用门禁前，先重新上传历史未签名固件的签名版本。热重载只覆盖会话与验证码相关项；监听地址、端口与整个 `server` 段（含 `server.quic` 空闲超时）需要重启生效。

## 运行与端口

| 端口 | 协议 | 用途 |
|---|---|---|
| 8080/TCP | HTTP/1.1 | 开发档控制台、API、WebSocket、SSE；生产档可托管 SPA |
| 8443/TCP | HTTPS + h2 | 控制台与 API；`Alt-Svc` 通告 h3 |
| 8443/UDP | HTTP/3 | 与公开 API 相同的路由 |
| 9443/TCP | HTTPS + mTLS | Agent 注册、心跳、遥测、命令、固件下载与 Agent WebSocket |
| 9100/TCP | HTTP/1.1 | 运维口；代码默认 `127.0.0.1`，生产样例与 Compose 绑 `0.0.0.0`；`/metrics` 与调试端点需 Bearer |
| 9555/UDP | UDP | HMAC 签名的遥测数据报，默认关闭 |

`/healthz` 与 `/readyz` 匿名可读；`/metrics`、`/debug/*` 必须带 `EDGEPULSE_OPS_BEARER_TOKEN`。`ops_address` 的代码默认是 `127.0.0.1`；`config/prod.yaml.example` 与 Compose 为容器抓取显式设为 `0.0.0.0`，此时必须用主机防火墙或容器网络策略限制来源，只在本机抓取时改回回环并核对监听地址。

`/api/health` 与 `/healthz` 只表示进程存活；`/readyz` 依次检查数据库、启停状态、缓存、实时总线与固件目录，任一不就绪返回 503 和第一条原因。HA 部署把 8080/8443/9443 交给 nginx：8080 与 8443 转发到实例的 8080（8443 在 nginx 终止 TLS），9443 只做 TCP 透传；应用容器不发布这些端口。

## API 与实时通道

- 基础路径 `/api`；除匿名端点外使用 `Authorization: Bearer <token>`。
- 响应包络 `{ code, message, data, traceId }`；分页 `{ pageNum, pageSize, total, rows }`，`pageSize` 上限 100。
- 写接口先过 `requireSession(..., 权限码)`，数据查询带租户边界与数据范围。
- 自动化客户端可带 `Idempotency-Key`（8–128 字符）安全重试写请求；24 小时内同键同内容重放首次响应，内容或权限变化返回 409。认证、Agent、令牌、用户/租户写入与流式固件上传等路径不参与缓存。
- `GET /api/openapi.json` 返回 OpenAPI 3.1 契约，包含 `/api/health`；`frontend/public/openapi.json` 由 `python scripts/generate-openapi.py` 生成，CI 用 `--check` 阻止漂移。契约在服务启动时读入内存，重建 `web/` 后需要重启服务。当前仍是 `/api`，没有 `/api/v1` 前缀。

| 分组 | 代表端点 | 说明 |
|---|---|---|
| 认证 | `/api/auth/login`、`/api/auth/me`、`/api/auth/oidc/*` | 验证码、会话、OIDC 一次性 code 换会话 |
| 两步验证 | `/api/auth/mfa`、`/api/auth/mfa/setup`、`/enable`、`/disable`、`/verify`、`/recovery-codes` | TOTP 设置、登录挑战、恢复码与关闭；管理端重置在 `/api/system/users/:id/mfa/reset` |
| API 令牌 | `/api/system/api-tokens`、`/api/system/api-tokens/:id/rotate` | 一次性明文、scope、轮换与吊销；权限码 `system:token:*` |
| 设备 | `/api/nodes`、`/api/nodes/:id`、`/api/node-groups` | 节点 CRUD 与分组；更新带 `version` 乐观锁 |
| 注册令牌 | `/api/enrollment-tokens` | 列表不回显明文；签发响应只出现一次 `token` |
| 证书 | `/api/nodes/:id/certificate/revoke` | 吊销后清空指纹、置离线并关闭该节点 Agent WebSocket |
| 设备影子 | `/api/nodes/:id/config`、`/revisions`、`/rollback` | 期望配置、漂移、不可变修订与回滚 |
| 遥测告警 | `/api/telemetry/*`、`/api/alarms`、`/api/alarm-rules` | 最新值、序列、告警确认与恢复 |
| 固件命令 | `/api/firmware/*`、`/api/commands/*` | 上传、验签策略、灰度、回滚、批量与计划命令 |
| Agent | `/api/agent/*` | 只挂 9443 mTLS；WebSocket 首条 `hello` 复核叶证书 |
| 实时 | `/api/ws`、`/api/events/alarms` | 浏览器 WebSocket 与告警 SSE |
| 运维 | `:9100/metrics`、`/readyz`、`/debug/*` | Prometheus、就绪与诊断 |

完整字段与状态码见 [docs/API.md](docs/API.md)。

浏览器 WebSocket 升级后 5 秒内发送 `{"type":"auth","token":"..."}`，成功后订阅 `nodes`、`alarms`、`commands` 主题。告警 SSE 用 `event: alarm` 推送，每 15 秒注释保活。

## 安全模型

- 浏览器会话：32 字节不透明 Bearer token，存在 Redis 或进程内存储，TTL 默认 7200 秒并滑动续期；不使用 Cookie 承载会话。
- API Token：明文形如 `ep_api_` 加 64 位十六进制，只在创建或轮换响应出现一次；数据库只存 SHA-256 与前缀。令牌继承 owner 的身份但 `platformAdmin` 恒为 false，每次请求按 owner 当前权限与令牌 scope 的交集授权；令牌吊销/过期、owner 或租户停用都会立即失效。`/api/auth/me` 保留给令牌自省；`/api/auth/routes`、`/api/auth/logout` 等浏览器会话端点、浏览器 WebSocket 与改密入口会拒绝令牌。
- TOTP 两步验证：秘密用 `EDGEPULSE_MFA_KEY` 做 AES-256-GCM 加密后落库（可解密用于校验），恢复码用同一主密钥做 HMAC-SHA256 后存储（不可逆）；两者都不以明文落库。登录挑战放在缓存中、300 秒过期、最多 5 次失败并绑定来源 IP；启用、关闭与换发恢复码对错误动态码另有每用户 5 次/300 秒的失败上限，设置入口不计数。MFA 是用户自助启用，没有租户级强制策略。
- 幂等键：只在带 Bearer 的写请求上生效，按令牌与幂等键的 SHA-256 分区；请求指纹覆盖方法、完整 URI、表示头、权限摘要与正文，权限降级后同键重试返回 409 而不是旧的已授权响应。401、403、5xx 与超过 256 KiB 的响应不缓存，认证、Agent、令牌、用户/租户写入与流式上传路径整段排除，避免一次性明文或口令进入幂等表。
- Agent 身份：9443 在 TLS 层校验客户端证书；注册自报的 SHA-256 指纹必须等于本条连接出示的叶证书；心跳、遥测、命令与固件状态都按叶证书绑定节点。开启 `edgepulse.agent_ca` 后，Agent 可在到期前用 CSR 通过 `POST /api/agent/certificate/renew` 换取新证书，服务端在 `overlap_hours` 内同时接受旧指纹；吊销会清空当前与上一枚指纹、置离线并以 1008 关闭已建立的 Agent WebSocket。
- 注册令牌：`enroll` 与 `rebind` 一次性使用，数据库只存 SHA-256，明文只在签发响应出现一次；有效期限制在 300 到 2592000 秒。
- 固件真实性：Ed25519 对文件 SHA-256 的 32 字节摘要签名，算法标识 `ed25519-sha256`；`require_signature=true` 时未签名固件不能上传、部署或回滚。
- 运维口：代码默认只监听回环；生产样例与 Compose 绑 `0.0.0.0`，须由防火墙或网络策略限制来源。`/metrics` 与 `/debug/*` 需要 Bearer，`/healthz` 与 `/readyz` 匿名。
- 出站 SSRF：Webhook 默认拒绝回环、私网、链路本地与云元数据地址，每次投递前复检；开启 `allow_private_webhooks` 会跳过整个私网/保留段分类，只保留云元数据主机名/地址与链路本地元数据段的拒绝，CGNAT、文档/基准测试段、组播、`240/4`、IPv6 ULA 等也会放行，只应在受信的自托管网关开启。
- 登录加固：口令策略、按账号失败锁定、按 IP 登录限频与全局令牌桶限流；审计日志对 `password`、`token`、`captcha` 等字段脱敏。

完整报告渠道、轮换步骤与已知边界见 [SECURITY.md](SECURITY.md)。

## 测试与验收

```bash
# 后端：单元、集成与协议用例
ctest --preset debug --output-on-failure

# 按名称过滤
ctest --preset debug -R "Enrollment|NodeConfig|NodeLifecycle" --output-on-failure

# 前端：API/工具单测 + 类型检查与构建
cd frontend
npm run test:unit
npm run build

# E2E：使用本机已安装的 Edge，不下载浏览器；先准备开发证书，Playwright 会启动后端与 Vite
npm run test:e2e
cd ..

# 全量协议与安全验收
python scripts/generate-openapi.py --check
python scripts/check-acceptance.py

# Agent 端到端：真实 server/agent 二进制，临时 SQLite 库
python scripts/check-agent-e2e.py

# SQLite 备份与恢复：往返、边车清理、路径与私钥过滤
python scripts/backup_restore_test.py

# 真机外部依赖：一次性 MySQL 8.4 与 Redis 7 容器，断言 7 项环境门控用例全部执行
python scripts/run-external-tests.py

# MySQL 备份与恢复往返：同样自带一次性容器
python scripts/backup_restore_mysql_test.py

# Docker 全栈：镜像、MySQL/Redis、Prometheus、OTel collector 与优雅停机
python scripts/check-docker-stack.py --build

# HA 双实例：Compose 起 MySQL/Redis/两实例/nginx，断言跨实例会话、命令路由、优雅停机与崩溃切换
python scripts/check-ha-stack.py --require-docker
```

- 2026-10-24 全量回归（`feat/p6-production-agent` @ `bca5b24`）：`ctest --preset debug` 注册 306 项，通过 302，跳过 4，失败 0，耗时 102.95 秒；`npm run test:unit` 30 项、`npm run test:e2e` 9 项全部通过，`npm run build` 类型检查与构建通过；`python scripts/check-acceptance.py` 通过 86 项、失败 0、跳过 0；`python scripts/check-agent-e2e.py` 通过 50 项、失败 0、跳过 0；`python scripts/backup_restore_test.py` 6 项中 5 项通过，1 项 Windows 文件权限位用例按平台跳过；`python scripts/run-external-tests.py` 三项门控用例全部通过、无跳过；`python scripts/backup_restore_mysql_test.py` 完成 MySQL 备份与恢复往返。
- 2026-10-25 P7 门禁（代码基线 `8e3885c`，只含文档改动）：`cmake --build --preset debug` 无待编译目标；`ctest --preset debug` 注册 401 项，通过 397，跳过 4，失败 0，耗时 143.85 秒；`python scripts/generate-openapi.py --check` 无漂移；前端单测 42 项、E2E 18 项全部通过，`npm run build` 通过且产物含 `openapi.json`；`python scripts/check-acceptance.py` 通过 86 项、失败 0、跳过 0；`python scripts/check-agent-e2e.py` 通过 50 项、失败 0、跳过 0。
- 2026-10-25 P8 门禁（`feat/enterprise-hardening`，最终评审修复后）：`cmake --build --preset debug` 退出 0；`ctest --preset debug` 注册 512 项，通过 507，跳过 5，失败 0，耗时 167.96 秒；`python scripts/generate-openapi.py --check` 无漂移；前端单测 42/42、E2E 20/20 通过（38.8 秒），`npm run build` 通过；`python scripts/check-acceptance.py` 通过 86 项、失败 0、跳过 0；`python scripts/check-agent-e2e.py` 通过 66 项、失败 0、跳过 0，覆盖短期证书自动续期、PID 不变、旧指纹重叠窗口、WebSocket 命令通道与续期后的心跳/遥测/命令。
- 2026-10-25 P9 门禁（`feat/enterprise-hardening` @ `ace30dd`，只含文档与外部用例过滤修正）：`cmake --build --preset debug` 无待编译目标；`ctest --preset debug` 注册 553 项，通过 544，跳过 9，失败 0，耗时 184.86 秒；`python scripts/check-ha-stack.py --require-docker` 56 项全部通过（跨实例会话、命令 0.02 秒送达、SIGTERM 后 1.75 秒接管、SIGKILL 后 16.71 秒接管且 fence 2→3）；`python scripts/generate-openapi.py --check` 无漂移；`python scripts/run-external-tests.py` 7/7 通过；前端单测 42/42、E2E 20/20（38.7 秒），`npm run build` 通过；`python scripts/check-acceptance.py` 通过 86 项、失败 0、跳过 0；`python scripts/check-agent-e2e.py` 通过 66 项、失败 0、跳过 0。
- 远程 Linux 实测（阿里云 ECS，Ubuntu 24.04/systemd 255）：真实 `edgepulse-agent` 的 50 项端到端检查通过；`install-agent.sh` 安装后服务为 active，`systemctl stop/start` 正常，注册退避期间停止立即生效，`uninstall-agent.sh --purge` 清理二进制、unit、配置、状态与日志。
- `ExternalStores.*`、`RealtimeBus.Redis*` 与 `AgentCommandBus.RedisFanout*` 需要真实 MySQL/Redis：`scripts/run-external-tests.py` 启动只监听回环随机端口的一次性容器并注入 `EDGEPULSE_TEST_MYSQL_*`、`EDGEPULSE_TEST_REDIS_*`，随后断言 7 项用例确实执行且输出不含 Skipped；镜像可用 `--mysql-image`/`--redis-image` 覆盖。直接跑 `ctest` 缺变量时仍 SKIP，不伪造通过。
- 并发与时序断言由用例自己构造条件，不依赖机器上已安装的组件或调度运气。
- CI 跑 Windows/Linux 后端构建与 ctest、Linux 后端 MySQL 8.4/Redis 7 service 与 MySQL 备份往返、Agent 端到端、前端单测/构建、密钥扫描与 actionlint；Linux 后端任务单独断言外部用例没有被 Skip。Playwright 在本地用系统 Edge 运行。
- 验收证据与逐项输出见 [docs/验收.md](docs/验收.md)。

## 部署

- Docker Compose：`cd deploy/docker && cp .env.example .env`，填好 MySQL/Redis/运维令牌后 `docker compose up -d`；需要两步验证时再填入 `EDGEPULSE_MFA_KEY`（`openssl rand -base64 32`）。样例含 MySQL 8、Redis 7 与 Prometheus。
- 双实例 HA：同一目录的 `docker-compose.ha.yml` 起 MySQL、Redis、`edgepulse-1/2` 与 nginx，两个实例共享配置、证书、固件卷与 Redis，只差 `EDGEPULSE_INSTANCE_ID`；见下文[高可用部署](#高可用部署)。
- systemd：`deploy/systemd/edgepulse.service` 是单实例模板，`edgepulse@.service` 是按实例名模板化的多实例单元（`/etc/edgepulse/<实例>.yaml` 与 `<实例>.env`）；二进制、`web/`、`migrations/` 与配置分目录放置，秘密走 `EnvironmentFile`。
- 静态站点：`bash scripts/build-web.sh`，由后端直接托管。
- 生产至少核对：MySQL 档、多实例时配置 Redis（跨实例会话、验证码与登录失败计数、租户状态、两步验证挑战与尝试计数、实时扇出与 Agent 命令路由）、每个实例唯一的 `edgepulse.instance_id`、共享固件目录、服务器证书与 Agent CA、`enrollment_mode=required`、`firmware.require_signature=true` 且信任键目录只含公钥、强口令与登录锁定、非零全局限流（HA 按实例计数，N 个实例各配总额的 1/N）、运维口仅回环或仅对受信来源开放（防火墙）、`allow_private_webhooks=false`、可用备份。

备份归档包含数据库快照、固件与非敏感配置，逐文件 SHA-256 校验；恢复必须先停服务，先 `--dry-run` 再执行。完整步骤见 [docs/部署.md](docs/部署.md)。

## 高可用部署

控制面支持双实例 active/active：两个实例共享 MySQL、Redis 与固件目录，nginx 在前面分发浏览器流量，Agent 仍走 mTLS 9443。

```mermaid
flowchart LR
    Browser["浏览器"] -->|"8080 / 8443"| Nginx["nginx<br/>8443 终止 TLS"]
    Agent["edgepulse-agent"] -->|"9443 mTLS<br/>TCP 透传"| Nginx
    Nginx --> A["edgepulse-1"]
    Nginx --> B["edgepulse-2"]
    A --> MySQL[("共享 MySQL")]
    B --> MySQL
    A --> Redis[("共享 Redis")]
    B --> Redis
    A --> Firmware[("共享固件卷")]
    B --> Firmware
```

- 实例身份：每个实例显式配置唯一的 `edgepulse.instance_id`；Docker HA 编排用入口脚本按 `EDGEPULSE_INSTANCE_ID` 生成各自的配置副本，两个容器只共享一份只读 `prod.yaml`。
- 租约与单飞：`sys_lease` 的 `control-plane` 行是唯一协调点，TTL 15 秒、每 5 秒续约；条件 UPDATE 命中才算 leader，接管时 fence 递增。只有 leader 执行计划命令调度、保留期清理、离线扫描与离线告警；指标读数每个实例都刷新。旧 leader 恢复后不能续约；崩溃重启的实例要等一个 TTL 才能重新当选。
- 迁移：MySQL 迁移前持有 `GET_LOCK('edgepulse:migrations:<库名>', 30)`，并发启动的实例串行迁移，拿不到锁的实例拒绝启动。SQLite 仍是单实例开发档。
- Agent 命令：先在本地投递，再经 Redis 的 `target=agent` 信封转给持有该节点连接的实例；Agent 在已连接时仍每 60 秒轮询一次，断开时按 `command_poll_interval_seconds`（默认 15 秒）轮询，重复投递按命令标识去重。
- 就绪：`/readyz` 依次检查数据库、启停状态、缓存探针、实时总线与固件目录可写，任一不就绪返回 503 和第一条原因；容器健康检查与 nginx 摘流都以它为准，`/api/health` 只表示进程存活。
- 证书分工：应用证书目录只放 `ca.pem`、`server.pem` 与 `server-key.pem`；nginx 只拿 `server.pem` 与 `server-key.pem`；Agent CA 私钥走独立 Compose secret 与 0400 命名卷，nginx 与应用证书卷都看不到它。

快速开始：

```bash
cd deploy/docker
cp .env.example .env          # 填 MySQL/Redis/运维令牌，按需填 MFA 密钥
# 准备 certs/app、certs/nginx、config/firmware-keys 与 CA 私钥，见 .env.example 注释
docker compose -f docker-compose.ha.yml up -d --build
curl -fsS http://127.0.0.1:8080/api/health
docker compose -f docker-compose.ha.yml exec -T mysql \
  sh -c 'MYSQL_PWD="$MYSQL_PASSWORD" mysql -uedgepulse edgepulse -e "SELECT holder,fence,expires_at FROM sys_lease"'
```

故障演练与滚动升级：

- 优雅停机：向实例发 SIGTERM，`/readyz` 先变 503 并立即清空租约 holder，随后按 HTTP、HTTPS、mTLS、QUIC、Ops 顺序排空，容器 `stop_grace_period` 为 40 秒。实测停机后新 leader 在 1.75 秒内接管、fence 递增。
- 崩溃切换：`SIGKILL` 后旧 holder 留在租约行直到 TTL 过期，新 leader 在 2 个 TTL（30 秒）内接管；实测 16.71 秒，fence 由 2 升到 3，重启的旧 leader 不抢占有效租约。
- 滚动升级：一次只动一个实例，先摘流（或 `docker compose stop edgepulse-1`），等 `/readyz` 503 并退出，再用新镜像 `up -d edgepulse-1`；新实例 `/readyz` 200 后 nginx 自动接回。两个实例不要同时重启，否则 nginx 没有可用上游，Agent 长连接与在途请求都会中断。
- 全量门禁：`python scripts/check-ha-stack.py --require-docker` 覆盖跨实例会话、Agent 命令路由、nginx 8080/8443/9443、共享固件、CA 私钥权限、SIGTERM 与 SIGKILL 两条停机路径。

已知限制：

- MySQL 与 Redis 仍是单点；本阶段不做存储层高可用、跨地域多活与自动分片。
- 开源自建 nginx 只有被动健康检查（`max_fails=2 fail_timeout=5s`），故障瞬间可能有一个请求收到 502 或超时；upstream 主机名在启动时解析一次，容器重建换 IP 后要 reload 或重启 nginx。
- 限流按实例计数：总额定速率 R 要拆成 N 份，`burst_capacity` 同样除 N；nginx 不传真实来源地址，应用也不信任 `X-Forwarded-For`，登录限频、MFA 来源绑定与审计看到的是代理地址。
- 固件目录必须共享卷；只挂本地卷会让上传与下载落到不同实例。SQLite 不参与 HA。
- 所有实例必须显式配置唯一 `edgepulse.instance_id` 并由 NTP 对齐时钟；时钟偏差超过 TTL 时，仍存活的 holder 可能被误判为过期。
- 租户配额、按租户或按 Agent 的数据面限流、命令并发上限不在本阶段范围内。

## Agent

`edgepulse-agent` 是边缘节点上的数据面进程，通过 9443 mTLS 接入控制面。Linux 与 Windows 发行包都包含二进制、安装/卸载脚本与配置模板，Linux 包另含 systemd 单元；包内 `manifest.json` 逐文件记录 SHA-256，`checksums.sha256` 供安装器复核。签名包的信任根是归档本身：打包脚本另写 `<包名>.sig`，它是归档 SHA-256 的 Ed25519 签名，`manifest.sig` 留在归档内作为纵深防御。

Linux 节点侧准备：

- 节点证书、私钥与 CA 默认读取 `/etc/edgepulse/agent.pem`、`/etc/edgepulse/agent-key.pem` 与 `/etc/edgepulse/ca.pem`，私钥权限保持 0600。`ca_file` 同时用于验证 9443 服务器证书链与续期返回的 Agent 证书链；独立 CA 部署时必须把服务器链根与所有 Agent CA 拼成一个信任包，缺一项会导致握手或续期安装失败。开发环境可先运行 `bash scripts/dev-certs.sh`，再用 `bash scripts/gen-agent-certs.sh <节点名> [--days <天数>] [--out-dir <目录>]` 为单个节点签发证书。
- 自动续期由 `agent.certificate.renewal_enabled`（默认 true）、`renew_before_days`（默认 0，0–90）与 `renewal_check_seconds`（默认 3600，60–86400）控制：窗口按 `max(25% 当前证书生命周期, 24h)` 计算，`0` 表示使用该公式；非 0 覆盖值只在小于证书有效期时生效，否则记录 ERROR 并回退公式。进入窗口后生成新私钥与 CSR 调续期端点，校验服务端签回的链与有效期，原子替换证书后热切换 HTTP 与 WebSocket，进程不重启。安装失败会用同一份签发结果重试，不会重新申请；HTTP/WebSocket 重载失败保留新证书并持续复查，进程重启直接加载新的一对。使用外部 CA 时把 `renewal_enabled` 置 false，Agent 不启动续期循环，证书到期前按 `rebind` 流程手工换证。控制面返回 403 时停止自动续期，需要重新签发 `rebind` 令牌并重新入场；其余可重试失败按 5/10/30/60/300 秒退避，HTTP 400 在默认序列上最长等待 20 分钟（300 秒 × 4）。
- 在控制台签发一次性的 `enroll` 令牌（已有节点更换证书时签发 `rebind`），安装后写入 `/etc/edgepulse-agent/agent.env`，例如 `EDGEPULSE_AGENT_ENROLLMENT_TOKEN=...`。令牌只从环境变量读取，不写 YAML、不写日志。

安装与卸载（Linux）：

```bash
# 在 Linux 上构建 Release 并打包；也可用 --binary 指定已有二进制
bash scripts/package-agent.sh --version 1.0.0

# 签名包：对归档 SHA-256 生成 .sig 与 .json，Agent 用对应公钥验签
bash scripts/package-agent.sh --version 1.0.0 \
  --signing-key var/agent-package-key.pem --signing-key-id prod-2026

mkdir -p /tmp/edgepulse-agent
tar -xzf build/agent-package/edgepulse-agent-*.tar.gz -C /tmp/edgepulse-agent
sudo /tmp/edgepulse-agent/install-agent.sh
# 安装器写入 /usr/local/bin/edgepulse-agent 与 systemd 单元，创建配置、状态与日志目录并启动服务；
# 已有 agent.yaml、agent.env 不会被覆盖。只安装不启动时加 --no-start。

sudo /tmp/edgepulse-agent/uninstall-agent.sh          # 保留配置、状态与日志
sudo /tmp/edgepulse-agent/uninstall-agent.sh --purge  # 一并删除
```

普通安装会把安装器复制到 `<prefix>/lib/edgepulse/install-agent.sh`，并把配置里的 `agent.update.installer_script` 替换为该路径。控制面下发 `agent.update` 时，命令给出归档 SHA-256 与覆盖该摘要的 Ed25519 签名；Agent 验签通过才下载，并核验下载摘要一致，然后调用安装器 `--upgrade`。安装器校验已认证归档内的 `checksums.sha256`、manifest 版本与平台，清理旧标记、备份旧二进制、停止服务，只替换二进制，再写入 `<state>/update-expected` 并等待新版本在首次成功心跳后写出的 `<state>/update-health`；超时或启动失败则恢复旧二进制、写入 `<state>/update-rollback` 并重启旧版本。升级成功与回滚都会用最终结论替换此前“安装器已启动”的结果，同一命令只上报一条；替换前失败时安装器用旧二进制重新启动服务。手工 `--upgrade` 没有待上报命令，新版本按 `update-expected` 的版本确认健康；内容与运行版本不符时保留标记，由安装器回滚。安装器日志在 `<state>/update-installer.log`。打包时提供 `--signing-key` 与 `--signing-key-id` 会写出 `<包名>.sig` 与 `<包名>.json`，Agent 用 `agent.update.package_trusted_keys_dir` 里的公钥验证归档签名；手工运行安装器只校验哈希，不验签名。

默认路径为配置 `/etc/edgepulse-agent`、状态 `/var/lib/edgepulse-agent`、日志 `/var/log/edgepulse-agent`；`--prefix`、`--config-dir`、`--state-dir`、`--log-dir` 可整体改到自定义位置，单元里的 `ExecStart`、`EnvironmentFile` 与 `ReadWritePaths` 会同步替换。运行状态看 `systemctl status edgepulse-agent`，日志看 `journalctl -u edgepulse-agent -f`，完整步骤见 [docs/部署.md](docs/部署.md#13-agentlinux)。

Windows：

```powershell
# 构建 Release 并打包，输出 build\agent-package\edgepulse-agent-<version>-windows-x86_64.zip
powershell -ExecutionPolicy Bypass -File scripts\package-agent.ps1 -Version 1.0.0

Expand-Archive build\agent-package\edgepulse-agent-1.0.0-windows-x86_64.zip -DestinationPath $env:TEMP\edgepulse-agent -Force
# 以管理员身份打开 PowerShell 后安装；默认安装到 C:\Program Files\EdgePulse\Agent，
# 数据放在 C:\ProgramData\EdgePulse\Agent，已有 agent.yaml 不会被覆盖。
# 首次安装先加 -NoStart，准备好证书与配置后再 sc.exe start EdgePulseAgent
powershell -ExecutionPolicy Bypass -File $env:TEMP\edgepulse-agent\install-agent.ps1 -NoStart

powershell -ExecutionPolicy Bypass -File $env:TEMP\edgepulse-agent\uninstall-agent.ps1          # 保留数据
powershell -ExecutionPolicy Bypass -File $env:TEMP\edgepulse-agent\uninstall-agent.ps1 -Purge  # 一并删除数据
```

服务名默认 `EdgePulseAgent`，可用 `-ServiceName` 自定义，安装器会把同一个名字写进服务命令行的 `--service-name`；账户 LocalSystem，安装器配置自动启动与 5 秒、10 秒、30 秒三次失败重启。注册前把 `ca.pem`、`agent.pem` 与 `agent-key.pem` 放进 `C:\ProgramData\EdgePulse\Agent\certs`。注册令牌通过服务作用域环境变量提供（服务注册表项的 `Environment` 值）后重启服务；数据目录只授予 Administrators 与 SYSTEM，证书与私钥不要放到其他可写位置。运行日志在 `C:\ProgramData\EdgePulse\Agent\agent.log`，服务控制错误看 `Get-EventLog -LogName System -Source "Service Control Manager"`。

Windows 升级同样由 `agent.update` 触发：Agent 先对命令里的归档摘要验签，下载后核对摘要，再调用安装器 `-Upgrade`；安装器校验已认证归档内的 `checksums.sha256`、manifest 版本与平台，清理旧标记、备份 exe、停服务、替换 exe，写入 `<数据目录>\update-expected` 并等待新版本在首次成功心跳后写出 `C:\ProgramData\EdgePulse\Agent\update-health`；超时或启动失败则恢复 exe、写入 `update-rollback` 并启动旧服务，替换前失败时也会重新启动已停止的旧服务。手工 `-Upgrade` 同样靠 `update-expected` 确认健康。升级结果替换此前“安装器已启动”的结果。升级模式固定服务名 `EdgePulseAgent`，配置、状态与安装脚本副本保持不变；日志在 `C:\ProgramData\EdgePulse\Agent\update-installer.log`。完整步骤见 [docs/部署.md](docs/部署.md#14-agentwindows)。

`agent.restart` 由 Agent 在命令结果入队并尝试上报之后触发：Linux 通过 `systemd-run --collect` 启动延迟的 `systemctl restart edgepulse-agent`，Windows 通过脱离的 `powershell.exe` 调用 `Restart-Service -Name 'EdgePulseAgent' -Force`，随后当前进程请求停止。启动器失败只记 ERROR 并保持进程运行，不会谎报重启成功；Windows 自定义服务名不支持该命令。

## 项目结构

```text
src/
├─ Base/       技术基座：响应、分页、CSV、数据库、密码、随机数、时间、JSON、安全原语
├─ Core/       领域与交付原语：认证、系统、设备、遥测、告警、固件、命令、缓存、HTTP、插件契约
├─ Plugins/    功能插件：认证、系统、监控、工作台、设备、遥测、告警、固件、命令、运维
├─ App/        配置、生命周期、监听器与工作线程装配
└─ Main/       进程入口
agent/        生产 Agent 源码与 Linux/Windows 打包脚本
frontend/      Vue 3 + Vite 管理后台与 Playwright E2E
3rd/AsynGyanis/  引擎 git submodule，只读，以 Conan 包消费
migrations/    SQL 迁移 V1–V21，已应用文件不可修改
config/        非敏感配置样例；生产配置与信任键目录不入库
deploy/        Docker（含 HA 编排）、nginx、systemd、Prometheus
scripts/       证书、开发运行、固件签名、备份恢复、HA 与验收
samples/       DeviceAgent（协议探针 edgepulse-agent-probe）、FirmwareVerifier、H3Probe 样例
tests/         GoogleTest 单元、集成与协议用例
docs/          架构、API、部署、验收与覆盖率
.github/       CI 工作流、Issue 与 PR 模板
```

生产 Agent 入口是 `bin/edgepulse-agent`；Linux systemd 单元与安装包见上文 Agent 一节，`samples/DeviceAgent` 是协议探针。

## Roadmap

P7 已交付 API Token、TOTP 两步验证、写请求幂等键与 OpenAPI 3.1 契约；P8 已交付控制面内部 CA、Agent CSR 续期与旧指纹重叠窗口；P9 已交付控制面双实例 HA：租约与 fence 单飞、MySQL 迁移锁、跨实例 Agent 命令路由、五项依赖的 `/readyz` 与 Compose/nginx/systemd 编排。仍延后的能力见 [docs/ROADMAP.md](docs/ROADMAP.md)：`/api/v1` 版本前缀、OTLP tracing、跨地域多活与存储层 HA、通用事件订阅、OTA 自动灰度、批量节点编辑/CSV 导入，以及租户级 MFA 强制策略、按令牌或按租户限流等后续项。

## 贡献

- 先读 [CONTRIBUTING.md](CONTRIBUTING.md)：构建、测试过滤、迁移编号、提交规范与秘密卫生。
- 参与即表示同意 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。
- 安全问题不要开公开 Issue，按 [SECURITY.md](SECURITY.md) 的私密渠道报告。
- 引擎目录只读；发现引擎缺陷时记录最小复现，在应用层绕过，不修改 submodule。

## 许可证

[Apache License 2.0](LICENSE)，Copyright 2026 EdgePulse contributors。
