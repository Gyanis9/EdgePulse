# EdgePulse

EdgePulse 是面向边缘节点与设备的运维平台：控制面是浏览器管理后台，数据面是运行在边缘节点上的 `edgepulse-agent`。后端基于 C++23 与 AsynGyanis 4.0.0 引擎，前端基于 Vue 3 + Vite。

当前状态：P0 纵向切片已通过验收（2026-10-20）。P0 覆盖认证、节点管理、节点状态 WebSocket、mTLS Agent 注册/心跳、真实 h3 连接与 Prometheus 指标；系统管理域、设备/遥测/告警/固件/命令与 SSE 按阶段推进。

## 目录

- `src/`：后端模块（App、Common、Web、Auth、System、Device）
- `frontend/`：Vue 3 管理后台
- `third_party/AsynGyanis/`：引擎 git submodule，固定 `v4.0.0`，只读
- `migrations/`：SQL 迁移
- `config/`：非敏感配置样例
- `scripts/`：证书、开发运行与验收脚本
- `docs/`：架构、API、部署、验收与覆盖率

## 前置条件

- Windows + MSVC ≥ 19.40 或 Linux + GCC ≥ 13 / Clang ≥ 17
- CMake ≥ 3.25、Ninja、Conan 2、Python 3、Node.js ≥ 20
- Git（含 submodule）、OpenSSL（生成开发证书）

本机已验证的工具链：`G:\Tools\CMake`、`G:\Tools\ninja`、`G:\Tools\Python\Python314`、`G:\Tools\nodejs`、`G:\Tools\Microsoft Visual Studio\18\Community`。

## 构建

引擎只作为 Conan 包消费，不复制源码、不 `add_subdirectory`：

```bash
git submodule update --init --recursive

conan create third_party/AsynGyanis/packaging/conan --build=missing \
  -s compiler.cppstd=23 -s build_type=Release -o "&:with_mysql=True"

conan install . --build=missing -s compiler.cppstd=23 -s build_type=Release
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

`debug` 预设使用 RelWithDebInfo，并强制 `/MD` 运行期库：引擎包只有 Release 档，混用 `/MDd` 会在链接期报 LNK2038。需要调试版 CRT 时另行构建一整套依赖包，不在本工程内维护第二套构建。

构建目录 `build/`、运行目录 `var/` 与前端 `dist/`、`node_modules/` 都不进版本库。

## 运行

```bash
bash scripts/dev-certs.sh          # 生成 CA、服务器、Agent 与对照证书（var/certs，未跟踪）
bash scripts/run-dev.sh            # 生成/读取运维令牌并启动 edgepulse-server
```

默认监听：

| 端口 | 用途 |
|---|---|
| 8080/TCP | 开发档 HTTP + WebSocket，Vite 代理目标 |
| 8443/TCP | HTTPS + h2 控制台，Alt-Svc 通告 h3 |
| 8443/UDP | HTTP/3 |
| 9443/TCP | Agent mTLS（要求客户端证书） |
| 9100/TCP | 运维口，仅回环；`/metrics` 需 Bearer |

开发种子账号：`admin` / `admin123`（仅 `migrations/V2__seed.sql` 的开发数据，部署后必须改密）。

前端：

```bash
cd frontend
npm ci
npm run dev      # http://127.0.0.1:5173，/api 与 /api/ws 代理到 8080
```

## 验收

```bash
# 后端：35 条用例
ctest --preset debug --output-on-failure

# 前端：22 条单测 + 构建 + Playwright
cd frontend && npm run test:unit && npm run build && npm run test:e2e

# 协议与安全：17 项（认证、节点、h3、mTLS、WebSocket、metrics）
python scripts/check-acceptance.py
```

验收脚本会按需启动开发服务，逐项打印 PASS/FAIL；结果见 [docs/验收.md](docs/验收.md)。

## 已知边界（P0）

- 开发档只使用 SQLite；MySQL/Redis 运行时与集成测试按 P1 推进。
- P0 是单进程；WebSocket 扇出在进程内。
- P0 不使用引擎的 `JwtVerifier`；会话是不透明随机 token，存于进程内，重启失效。
- 引擎 v4.0.0 包没有 `Core/Crypto/Random.h` 与 `EventLoop::currentEventLoop()`；应用层用 OpenSSL 随机源并显式传循环。
- mTLS 在 TLS 层用 CA 链拒绝无证书与陌生 CA；指纹白名单在 Agent 注册时按 Agent 自报值校验（见 [架构](docs/架构.md) 的边界说明）。
- 生产静态站点由后端托管的实现尚未启用；P0 的浏览器入口是 Vite 开发服务器。
- WebSocket 首条认证的 5 秒兜底目前用一条计时线程（实现里标了 `shortcut`），等引擎提供带超时的接收后替换。

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
- [设计规范](docs/superpowers/specs/2026-10-20-edgepulse-design.md)
- [P0 实施计划](docs/superpowers/plans/2026-10-20-edgepulse-p0.md)
