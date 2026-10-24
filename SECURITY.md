# 安全策略

EdgePulse 同时处理浏览器会话、Agent 证书身份、一次性注册令牌与固件签名。安全问题请按下面的私密渠道报告，不要先公开。

## 支持的版本

| 版本 | 支持状态 |
|---|---|
| 1.x | 接收安全报告，修复进入最新补丁版本 |
| 1.0 之前 | 不再支持 |

安全修复默认只进入最新发布线。部署时请记录实际运行的版本或提交号，报告里一并给出。

## 报告漏洞

优先使用仓库托管平台的私密安全报告功能。若代码托管在 GitHub，从仓库的 **Security → Advisories → Report a vulnerability** 提交。

如果平台不提供私密入口，先在仓库开一个不含漏洞细节的 Issue，请求维护者提供私密渠道；不要在公开 Issue、讨论区、Pull Request 或聊天记录里贴 PoC、利用步骤、受影响地址、令牌、证书或日志原文。

报告尽量包含：

- 受影响版本、提交号与部署形态（Docker Compose、systemd、本地开发或其他）。
- 受影响组件与前置条件（是否需要有效会话、Agent 证书、特定租户或配置）。
- 最小复现步骤或 PoC；能稳定复现的输入比截图更有用。
- 实际影响与攻击者能获得的能力。
- 相关日志或抓包，先删除 `Authorization`、Cookie、会话令牌、注册令牌明文、私钥、证书口令、数据库口令与个人信息。
- 是否已向其他人披露、是否计划公开。

维护者会确认收到、评估影响并在修复可用后与报告者协调披露时间。未修复前请不要公开漏洞细节。

## 安全模型

