# EdgePulse

EdgePulse 是面向边缘节点与设备的运维平台：控制面是 Vue 3 管理后台，数据面是运行在边缘节点上的 `edgepulse-agent`。后端是 C++23 + AsynGyanis 异步引擎，前后端通过统一的 `/api` 契约分离。

当前状态：P0–P4 已落地并通过验收（2026-10-21）。

- 认证与权限：验证码、PBKDF2 登录、OIDC 授权码登录、不透明会话、动态菜单、权限码、数据范围、操作审计。
- 多租户：租户编码登录、平台管理员跨租户、租户管理员本租户隔离；用户、节点、分组、遥测、告警、固件、命令、日志、角色、部门、岗位与字典按租户隔离，实时 WebSocket/SSE 按租户分区；菜单与参数配置保持平台级共享。
- 系统管理：用户、角色、菜单、部门、岗位、字典、参数、租户、通知渠道、操作日志、登录日志、CSV 导出。
- 设备域：节点、节点分组、Agent mTLS 注册/心跳、遥测、告警规则与告警生命周期、Webhook/邮件告警通知、固件上传/下载/分批灰度下发与回滚、命令批量与计划执行。
- 实时：浏览器 WebSocket（节点、告警、命令主题）、告警 SSE、Redis 跨实例事件扇出、Agent 命令长连接与轮询兜底、HMAC 签名的 UDP 遥测入口。
- 边缘入口：可选的 HTTP 反向代理前缀，把受控请求转发到上游服务。
- 运行时：SQLite 本地档、MySQL 生产档、Redis 可选会话/验证码后端、配置热重载、数据库健康探针。
- 可观测：Prometheus 指标、`/readyz`、业务运维总览、OTLP 指标出口开关。
- 交付：生产静态站点回退、Docker Compose、systemd、Prometheus 抓取配置。

## 目录

```text
src/
├─ Base/       技术基座：响应、分页、CSV、数据库、密码、随机数、时间、JSON
├─ Core/       领域与交付原语：认证、系统、设备、遥测、告警、固件、命令、缓存、HTTP、插件契约
├─ Plugins/    功能插件：认证、系统、监控、工作台、设备、遥测、告警、固件、命令、运维
├─ App/        配置、生命周期、监听器与工作线程装配
└─ Main/       进程入口
frontend/      Vue 3 + Vite 管理后台
third_party/AsynGyanis/  引擎 git submodule，只读
migrations/    SQL 迁移 V1–V13
config/        非敏感配置样例
deploy/        Docker、systemd、Prometheus
scripts/       证书、开发运行、验收
docs/          架构、API、部署、验收、覆盖率与设计/计划
```

依赖方向固定为 `Base ← Core ← Plugins ← App ← Main`：基础件不碰业务，Core 承载领域服务与 HTTP 交付原语，Plugins 只放控制器与路由，App 负责装配，Main 只剩入口。

## 引擎版本

引擎使用重构后的 AsynGyanis 1.0.0 线。该线源码已重组为 Platform/Base/Core/Net/Database 五库，仓库尚未在该线上打新 tag，因此子模块钉在提交 `5a4e9769`；本地 Conan 包版本为 `asyngyanis/1.0.0`，构建时打开 MySQL 驱动。

引擎只作为 Conan 包消费：不修改、不复制进 `src/`、不 `add_subdirectory`。

## 前置条件

- Windows + MSVC ≥ 19.40 或 Linux + GCC ≥ 13 / Clang ≥ 17
- CMake ≥ 3.25、Ninja、Conan 2、Python 3、Node.js ≥ 20
- Git（含 submodule）、OpenSSL（生成开发证书）

## 构建

