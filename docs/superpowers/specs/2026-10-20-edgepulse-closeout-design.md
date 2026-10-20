# EdgePulse 收口设计（P1–P3 边界）

- 日期：2026-10-20
- 状态：用户已授权自行裁决，直接进入实现
- 上游设计：[全量设计](2026-10-20-edgepulse-full-design.md)

## 1. 目标

关闭全量交付后仍明确列出的四处 P1–P3 边界，不扩大 P4 范围：

1. 数据范围从用户列表扩展到节点与审计日志。
2. Agent WebSocket 纳入端到端验收，而不只覆盖浏览器 WebSocket。
3. 用真实 MySQL 与 Redis 跑通迁移、仓储与会话/验证码后端，不再只验证 SQLite/内存档。
4. 用本地 OTLP 接收端验证指标出口确实送达；Docker/Compose 只做配置校验，不伪称镜像已构建。

P4（边缘反代、UDP 遥测、OIDC、多租户）仍按原设计另行设计，本次不实现空壳。

## 2. 数据范围

- 节点：新增 `NodeListScope`，按 `owner_dept_id` 与 `created_by` 过滤；浏览器创建节点时写入登录用户的部门；Agent 自注册节点归属部门为 0，非 ALL 角色不可见。
- 操作日志：V7 给 `sys_oper_log` 增加 `oper_id`、`dept_id`；审计中间件写入操作人与部门。
- 登录日志：V7 给 `sys_login_log` 增加 `user_id`、`dept_id`；登录成功与失败都按账号写入（失败账号查不到用户时写 0）。
- 监控查询：`LogRepository` 的各读取入口接受 `LogScope`；`ALL` 不加条件，`SELF` 按账号，部门范围按部门集合，分页与导出在同一 SQL 过滤下完成。
- 管理端种子角色 `ops_team` 继续使用 `DEPT`，用于验证低权限账号看不到其他部门节点与日志。

## 3. Agent WebSocket 验收

- `samples/DeviceAgent` 增加 `--ws` 模式：用 mTLS 连接 `/api/agent/ws`，发送 hello，接收 `command`，回传 `command.result`。
- `scripts/check-acceptance.py` 在命令轮询用例之外增加一条 WS 路径：创建命令 → Agent 进程通过 WS 收到 → 回传 → 命令状态变为 succeeded。
- 若 Agent 未连接，服务端保持 pending，HTTP 轮询仍是兜底；两条路径都必须可观测。

## 4. 真实 MySQL / Redis

- 新增环境变量门控用例：
  - `EDGEPULSE_TEST_MYSQL_{HOST,PORT,USER,PASSWORD,DATABASE}`；
  - `EDGEPULSE_TEST_REDIS_{HOST,PORT,PASSWORD,DATABASE}`。
- MySQL 用例在目标库应用 V1–V7 迁移，验证种子用户、用户 CRUD、角色菜单与日志表列；Redis 用例验证 KV 的 set/get/expire/increment/erase。
- 缺任一变量即 `GTEST_SKIP`，不伪造通过。
- 用本机 Docker 启动临时 MySQL 容器（端口 3306）与现成 Redis 容器完成一次真机跑绿，结束后删除自己启动的容器。

## 5. OTLP

- 增加一个本地 mock OTLP/HTTP 接收端（Python），配置 `metrics.enabled=true`、短导出周期，启动服务后确认收到 `/v1/metrics` POST。
- 不引入真实 collector 镜像依赖；验证的是出口建立、周期导出与 HTTP 送达，不宣称后端语义。

## 6. 多 worker

监听分发涉及 HTTP/HTTPS/mTLS/QUIC 四类监听器与 Agent 通道的循环归属，本轮不做冒险改造；`threads` 仍只创建额外循环，文档继续如实标注。节点/日志数据范围与真机存储优先。

## 7. 测试

- 单元：LogScope 条件拼接、NodeListScope 过滤。
- 集成：SQLite 数据范围节点/日志；MySQL 真机迁移与仓储；Redis 真机 KV。
- 协议：低权限账号节点/日志 403 或空集；Agent WS 命令往返。
- 验收脚本：Agent WS、数据范围、MySQL/Redis 门控项、OTLP mock。

## 8. 验收

- `ctest` 50+ 条全绿；真机用例在本机 Docker 跑绿后才报告通过。
- 数据范围：DEPT 账号列表只看到本部门节点，导出同样不越权。
- Agent WS：命令通过 WS 收到并回传，列表状态 succeeded。
- OTLP mock 收到至少一次指标 POST。