- 浏览器会话：登录成功后签发 32 字节不透明 Bearer token，服务端只保存会话记录；默认 TTL 7200 秒并滑动续期。浏览器不使用 Cookie 承载会话，跨站请求不能自动携带凭据。会话存储可落在 Redis，供多实例共享。
- API Token：明文以 `ep_api_` 开头，由 32 字节随机数编码；响应只在创建或轮换时返回一次，数据库只存 SHA-256 与前 12 个字符。令牌身份继承 owner 的用户与租户，但 `platformAdmin` 恒为 false；每次请求按 owner 当前权限与令牌 scope 的交集授权，令牌吊销或过期、owner 或租户停用都会立即失效。`/api/auth/me` 保留给令牌自省；令牌身份不能访问 `/api/auth/routes`、`/api/auth/logout` 等浏览器会话端点、浏览器 WebSocket 与改密入口。令牌列表按租户返回；创建与轮换只接受浏览器会话，令牌会话即使带对应 scope 也返回 403；吊销对令牌会话仍仅限本人且 scope 不宽于自身的令牌，浏览器会话按租户管理。轮换保留一个 1 小时旧哈希重叠窗口，吊销只写 `revoked_at`，不物理删除。
- 两步验证：TOTP 按 RFC 6238 校验，秘密用 `EDGEPULSE_MFA_KEY`（base64 32 字节）做 AES-256-GCM 加密后落库，nonce 随机，密钥缺失或认证失败只返回空值。恢复码是 10 枚 `xxxx-xxxx`，以同一主密钥做 HMAC-SHA256 后存 JSON，逐枚 CAS 删除，一枚只能用一次。登录挑战在缓存中原子消费，300 秒过期、单个挑战最多 5 次失败、绑定来源 IP；登录挑战失败、设置口令复核失败、启用、关闭与换发恢复码的错误动态码共用每用户 5 次/300 秒的失败计数，到达上限后不再签发挑战或接受口令复核，第 6 次起返回 429。设置口令正确但未完成第二因素不会清零，完成挑战校验或成功的管理操作才清零。密钥缺失时设置、启用、关闭与换发恢复码返回 503。管理员重置需要 `system:user:edit` 并按会话租户校验目标。MFA 是用户自助启用，没有租户级强制策略。
- 写请求幂等：只对带有效 Bearer 与 `Idempotency-Key` 的写请求生效，按令牌哈希与键哈希分区，请求指纹覆盖方法、完整 URI、表示头、权限摘要与正文。完成态保留 24 小时；同键不同内容或权限降级返回 409，处理中返回 409，已吊销会话不能重放旧响应。401、403、5xx 与超过 256 KiB 的响应不缓存；认证、Agent、令牌、注册令牌、用户/租户写入与流式上传路径整段排除，避免一次性明文或口令进入幂等表。处理中占位有 90 秒租约，崩溃接管按至少一次语义执行。
- OpenAPI 契约：`GET /api/openapi.json` 匿名返回生成物，内容来自源码路由字面量与 `docs/openapi-notes.json`，CI 用 `--check` 阻止漂移。契约不含凭据；服务启动时解析并校验它含字符串 `openapi` 与对象 `paths`，缺失、语法不完整或骨架不符时返回带 `x-openapi-error: missing` 的 JSON 404，`message` 为「OpenAPI 契约文件缺失或不可读」，不会回退到 HTML 或 SPA。
- Agent 身份：Agent 只通过 9443 的 mTLS 通道通信。TLS 层要求客户端证书由配置的 CA 签发；注册时自报的 SHA-256 叶证书指纹必须与这条连接出示的叶证书一致；心跳、遥测、命令结果、固件状态与固件下载都复核同一份叶证书，防止已注册节点被其他证书冒充。证书被吊销后指纹清空、节点置离线，已建立的 Agent WebSocket 以 1008 关闭。
- 生产 Agent 守护进程：`agent/` 随仓库交付，Linux 与 Windows 都由安装器注册为系统服务。命令执行不经过 shell，只接受 `config.apply`、`diagnostics.collect`、`firmware.update`、`agent.restart`、`agent.update` 等白名单类型，未知类型按失败上报；配置应用与固件安装钩子只运行配置里显式给出的程序，统一限时并限制输出。`agent.restart` 的结果先入队并尝试上报，之后才通过 systemd 或 SCM 重启服务；重启启动器失败只记 ERROR，不谎报成功。固件与升级包都用 Ed25519 对 SHA-256 摘要验签（`ed25519-sha256`）。升级包的签名对象是归档本身的 SHA-256：Agent 按命令摘要验签通过后才下载，并要求下载文件的摘要一致；归档内的 manifest 版本、平台、文件哈希与安装器脚本因此处于已认证范围，安装器继续校验它们。命令里的 `manifest` 字段不参与信任判断。升级后新版本在首次成功心跳写出 `update-health`；安装器在启动服务前写 `<状态目录>/update-expected`，手工升级没有待上报命令时也靠它确认健康。超时或启动失败时安装器恢复旧二进制并写 `update-rollback`，旧版本补报失败；版本不符时 `update-expected` 保留给安装器回滚。
- Agent 凭据与服务权限：注册与改绑令牌只从环境变量读取，Linux 写在 0600 的 `agent.env`，Windows 写在服务注册表项的 `Environment` 值；令牌不写 YAML、日志或状态库，注册成功后清空。Linux 以 systemd 单元运行，配置文件 0600、配置/状态/日志目录 0750，单元启用 `NoNewPrivileges`、`ProtectSystem=strict`、`PrivateTmp` 并只放开必要目录；Windows 以 SCM 服务运行，默认 LocalSystem，数据目录 ACL 只授予 Administrators 与 SYSTEM，安装目录给 Users 读取/执行，并配置失败自动重启。
- 注册令牌：`enroll` 用于新节点，`rebind` 用于已有节点换证；令牌一次性消费，数据库只存 SHA-256，明文只在签发响应出现一次。有效期限制在 300 到 2592000 秒。生产使用 `enrollment_mode=required`，新注册与换证都必须持对应令牌。
- 固件签名：固件签名覆盖文件 SHA-256 的 32 字节原始摘要，算法为 Ed25519，传输标识 `ed25519-sha256`。服务端只从 `trusted_keys_dir` 加载 Ed25519 公钥；`require_signature=true` 时缺签名、未知 key id、摘要不符或签名格式错误的固件不能上传、部署或回滚。公钥变更后重启加载，`GET /api/firmware/policy` 可确认已加载的 key id。
- 运维口：`/metrics`、`/debug/loops`、`/debug/connections` 与 `/debug/log-level` 需要 Bearer；`/healthz` 与 `/readyz` 只暴露进程与就绪状态。代码默认监听回环；生产样例与 Compose 为容器抓取绑 `0.0.0.0`，必须用主机防火墙或容器网络策略限制来源。
- 出站 SSRF：租户填写的 Webhook 地址在创建/修改与每次投递前都要通过地址检查。默认拒绝回环、RFC1918 私网、CGNAT、组播、保留与文档示例段、IPv4/IPv6 链路本地段以及云元数据地址和主机名。开启 `allow_private_webhooks` 会跳过整个私网/保留段分类，只保留云元数据主机名与地址、链路本地元数据段（169.254.0.0/16、fe80::/10 及已知 IPv6 元数据端点）的拒绝；CGNAT、0.0.0.0/8、文档与基准测试段、组播、240/4、IPv6 ULA 等其余私网与保留地址都会放行，只应在受信的自托管网关开启，不能当作通用的 SSRF 豁免。该防护只覆盖 Webhook 的 HTTP(S) 出站；SMTP 邮件渠道的 `smtp://`、`smtps://` 与 `starttls://` 端点不经过这道 HTTP 地址检查，必须由运维选择可信服务器并自行校验。
- 登录加固：建号、重置与改密共用口令策略；窗口内连续失败达到阈值后锁定账号；登录入口有按 IP 的频率限制，公开 HTTP/HTTPS/h3 可启用共享令牌桶。审计日志对 `password`、`token`、`captcha` 等字段脱敏。