```bash
git submodule update --init --recursive

conan create third_party/AsynGyanis/packaging/conan --build=missing \
  -s compiler.cppstd=23 -s build_type=Release -o "&:with_mysql=True"

conan install . --build=missing -s compiler.cppstd=23 -s build_type=Release
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

`debug` 预设使用 RelWithDebInfo，并强制 `/MD` 运行期库：引擎包只有 Release 档，混用 `/MDd` 会在链接期报 LNK2038。

## 运行

```bash
bash scripts/dev-certs.sh          # CA、服务器、Agent 与对照证书（var/certs，未跟踪）
bash scripts/run-dev.sh            # 生成/读取运维令牌并启动 edgepulse-server
```

默认监听：

| 端口 | 用途 |
|---|---|
| 8080/TCP | 开发档 HTTP + API + WebSocket；生产档可托管 SPA |
| 8443/TCP | HTTPS + h2 控制台，Alt-Svc 通告 h3 |
| 8443/UDP | HTTP/3 |
| 9443/TCP | Agent mTLS（要求客户端证书） |
| 9100/TCP | 运维口，仅回环；`/metrics`、`/debug/*` 需 Bearer，`/readyz` 匿名 |

开发种子账号：`admin` / `admin123`（只在开发迁移里，部署后必须改密）。

前端开发：

```bash
cd frontend
npm ci
npm run dev      # http://127.0.0.1:5173，/api 与 /api/ws 代理到 8080
```

生产静态站点：`bash scripts/build-web.sh` 会把 `frontend/dist` 刷新到未跟踪的 `web/`；后端启动时若发现 `web/index.html`，自动提供静态文件与 SPA 深链回退。

## 验收

```bash
# 后端：92 条用例（真机存储与 Redis 扇出用例按环境变量门控）
ctest --preset debug --output-on-failure

# 前端：23 条单测 + 类型检查/构建 + 6 条 Playwright
cd frontend && npm run test:unit && npm run build && npm run test:e2e

# 全量协议与安全验收：认证、租户隔离、系统管理、节点、遥测告警、SSE、命令、100 MiB 固件、h3、mTLS、metrics、readyz
python scripts/check-acceptance.py

# Docker 全栈：镜像 + MySQL/Redis + Prometheus + 真实 OTel collector + 优雅停机
python scripts/check-docker-stack.py --build
```

结果与证据见 [docs/验收.md](docs/验收.md)。

## 配置与秘密

- `config/dev.yaml` 只放非敏感项；生产从 `config/prod.yaml.example` 复制一份未跟踪的配置。
- 数据库/Redis 口令、运维令牌、Agent 指纹、初始口令提示只从环境变量读取：
  `EDGEPULSE_DB_PASSWORD`、`EDGEPULSE_REDIS_PASSWORD`、`EDGEPULSE_OPS_BEARER_TOKEN`、
  `EDGEPULSE_AGENT_FINGERPRINT`、`EDGEPULSE_INITIAL_PASSWORD`、`EDGEPULSE_UDP_TELEMETRY_SECRET`、`EDGEPULSE_OIDC_CLIENT_SECRET`。
- 开发脚本把运维令牌写进 `var/ops-token`，证书写进 `var/certs`，两者都在 `.gitignore` 中。
- 仓库零明文凭据；日志不回显 Authorization、Cookie、口令与 token。

## 测试

- 后端单元：密码、验证码、会话、CSV、KV、分页、时间。
- 后端集成：迁移 V1–V13、认证、租户隔离、租户级角色/部门/岗位/字典、通知渠道、日志与节点的数据范围、遥测、告警、计划/批量命令、固件灰度/回滚、Redis 跨实例扇出、真机 MySQL/Redis（环境变量门控）。
- 协议：401/403 矩阵、租户生命周期与跨租户隔离、管理端 CRUD、数据范围、遥测告警、命令往返与 Agent WebSocket 推送、固件 Range/ETag、节点分组、WebSocket 首条认证、h3、mTLS。
- 前端：API 契约单测；Playwright 用系统 Edge（`channel: 'msedge'`），不下载浏览器。
- `ExternalStores.*` 需要真实 MySQL/Redis：设置 `EDGEPULSE_TEST_MYSQL_*` 与 `EDGEPULSE_TEST_REDIS_*` 后运行；缺变量时用例 SKIP，不伪造通过。本机已用临时容器跑绿。
- Docker：`scripts/check-docker-stack.py` 起 MySQL 8.4、Redis 7、Prometheus 与真实 OTel collector，验证登录、自定义角色、节点、审计落库、会话跨重启、指标抓取与 SIGTERM 优雅停机。
- UDP 遥测：`scripts/check-udp-telemetry.py` 用真实数据报验证 HMAC 签名、重放拒绝、落库与回读。

## 已知边界

- 多租户：用户、节点、分组、遥测、告警、固件、命令、日志、角色、部门、岗位与字典按租户隔离；菜单与参数配置是平台级共享。租户管理员只能创建自己权限范围内的角色，不能管理租户或平台级配置。
- 通知渠道支持 Webhook（HMAC-SHA256 签名）与 SMTP 邮件（smtp://、smtps:// 隐式 TLS 或 starttls://）；私有 CA 可在渠道里指定 PEM 文件。短信通过 Webhook 网关接入。
- 平台管理员可在节点创建与固件上传时选择目标租户；未指定时落在自己的租户。Agent 注册用 `tenantCode` 声明归属。
- 停用租户会阻止新登录，并在数秒内拒绝已签发会话（租户状态缓存 5 秒，更新时主动失效）。
- 固件灰度按批次手动推进，不做自动健康门控；回滚依赖任务里记录的上一版本。
- 命令批次可在列表里整批取消；已取消命令不会再被 Agent 取走。
- Redis 跨实例扇出只在启用 Redis 时生效；单实例未启用 Redis 时仍走进程内扇出。
- 监听器目前统一绑定事件循环 0；`edgepulse.threads` 只创建额外循环，不会分担监听。多 worker 监听分发与多进程编排未作为本次交付形态。
- mTLS 指纹白名单在 Agent 注册时按自报值校验；逐请求身份绑定需要引擎开放对端证书接口或每节点独立 CA。
- WebSocket 首条认证的 5 秒兜底仍用计时线程（实现标注了 `shortcut`），等引擎提供带超时接收后替换。
- OTLP 出口已用 mock collector 与真实 OTel collector 容器验证；外部生产采集端的网络与鉴权仍需部署环境复核。
- Docker 镜像、Compose 全栈与 systemd 服务已在本机 Docker 中真机启动验证；systemd 验证使用特权容器，未覆盖真实主机的内核与 cgroup 差异。

## 编码与提交

- C++23；类/文件 PascalCase，函数/成员 camelCase，成员变量 `m_` 前缀；中文 Doxygen 注释。
- 代码格式以仓库根 `.clang-format`、`.clang-tidy` 为准（与引擎同一份）。
- 提交信息遵循约定式提交，首行 ≤ 72 列，正文写动机；按模块分批提交。
- 引擎目录只读；发现引擎缺陷时记录复现步骤，在应用层绕过。

## 文档

- [架构](docs/架构.md)
- [API](docs/API.md)
- [部署](docs/部署.md)
- [验收](docs/验收.md)
- [覆盖率](docs/覆盖率.md)
- [全量设计规范](docs/superpowers/specs/2026-10-20-edgepulse-full-design.md)
- [全量实施计划](docs/superpowers/plans/2026-10-20-edgepulse-full.md)
