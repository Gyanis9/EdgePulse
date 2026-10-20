# EdgePulse 收口实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** 关闭 P1–P3 的数据范围、Agent WS、真实 MySQL/Redis 与 OTLP 边界。

**Architecture:** 在现有 Base/Core/Plugins/App 分层上增量修改：V7 迁移补日志归属列，仓储接收 scope，控制器解析会话范围，Agent 示例增加 WS 模式，测试增加环境变量门控的真机与 mock 出口。

**Tech Stack:** C++23、AsynGyanis 1.0.0、GoogleTest、MySQL 8、Redis 7、Python mock collector、Playwright。

**Spec:** `docs/superpowers/specs/2026-10-20-edgepulse-closeout-design.md`

## Global Constraints

- 不修改 `third_party/AsynGyanis`；不改引擎源码。
- 秘密只从环境变量或未跟踪文件读取。
- 真机用例缺依赖即 SKIP，不伪造通过。
- C++ 注释中文、无 Markdown 记号；提交遵循约定式提交。
- P4 不在本轮实现。

---

### Task 1: V7 日志归属与 scope

**Files:**
- Create: `migrations/V7__log_scope.sql`
- Modify: `src/Core/System/SystemModels.h`、`src/Plugins/SystemPlugin/SystemController.cpp`、`src/Core/System/LogRepository.*`
- Test: `tests/Integration/TestSystemServices.cpp`

- [x] 写迁移：`sys_oper_log` 加 `oper_id/dept_id`，`sys_login_log` 加 `user_id/dept_id`。
- [x] 登录审计写入 user/dept；操作审计写入 session 的 user/dept。
- [x] 定义 `LogScope`，在操作/登录日志列表、导出、统计、时间线、热力中应用。
- [x] 列出低权限账号只能看到本部门日志的集成断言。
- [x] 构建、跑相关测试、提交。

### Task 2: 节点数据范围

**Files:**
- Modify: `src/Core/Device/NodeRepository.*`、`NodeService.*`、`src/Plugins/DevicePlugin/DeviceController.*`、`src/App/Application.cpp`
- Test: `tests/Integration/TestDeviceService.cpp`、`tests/Protocol/TestNodeEndpoints.cpp`

- [x] `NodeListScope` 加 `owner_dept_id`/`created_by` 过滤。
- [x] 浏览器创建节点写入会话部门；Agent 注册仍为 0。
- [x] 控制器解析 `DataScopeService`，列表按 scope 查询。
- [x] 测试 ALL/DEPT/SELF 三档与导出。
- [x] 构建、跑测试、提交。

### Task 3: Agent WS 示例与验收

**Files:**
- Modify: `samples/DeviceAgent/Main.cpp`、`scripts/check-acceptance.py`
- Test: `tests/Protocol/TestFleetEndpoints.cpp`

- [x] 示例增加 `--ws`：mTLS WebSocket 连接、hello、收 command、回 result。
- [x] 协议测试覆盖 hello/command/result 通道。
- [x] 验收脚本创建命令后启动 WS Agent，断言状态 succeeded。
- [x] 构建、跑验收、提交。

### Task 4: 真机 MySQL/Redis 用例

**Files:**
- Create: `tests/Integration/TestExternalStores.cpp`
- Modify: `tests/CMakeLists.txt`

- [x] 环境变量门控 MySQL/Redis。
- [x] MySQL 应用迁移并验证种子/用户/日志列。
- [x] Redis 验证 KV 五类操作与过期。
- [x] 启动临时 MySQL 容器、复用现有 Redis，跑绿后删容器。
- [x] 提交。

### Task 5: OTLP mock 验收

**Files:**
- Create: `scripts/check-otlp.py`
- Modify: `src/App/Application.cpp`（若出口启动配置有缺口）

- [x] Mock HTTP 接收端记录 `/v1/metrics` POST。
- [x] 临时配置开启导出、短周期，启动服务，断言收到请求。
- [x] 关闭服务与 mock，提交。

### Task 6: 文档与最终验收

- [x] 更新架构/API/验收/覆盖率与 README 的边界描述。
- [x] 全量 `ctest`、前端单测/构建、Playwright、验收脚本。
- [x] 按模块提交，工作树干净。