## 已知边界

- DNS 重绑定窗口：Webhook 地址校验与实际连接之间存在时间差，引擎客户端会再次解析域名，可能拿到与校验时不同的地址。高安全环境不能只依赖 `allow_private_webhooks`，应在主机、容器或出口网关配置出站防火墙与域名白名单。
- 两步验证的边界：MFA 是每用户自助启用，没有租户级强制开关，管理员只能查看 `mfaEnabled` 并逐个重置。`EDGEPULSE_MFA_KEY` 丢失或更换后，已启用账号的 TOTP 与恢复码都不可用；本阶段没有密钥轮换、`key_version` 迁移或 AAD 绑定。能直接写数据库的攻击者可以替换加密秘密、回退 `last_used_step` 或替换恢复码哈希，这类数据库写权限失守不在本阶段防护范围内。多实例部署必须启用共享 Redis：登录挑战、待启用秘密与尝试计数都在缓存中，否则设置与挑战只落在签发实例。
- 反向代理与来源 IP：服务端没有可信代理头处理，`request.remoteIp()` 取直接对端地址，不信任 `X-Forwarded-For`。在负载均衡或反向代理之后，MFA 挑战绑定、登录限频与审计看到的是代理地址，同一代理后的用户会共享这些计数；需要真实来源时应在受信代理层解析并在应用支持可信代理之前谨慎启用，不能把客户端自报的头当作身份依据。
- 幂等键的边界：身份在中间件与处理器各校验一次，吊销后的重试不能重放，但处理中途崩溃后由新请求接管时，业务可能执行第二次（至少一次语义）。幂等表会保存最多 256 KiB 的响应正文与 24 小时历史，不要把敏感正文写进可缓存的写接口响应。API 令牌不能创建或轮换令牌（包括子令牌），因此不存在父令牌轮换或吊销后的级联问题；令牌也还没有独立的速率限制，仍受 owner 权限与全局限流约束。
- 生产 Agent 的边界：控制面不提供 CA 或 CSR 服务，节点证书由外部 CA 或 `scripts/gen-agent-certs.sh` 签发；证书轮换依靠 `rebind` 令牌重新注册，不是自动轮转。调度、离线扫描与保留期清理仍是单实例进程内定时器。Agent 安装包由外部托管，控制面只在 `agent.update` 命令里给出下载地址。Windows 升级模式固定服务名 `EdgePulseAgent`，自定义 SCM 服务名不支持自动升级。
- 手工 Agent 升级：`install-agent.sh --upgrade` 与 `install-agent.ps1 -Upgrade` 只校验包内 `checksums.sha256`，不验证归档的 Ed25519 签名。自动路径的归档认证由 Agent 在下载前后完成；手工升级必须由操作者确认归档来源。
- 单实例调度：离线扫描、生命周期处理与保留期清理依赖进程内定时器，没有 HA 选主或分布式租约；计划命令的到点判断在查询侧，也没有跨实例协调。多实例部署时不要假设只有一个实例在扫描，需要明确的活动实例或外部协调。
- h3 空闲超时：HTTP/3 的空闲计时由 QUIC 传输层决定，取 `server.quic.idle_timeout_seconds`（默认 30 秒，允许 1 到 3600，启动时生效）。`server.limits.idle_timeout_ms` 不作用于 h3 连接，调大它不会延长 h3 连接。
- 备份与恢复：恢复必须停止服务，运行中的进程会持有数据库、WAL 或固件文件句柄；SQLite 备份支持在线快照，但一致性归档仍建议放在维护窗口。证书、运维令牌与固件签名私钥不要和数据库备份放在同一份未加密介质里。
- Agent 身份绑定只存在于 mTLS 通道：开发与测试的明文链路不做叶证书绑定，不要把 Agent 路由暴露到明文监听器。
- 开启固件签名门禁后，历史已入库的未签名固件仍可被 Agent 下载。需要继续下发时，先重新上传带签名的同版本固件，再按新记录部署。

