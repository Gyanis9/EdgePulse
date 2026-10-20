# EdgePulse 全量实施计划（P0–P3）

- 日期：2026-10-20
- 对应设计：[全量设计规范](../specs/2026-10-20-edgepulse-full-design.md)
- 状态：已完成并通过全量验收

## 1. 约束

- 引擎固定 AsynGyanis 重构后的 1.0.0 提交 `5a4e9769`，Conan 包 `asyngyanis/1.0.0`，不改引擎源码。
- 源码分 `Base / Core / Plugins / App / Main`，依赖方向只向下。
- P0 验收后才进入 P1；每阶段先补测试再扩接口。
- 秘密只从环境变量或未跟踪文件读取；MySQL/Redis 真机用例环境变量门控。
- 提交遵循约定式提交，按模块拆分，提交前构建与相关用例通过。

## 2. 阶段与任务

### 阶段 A：引擎升级与源码重组（完成）

1. 子模块切到重构后的 1.0.0 提交，本地 Conan 包重建并打开 MySQL。
2. `src/Common/Auth/System/Device/Web` 迁入 `Base/Core/Plugins`，`Main.cpp` 独立。
3. CMake 目标改为 `EdgePulse::Base/Core/Plugins/App`，P0 用例回归。
4. 验证：构建通过、P0 35 条用例全绿、验收脚本 17 项通过。

### 阶段 B：P1 基础设施与系统管理（完成）

1. 增加 `Base/Json`、`Base/Api/CsvWriter`、`QueryParameters`、`SqlHelpers`、`SqlExecution`。
2. 增加 `Core/Cache`：内存与 Redis 两种异步 KV；会话、验证码、登录失败计数改走 KV。
3. 增加 V4 迁移：部门、岗位、字典、参数、操作日志、角色观测列、节点分组/归属部门、完整菜单与按钮权限。
4. 增加用户/角色/菜单/组织/字典/参数/日志仓储与系统、监控、工作台插件。
5. 增加权限校验助手、数据范围解析、操作审计中间件。
6. 前端接通现有 scaffold 用户/角色/菜单/部门/岗位/字典/参数/日志/工作台页面。
7. 验证：后端 46 条用例、前端单测/构建、真实 HTTP 冒烟。

### 阶段 C：P2 设备域（完成）

1. V5/V6 迁移：节点分组、遥测、告警规则/告警、固件/任务、命令。
2. 节点分组 CRUD 与节点 `groupId`；节点 JSON 回传分组名。
3. Agent 遥测批量上报、最新值、时间序列、告警评估与冷却去重。
4. 浏览器 WebSocket 订阅 `nodes/alarms/commands`；告警 SSE `ready/alarm`；Agent WebSocket 通道。
5. 告警规则与告警生命周期 API；确认/恢复广播。
6. 固件流式上传 + SHA-256、Range/ETag 下载、下发任务与状态回传。
7. 命令创建/轮询/结果/取消；在线直推与离线轮询。
8. 前端节点分组筛选、告警中心/遥测/固件/命令页。
9. 验证：54 条后端用例、23 条前端单测、3 条 Playwright、100 MiB 固件与 SSE 验收。

### 阶段 D：P3 可观测与部署（完成）

1. 配置加 MySQL/Redis/线程数/固件大小/遥测保留/热重载。
2. 数据库健康探针、`/readyz`、停机前 503、按监听器排空后停循环。
3. `edgepulse_build_info` 与 Prometheus 抓取；OTLP 配置与出口构建。
4. 配置目录绝对化并启用热重载，更新会话/验证码参数。
5. 生产静态站点：静态文件 + SPA 深链回退，API 未命中保持 JSON 404。
6. Docker Compose、systemd、Prometheus 配置与构建前端脚本。
7. 验证：readyz/metrics、热重载日志、SPA 深链、全量验收脚本。

## 3. 验收判据

- 后端 54/54；前端 23/23；Playwright 3/3；全量验收脚本全 PASS。
- 100 MiB 固件 SHA-256 一致，Range 206、ETag 304。
- 告警 2 秒内可见，SSE 3 秒内收到事件。
- 无客户端证书访问 9443 被拒；H3Probe 打印真实 h3。
- `/metrics` 无令牌 401、带令牌 200；`/readyz` 200；SPA 深链 200。
- 仓库零明文凭据，`git status` 只包含预期改动。

## 4. 执行偏差

- 告警 SSE 初版没有 ready 事件，浏览器在事件到来前拿不到响应头；验收驱动下补了 `event: ready`。
- 系统/监控/工作台依赖结构原先按栈引用捕获，导致 `/api/system/users` 悬空崩溃；改为共享所有权对象按值捕获。
- 引擎静态文件服务的 `any("*")` 会覆盖统一 404 与 SPA 回退，改为应用层单条站点兜底路由。
- 遥测最新值按最大采集时间在秒级会返回重复行，改为按最大 ID 取每指标最新一条。
- 引擎 1.0.0 尚无新 tag；子模块以提交号作为可复现真源，已写进 README 与架构。
- 监听器仍固定循环 0，`threads` 只创建额外循环；多 worker 监听分发未在本次冒险实现，已写入设计边界。