## 密钥与证书管理

- 不提交私钥、证书、令牌、`.env`、日志或任何明文凭据。`.gitignore` 只覆盖开发证书所在的 `var/`（含 `var/certs/`）、`config/prod.yaml`、`config/firmware-keys/` 与部署令牌；生产 `cert_dir`（如 `/etc/edgepulse/tls`）在仓库之外，不受仓库忽略规则保护，必须由主机文件权限与备份策略单独保护。`var/`、`config/prod.yaml` 与 `deploy/prometheus/ops-token` 同时在 `.dockerignore` 中。
- 数据库、Redis、运维口、UDP 遥测、OIDC、两步验证与初始口令提示只从环境变量读取：`EDGEPULSE_DB_PASSWORD`、`EDGEPULSE_REDIS_PASSWORD`、`EDGEPULSE_OPS_BEARER_TOKEN`、`EDGEPULSE_UDP_TELEMETRY_SECRET`、`EDGEPULSE_OIDC_CLIENT_SECRET`、`EDGEPULSE_MFA_KEY`、`EDGEPULSE_INITIAL_PASSWORD`。`EDGEPULSE_MFA_KEY` 用 `openssl rand -base64 32` 生成，同一数据库的所有实例共用同一值，与数据库备份分开保管。
- 固件签名私钥留在签名工作站或密钥管理系统，只把公钥分发到各实例的 `trusted_keys_dir`；不要复用历史 key id 覆盖公钥，轮换时使用新文件名并在确认旧固件不再需要后移除旧公钥。
- 服务器证书与私钥放在 `cert_dir` 指向的目录，Agent 客户端证书的信任锚是同一目录的 `ca.pem`。证书文件权限只给服务账号读取。

### 证书轮换

1. 用同一 CA 或受信 CA 为 Agent 签发新证书；不要在旧证书过期后才开始。
2. 管理员用 `POST /api/enrollment-tokens` 签发 `rebind` 令牌，`nodeName` 限定目标节点；令牌只交付给该节点。
3. Agent 用新证书连接 9443，并在 `/api/agent/register` 带上 `enrollmentToken`；注册成功会更新指纹与 `certNotAfter`。
4. 用 `GET /api/nodes/:id` 核对 `certFingerprint`、`certNotAfter` 与 `certRevokedAt`，确认新证书已生效。
5. 怀疑私钥泄露时，先调用 `POST /api/nodes/:id/certificate/revoke` 清空指纹并断开该节点的 Agent 连接，再按新节点或 `rebind` 流程重新准入；不要继续使用旧证书。
6. CA 轮换时先把旧 CA 与新 CA 一起放入 `ca.pem`（或部署支持的信任 bundle），确认新 CA 签发的证书能通过校验，再逐节点换证；全部切换完成后再移除旧 CA。若当前部署只能放单个 CA，先安排维护窗口，不要直接替换。
