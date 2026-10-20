# EdgePulse P0 纵向切片实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付可运行、可验收的 EdgePulse P0 纵向切片：引擎包消费、SQLite 迁移、验证码登录、节点 CRUD、节点状态 WebSocket、mTLS Agent 注册心跳、真实 h3 探针、运维指标与可登录的 Vue 前端。

**Architecture:** 后端按 Common/Web/Auth/System/Device/Ops 分层，App 负责装配与生命周期；Auth 与 Device 暴露路由注册函数，由 App 挂到不同监听器。引擎通过 Conan 包消费，应用层补齐 SPA 回退、PBKDF2、会话、mTLS 指纹绑定与 h3 验证。前端 fork scaffold-vue，按 `sys_menu` 动态生成路由。

**Tech Stack:** C++23、CMake 3.25+、Ninja、Conan 2、AsynGyanis 4.0.0、GoogleTest、SQLite/MySQL、Vue 3 + Vite + TypeScript、Playwright（系统 Edge）。

**Spec:** `docs/superpowers/specs/2026-10-20-edgepulse-design.md`

## Global Constraints

- C++23；CMake ≥ 3.25；Ninja；Conan 2；MSVC ≥ 19.40 / GCC ≥ 13 / Clang ≥ 17；只支持 Linux 与 Windows。
- 引擎固定 `third_party/AsynGyanis` 的 `v4.0.0` tag，只读；不得复制源码进 `src/`；不得 `add_subdirectory` 引擎。
- 包消费：`conan create third_party/AsynGyanis/packaging/conan --build=missing -s compiler.cppstd=23 -s build_type=Release -o "&:with_mysql=True"`；应用 `find_package(AsynGyanis REQUIRED COMPONENTS Net Database)`。
- 单一构建形态：`debug` 预设 = RelWithDebInfo + `/MD`，消费 Release 引擎包；不建 Debug CRT 变体（见 spec R4）。
- 所有口令、token、私钥只从环境变量或未跟踪文件读取；仓库零明文。dev 运行时令牌写 `var/ops-token`，证书写 `var/certs/`，二者 gitignore。
- C++ 命名：类/文件 PascalCase，函数/成员 camelCase，成员变量 `m_` 前缀；中文 Doxygen 注释；注释里禁止 Markdown 排版记号、禁止引用计划编号。
- 统一响应 `{code,message,data,traceId}`；分页 `{pageNum,pageSize,total,rows}`，pageSize 上限 100；回写 `x-request-id`。
- 测试：GoogleTest；数据库集成测试用真实 SQLite 临时文件；MySQL/Redis 用例用环境变量门控并在缺凭据时 `GTEST_SKIP`；禁止 mock 数据库制造假绿。
- 前端 TypeScript strict；不引入新的 UI 框架或图表库；保留 scaffold 的 `scaffold.*` 存储键与 401 契约。
- 每次提交前：相关构建 rc=0 且相关用例通过；提交信息遵循 `git-commit-convention`，按模块分批。

---

### Task 1: 工程骨架、构建预设与健康检查

**Files:**
- Create: `.gitignore`, `.gitattributes`, `.clang-format`, `.clang-tidy`, `README.md`, `CONTRIBUTING.md`
- Create: `conanfile.py`, `CMakeLists.txt`, `CMakePresets.json`, `cmake/EdgePulseWarnings.cmake`
- Create: `src/CMakeLists.txt`, `src/Common/CMakeLists.txt`, `src/Web/CMakeLists.txt`, `src/App/CMakeLists.txt`
- Create: `src/Common/ApiResponse.h/.cpp`, `src/Common/ErrorCatalog.h/.cpp`, `src/Common/Pagination.h/.cpp`, `src/Common/TimeFormat.h/.cpp`
- Create: `src/Web/RequestId.h/.cpp`, `src/Web/MiddlewareSetup.h/.cpp`
- Create: `src/App/ApplicationConfiguration.h/.cpp`, `src/App/Application.h/.cpp`, `src/App/ListenerAssembly.h/.cpp`, `src/App/Main.cpp`
- Create: `config/dev.yaml`, `config/logging.yaml`, `config/prod.yaml.example`
- Create: `tests/CMakeLists.txt`, `tests/TestSupport/HttpClientHarness.h/.cpp`, `tests/Protocol/TestHealthServer.cpp`
- Test: `tests/Unit/TestApiResponse.cpp`, `tests/Unit/TestPagination.cpp`

**Interfaces:**
- Produces:
  - `EdgePulse::Common::ApiResponse::success(Net::HttpResponse&, Base::ConfigValue, std::string_view traceId)`
  - `EdgePulse::Common::ApiResponse::failure(Net::HttpResponse&, int httpStatus, int code, std::string_view message, std::string_view traceId)`
  - `EdgePulse::Common::PaginationParameters EdgePulse::Common::parsePagination(const std::unordered_map<std::string,std::string>&)`
  - `EdgePulse::App::AppConfig`（端口、数据库路径、证书路径、会话与验证码参数、Origin 白名单）
  - CMake 目标 `edgepulse-server`，输出 `build/debug/edgepulse-server.exe`
- Consumes: `AsynGyanis::Net`, `AsynGyanis::Base`, `AsynGyanis::Core`, `AsynGyanis::Database`。

- [ ] **Step 1: 初始化仓库文件与子模块记录**

`.gitignore` 至少包含：

```gitignore
/build/
/var/
/frontend/node_modules/
/frontend/.npm-cache/
/frontend/dist/
/third_party/AsynGyanis/
*.log
*.pdb
*.ilk
.env
```

保留 `third_party/AsynGyanis` 由 `.gitmodules` 跟踪。复制引擎的 `.clang-format` 与 `.clang-tidy`（不另裁一套），`.gitattributes` 设 `* text=auto eol=lf`，`*.bat text eol=crlf`。

- [ ] **Step 2: 写 Conan 配方与构建预设**

`conanfile.py`：

```python
from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain, cmake_layout


class EdgePulseRecipe(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    requires = (
        "asyngyanis/4.0.0",
        "openssl/3.6.5",
        "gtest/1.15.0",
    )
    default_options = {"gtest/*:shared": False}

    def layout(self):
        cmake_layout(self)

    def generate(self):
        CMakeDeps(self).generate()
        toolchain = CMakeToolchain(self)
        toolchain.user_presets_path = False
        toolchain.variables["EDGEPULSE_BUILD_TESTS"] = True
        toolchain.generate()
```

`CMakePresets.json` 的 `debug` 预设：

```json
{
  "name": "debug",
  "generator": "Ninja",
  "binaryDir": "${sourceDir}/build/debug",
  "cacheVariables": {
    "CMAKE_BUILD_TYPE": "RelWithDebInfo",
    "CMAKE_CXX_STANDARD": "23",
    "CMAKE_CXX_STANDARD_REQUIRED": "ON",
    "CMAKE_CXX_EXTENSIONS": "OFF",
    "CMAKE_TOOLCHAIN_FILE": "${sourceDir}/build/generators/conan_toolchain.cmake",
    "CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO": "Release;",
    "CMAKE_MAP_IMPORTED_CONFIG_DEBUG": "Release;",
    "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
    "EDGEPULSE_BUILD_TESTS": "ON"
  }
}
```

`release` 预设同形，`CMAKE_BUILD_TYPE=Release`。测试预设 `debug` / `release` 从仓库根调用。

- [ ] **Step 3: 写最小 Common 与健康检查服务**

`ApiResponse` 用 `Base::ConfigValue`（即 nlohmann::json）构造对象，`setJsonBody`，并设置 `x-request-id`。禁止用字符串拼接 JSON。

`Application` 的 P0 起点：持有 `Core::IoContext`、一个 `Net::HttpServer`（127.0.0.1:8080）、`GET /api/health` 与一个 404 兜底；`run()` 启动后阻塞，`SIGINT` 时停止。

- [ ] **Step 4: 写测试并先看红**

`TestHealthServer`：在 127.0.0.1:0 起 HttpServer，用 `Net::HttpClient::get` 请求 `/api/health`，断言 200、`code=200`、`status=UP`。`TestApiResponse` 断言成功/失败包络与 traceId。先提交测试前状态，运行确认因实现缺失而失败。

Run:

```bash
conan install . --build=missing -s compiler.cppstd=23 -s build_type=Release
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Expected: 首轮测试失败（端点未注册），实现后全部通过。

- [ ] **Step 5: 提交**

```bash
git add <显式路径>
git commit -m "chore: 初始化 EdgePulse 工程骨架与构建预设"
```

---

### Task 2: 数据库封装、迁移与 System 仓储

**Files:**
- Create: `migrations/V1__schema.sql`, `migrations/V2__seed.sql`, `migrations/V3__device_domain.sql`
- Create: `src/Common/Database.h/.cpp`, `src/Common/JsonFields.h/.cpp`
- Create: `src/System/MigrationRunner.h/.cpp`, `src/System/UserRepository.h/.cpp`, `src/System/MenuRepository.h/.cpp`
- Create: `src/System/CMakeLists.txt`
- Test: `tests/Unit/TestMigrationSplitter.cpp`, `tests/Integration/TestMigrations.cpp`, `tests/Integration/TestUserRepository.cpp`

**Interfaces:**
- Produces:
  - `EdgePulse::Common::Database::openSqlite(std::string path, std::size_t poolSize)`
  - `Core::Task<void> EdgePulse::Common::Database::run(Core::EventLoop&, std::function<void(Database::Connection&)>)`
  - `EdgePulse::System::MigrationRunner::apply(const Database&, std::string directory, std::string dialect)`（启动期同步执行）
  - `EdgePulse::System::UserRecord`, `UserRepository::findByUsername`, `findById`, `permissionsFor`, `updatePassword`, `touchLastLogin`
  - `EdgePulse::System::MenuNode`, `MenuRepository::treeForUser`
- Consumes: Task 1 的 Common 类型；引擎 `Database::ConnectionPool`。

- [ ] **Step 1: 写迁移 SQL（P0 子集）**

V1 建 `sys_user`、`sys_role`、`sys_menu`、`sys_user_role`、`sys_role_menu`、`sys_login_log`。V2 插入：
- 角色 `super_admin`（id=1，data_scope=ALL，builtin=1）
- 用户 `admin`（id=1，password 用下方哈希，status=1）
- 菜单 id=1000（目录「边缘运维」，path=`/device`）、id=1001（菜单「节点」，path=`/device/nodes`，component=`device/Nodes`，permission=`device:node:list`）、按钮 id=1002（`device:node:write`，parent=1001，type=btn）
- `sys_user_role(1,1)`、`sys_role_menu(1,1000/1001/1002)`

管理员种子密码为 `admin123`，PBKDF2 字符串（210000 次，固定盐，仅用于开发种子）：

```text
pbkdf2_sha256$210000$nUwvGntuPFCBorPE1eb3CA==$cxCcCezVVbPzfukzjfcpWoTZpVyQ3AR1H9Z2V1QWObk=
```

V3 建 `biz_node`（含 `version`、`deleted`、审计四列）与索引。全部 SQL 只用 MySQL 与 SQLite 都接受的语法：`BIGINT`、`VARCHAR`、`DATETIME`、`TINYINT`、`JSON`、`TEXT`；不用 `AUTO_INCREMENT`，ID 由应用生成。

- [ ] **Step 2: 实现迁移执行器**

要求：
- 按文件名 `V<数字>__` 升序；版本表 `schema_migrations(version BIGINT PRIMARY KEY, description, checksum, applied_at)`。
- 切分器跳过 `--` 行注释、单引号字符串内的分号；空语句忽略。
- 已应用版本的 SHA-256 与库中不一致时抛错并拒绝启动；新版本在单连接上顺序执行。
- 用临时 SQLite 文件测试：首次应用成功、二次应用跳过、篡改文件后报校验失败。

- [ ] **Step 3: 实现 Database 与仓储**

`Database::run` 在 `AsyncExecutor::shared()->submit(loop, ...)` 里取一条池化连接执行 lambda，异常转成 `std::expected`/抛回协程。仓储只写参数化 SQL；禁止拼接用户输入。`UserRepository::permissionsFor` 用 `sys_user_role`/`sys_role_menu`/`sys_menu` 三表连接，只取 `deleted=0` 且 `status=1` 的菜单权限。

- [ ] **Step 4: 测试**

集成测试用 `tmp_path` 生成 SQLite 文件，应用迁移后断言：admin 存在、密码哈希匹配（Task 3 未完成前只断言字符串）、菜单树包含 `/device/nodes`、权限集合含 `device:node:list` 与 `device:node:write`、重复应用不重复插入。

Run:

```bash
cmake --build --preset debug --target edgepulse-system-tests
ctest --preset debug -R "Migration|UserRepository" --output-on-failure
```

Expected: PASS。

- [ ] **Step 5: 提交**

```bash
git commit -m "feat(System): 增加可校验的 SQL 迁移与用户菜单仓储"
```

---

### Task 3: PBKDF2、验证码、会话与认证接口

**Files:**
- Create: `src/Common/PasswordHash.h/.cpp`, `src/Common/RandomToken.h/.cpp`
- Create: `src/Auth/CaptchaService.h/.cpp`, `SessionStore.h/.cpp`, `PermissionService.h/.cpp`, `AuthService.h/.cpp`, `LoginAudit.h/.cpp`, `AuthController.h/.cpp`
- Create: `src/Auth/CMakeLists.txt`
- Test: `tests/Unit/TestPasswordHash.cpp`, `tests/Unit/TestCaptchaService.cpp`, `tests/Unit/TestSessionStore.cpp`, `tests/Integration/TestAuthService.cpp`, `tests/Protocol/TestAuthEndpoints.cpp`

**Interfaces:**
- Produces:
  - `Common::hashPassword(std::string_view) -> std::string`；`Common::verifyPassword(std::string_view hash, std::string_view password) -> bool`
  - `Common::randomTokenHex(std::size_t bytes) -> std::string`
  - `Auth::CaptchaService::issue(remoteIp) -> CaptchaChallenge{uuid, code, svg}`；`consume(uuid, code) -> bool`
  - `Auth::SessionStore::create(userId) -> Session{token, expiresAt}`；`validateAndTouch(token) -> std::optional<Session>`；`remove(token)`
  - `Auth::AuthService::login(...) -> std::expected<LoginResult, AuthError>`；`currentUser(token)`；`routes(token)`；`changePassword(...)`
  - `Auth::registerRoutes(Net::Router&, Auth::AuthService&, Auth::CaptchaService&, Auth::SessionStore&)`
- Consumes: Task 2 的 `UserRepository`、`MenuRepository`、`LoginAudit` 表。

- [ ] **Step 1: 写测试先看红**

密码哈希测试覆盖：格式四段、210000 次、16 字节盐、32 字节哈希、正确口令 true、错口令 false、篡改哈希 false、空口令 false。会话测试覆盖：创建、滑动续期、过期、登出删除。验证码测试覆盖：一次性消费、2 分钟过期（注入时钟）、同 IP 每分钟第 31 次被拒。运行确认失败。

- [ ] **Step 2: 实现密码与随机 token**

`PasswordHash` 用 OpenSSL `PKCS5_PBKDF2_HMAC` 与 `RAND_bytes`；盐与哈希用 base64（无换行）；比较用 `CRYPTO_memcmp`。解析哈希时严格校验段数与迭代数，失败一律 false，不抛到 HTTP 层。

- [ ] **Step 3: 实现验证码与会话**

验证码：
- 字符集 `ABCDEFGHJKLMNPQRSTUVWXYZ23456789`（去易混淆字符），4 位。
- `uuid` 为 16 字节随机十六进制。
- 进程内 map，互斥保护；2 分钟 TTL；`consume` 原子取出并删除。
- SVG：4 个 `<text>`，随机旋转 -18°..18°，随机颜色，噪点线；输出 UTF-8 字符串。
- 按 IP 的 60 秒固定窗口计数，超过 30 返回 `429`。

会话：
- token = 32 字节随机十六进制；TTL 7200 秒；每次 `validateAndTouch` 续期到 now+7200。
- map 存储 userId、expiresAt、createdAt；过期项惰性清理；登出立即 erase。

- [ ] **Step 4: 实现 AuthService 与路由**

登录流程：校验验证码 → 查用户（deleted=0）→ 状态必须为 1 → PBKDF2 校验 → 生成 token → 更新 last_login_time → 写 `sys_login_log`（成功/失败都写，失败消息不含口令）→ 返回 `{token,expiresIn:7200}`。

`/api/auth/me` 返回 `id` 与 `userId` 两个字段（兼容 scaffold 前端与 Java 契约），roles 为 role_code 数组，permissions 为权限码数组。`/api/auth/routes` 只返回 `menu_type='menu'`、`visible=1`、`status=1` 的节点，按 sort 排序，构建树。`PUT /api/auth/password` 校验旧密码后更新哈希。

所有认证接口的 `traceId` 来自 `request.requestId()`，并回写 `x-request-id`。

- [ ] **Step 5: 协议测试**

启动 Task 1 的 8080 服务并挂 Auth 路由，用 `HttpClient`：
- `GET /api/auth/captcha` 200，dev 档含 `code` 与 `imageSvg`。
- 用返回的 uuid/code + `admin/admin123` 登录 200，拿到 token。
- `GET /api/auth/me` 带 Bearer 200，字段齐全；不带 Bearer 401。
- `GET /api/auth/routes` 含 `device/Nodes`。
- 错误验证码 400；错误口令 401。
- 登出后旧 token 401。

Run:

```bash
cmake --build --preset debug
ctest --preset debug -R "Password|Captcha|Session|Auth" --output-on-failure
```

Expected: PASS。

- [ ] **Step 6: 提交（两笔）**

```bash
git commit -m "feat(Common): 增加 PBKDF2 密码哈希与随机 token"
git commit -m "feat(Auth): 实现验证码登录、会话与权限路由"
```

---

### Task 4: 节点管理与 Agent 注册心跳

**Files:**
- Create: `src/Device/NodeRepository.h/.cpp`, `NodeService.h/.cpp`, `AgentRegistry.h/.cpp`, `AgentController.h/.cpp`
- Create: `src/Device/CMakeLists.txt`
- Modify: `src/Common/TimeFormat.h/.cpp`（增加 DATETIME 与 epoch 互转）
- Test: `tests/Integration/TestNodeRepository.cpp`, `tests/Unit/TestNodeService.cpp`, `tests/Integration/TestAgentRegistry.cpp`

**Interfaces:**
- Produces:
  - `Device::NodeRecord {std::int64_t id; std::string name; std::string status; std::string agentVersion; std::string certFingerprint; std::string labelsJson; std::string lastSeen; std::uint64_t version; bool deleted;}`
  - `Device::NodeService::list(loop, PageRequest, keyword, status) -> PageResult<NodeRecord>`；`create`；`update`；`remove`
  - `Device::AgentRegistry::registerAgent(AgentRegistration) -> std::expected<NodeRecord, AgentError>`；`heartbeat(nodeId, version) -> std::expected<NodeRecord, AgentError>`
  - `using NodeStatusPublisher = std::function<void(const NodeRecord&)>`
  - `Device::registerRoutes(Net::Router&, NodeService&)`
  - `Device::registerAgentRoutes(Net::Router&, AgentRegistry&)`
- Consumes: Task 2 的 `Database`、Task 3 的 `PermissionService`（接口以 `std::function<bool(std::string_view)>` 注入，避免 Device 依赖 Auth）。

- [ ] **Step 1: 写测试先看红**

节点测试：创建、按关键字/状态分页、更新成功、旧 version 更新返回 `Conflict`、删除后列表不含、按名称查重。Agent 测试：白名单内指纹注册成功；白名单外指纹注册返回 `FingerprintRejected`；心跳更新 last_seen/status 并触发 publisher；未知 nodeId 返回 `NotFound`。

- [ ] **Step 2: 实现仓储与服务**

ID 生成：`(epochMillis << 20) | (counter++ & 0xFFFFF)`，进程内原子计数。所有 SQL 参数化；列表查询 `deleted=0` 且 `ORDER BY id DESC`；分页 `LIMIT ? OFFSET ?`。更新节点用：

```sql
UPDATE biz_node SET name=?, labels=?, version=version+1, updated_at=?, updated_by=?
WHERE id=? AND version=? AND deleted=0
```

影响行数为 0 时再查一次：不存在返回 NotFound，存在返回 Conflict。软删除同样带 `deleted=0` 条件。

- [ ] **Step 3: 实现 Agent 注册表**

注册请求体：`{name, agentVersion, certFingerprint, labels?}`。白名单来自 `AgentRegistry::Options::allowedFingerprints`（64 位小写十六进制集合）。指纹必须命中白名单；节点按 name 查重，存在则更新 agentVersion/certFingerprint/status='online'/last_seen/version+1，不存在则创建。注册与心跳都调用 publisher。

- [ ] **Step 4: 路由与权限**

节点路由：
- `GET /api/nodes` 需要 `device:node:list`；
- `POST/PUT/DELETE /api/nodes` 需要 `device:node:write`。
权限由调用方注入的 `PermissionCheck` 完成，返回 403 时响应 `{code:403}`。

Agent 路由：
- `POST /api/agent/register`、`POST /api/agent/heartbeat`，不校验浏览器 token（mTLS 监听器隔离），但校验指纹白名单。

- [ ] **Step 5: 测试与提交**

Run:

```bash
ctest --preset debug -R "Node|Agent" --output-on-failure
git commit -m "feat(Device): 节点 CRUD、乐观锁与 Agent 注册心跳"
```

---

### Task 5: Web 安全头、静态 SPA 与 WebSocket

**Files:**
- Create: `src/Web/StaticSite.h/.cpp`, `WebSocketEndpoints.h/.cpp`, `WebSocketHub.h/.cpp`, `ProtocolStats.h/.cpp`
- Create/Modify: `src/Web/MiddlewareSetup.h/.cpp`, `src/Web/CMakeLists.txt`
- Test: `tests/Unit/TestProtocolStats.cpp`, `tests/Protocol/TestSecurityHeaders.cpp`, `tests/Protocol/TestStaticSite.cpp`, `tests/Protocol/TestBrowserWebSocket.cpp`

**Interfaces:**
- Produces:
  - `Web::applySecurityHeaders(Net::Router&)`
  - `Web::installStaticSite(Net::Router&, const std::filesystem::path& webRoot)`
  - `Web::WebSocketHub::{subscribe(topic, peer), publish(topic, json)}`（每条连接单写者）
  - `Web::installBrowserWebSocket(Net::Router&, WebSocketHub&, Auth::SessionStore&, std::vector<std::string> allowedOrigins)`
  - `Web::ProtocolStats::{countH1(), countH2(), countH3(), countWs(), snapshot()}`（原子计数）
  - `Web::NodeTopic = "nodes"`
- Consumes: Task 1 的 Common、Task 3 的 SessionStore。

- [ ] **Step 1: 安全头与静态站点测试先看红**

静态站点测试：临时目录含 `index.html`、`assets/app.js`；GET `/` 返回 index；GET `/assets/app.js` 返回 JS 且 `Cache-Control` 长缓存；GET `/device/nodes`（无文件）返回 index 且 `Cache-Control: no-store`；GET `/api/unknown` 返回 404 而非 index；`GET /../README.md` 不得越出 webRoot。安全头测试断言 nosniff、DENY、Referrer-Policy、CSP；HTTP 响应没有 HSTS，TLS 响应有 HSTS。

- [ ] **Step 2: 实现静态站点**

自行解析路径，禁止用 `staticFileDir` 的兜底（它没有 SPA 回退）。规则：
- 只处理 GET/HEAD；`/api/` 前缀直接 404。
- 规范化路径并拒绝 `..`；只允许 webRoot 内的普通文件。
- 已知扩展名映射 Content-Type；`index.html` 与无扩展名回退都发 `Cache-Control: no-store`；`/assets/` 下发 `public, max-age=31536000, immutable`。
- 未命中且不是 `/api/` 的 GET → 发 index.html。

- [ ] **Step 3: WebSocket 测试先看红**

用 `Net::WebSocketClient`：
- 连接 `/api/ws` 后不发消息，5 秒后应收到关闭码 1008。
- 发 `{"type":"auth","token":"bad"}` → 关闭码 1008。
- 发有效 token → 收到 `{"type":"auth","ok":true}`；随后 `WebSocketHub.publish("nodes", ...)`，客户端收到 `node.status`。
- 断开后 hub 不保留失效 peer。

- [ ] **Step 4: 实现 WebSocket**

升级前用 `WebSocketUpgradePolicy` 设置 Origin 白名单（`AllowedList` 模式，含 dev 的 5173 与 8080）。升级后启动一个 5 秒定时器；首条消息必须是 auth，token 无效或超时 `peer.close(1008, "unauthorized")`。认证后：
- 订阅 `nodes` topic；
- 每 25 秒发 ping，记录 pong；
- 收到 hub 发布时转发为 `{"type":"node.status","data":...}`。

`WebSocketHub` 内部对每个 peer 维护出站队列；发布协程只入队，单写者 drain。不要在多个协程里同时调用 `sendText`（引擎的 peer 契约）。

- [ ] **Step 5: 协议统计中间件**

在 8080 路由上按 `request.httpVersion()` 计 h1/h2；在 8443 路由上同样；在 h3 路由器中间件里计 h3；WS 升级成功计 ws。`ProtocolStats` 用 `std::atomic<std::uint64_t>`，快照返回普通结构。

- [ ] **Step 6: 测试与提交**

Run:

```bash
ctest --preset debug -R "SecurityHeaders|StaticSite|BrowserWebSocket|ProtocolStats" --output-on-failure
git commit -m "feat(Web): 安全响应头、静态 SPA 与浏览器 WebSocket"
```

---

### Task 6: 运维端点与协议统计接口

**Files:**
- Create: `src/Ops/OpsController.h/.cpp`, `MetricsSummary.h/.cpp`, `ReadinessProbe.h/.cpp`
- Create: `src/Ops/CMakeLists.txt`
- Test: `tests/Protocol/TestOpsEndpoints.cpp`

**Interfaces:**
- Produces:
  - `Ops::registerRoutes(Net::Router&, ProtocolStats&, ReadinessProbe&)`
  - `GET /api/ops/protocols` → `{h1,h2,h3,ws,sse}`，权限 `ops:view`
  - `GET /metrics`（9100，Bearer）→ Prometheus 文本
  - `GET /healthz`、`/readyz`（9100，匿名）
- Consumes: Task 5 的 `ProtocolStats`；引擎 `enableMetricsEndpoint`、`ProcessMetricsRegistry`。

- [ ] **Step 1: 测试先看红**

- 9100 的 `/healthz` 200；`/readyz` 在数据库可用时 200、不可用时 503（可用注入的 `ReadinessProbe` 模拟）。
- `/metrics` 不带 Bearer 401，带正确 Bearer 200，正文含 `asyn_http_requests_total` 或引擎实际注册的指标族名（先读引擎实际输出，不猜）。
- `/api/ops/protocols` 在 8080 上带 `admin` token 200，匿名 401，权限不足 403。

- [ ] **Step 2: 实现**

`/metrics` 用引擎的 `enableMetricsEndpoint()`；Bearer 用 `OpsAccessOptions` 或全局中间件。自定义指标：`edgepulse_nodes_online`、`edgepulse_ws_connections`、`edgepulse_h3_requests_total`，用 `ProcessMetricsRegistry::registerMetric` 注册。`ReadinessProbe` 检查数据库 `SELECT 1` 与监听器存活；回调必须 O(1) 快照。

- [ ] **Step 3: 提交**

```bash
ctest --preset debug -R "Ops" --output-on-failure
git commit -m "feat(Ops): 协议统计、就绪探针与 Prometheus 指标"
```

---

### Task 7: TLS、mTLS Agent 监听器与真实 h3 探针

**Files:**
- Create: `scripts/dev-certs.sh`, `scripts/gen-agent-certs.sh`
- Create: `src/App/TlsAssembly.h/.cpp`
- Create: `samples/H3Probe/Main.cpp`, `samples/DeviceAgent/Main.cpp`
- Create: `samples/CMakeLists.txt`, `samples/H3Probe/CMakeLists.txt`, `samples/DeviceAgent/CMakeLists.txt`
- Test: `tests/Protocol/TestMtls.cpp`, `tests/Protocol/TestH3Probe.cpp`
- Modify: `src/App/ApplicationConfiguration.h/.cpp`（TLS 路径、Agent 白名单、h3 开关）

**Interfaces:**
- Produces:
  - `HttpsServer` 8443：证书/私钥来自 `var/certs/server.pem|server-key.pem`，挂 `altSvcMiddleware(8443)`。
  - `QuicServer` 8443/UDP：同一证书，独立 h3 路由器；额外 `GET /api/probe/h3` → `{"proto":"h3"}`。
  - `HttpsServer` 9443：`loadClientCertificateAuthority(ca.pem)`、`setClientCertificateRequired(true)`、`TlsPolicy::allowedPeerSha256Fingerprints = {agentFingerprint}`；只挂 Agent 路由。
  - `H3Probe --url https://127.0.0.1:8443 --ca var/certs/ca.pem`：`HttpClient` 注入 `TlsPolicy{certificateAuthorityFile=ca}`，`setHttp3Enabled(true)`，GET `/api/probe/h3`，200 且 body 含 `"h3"` 时打印 `h3`，否则退出码 1。
  - `edgepulse-agent --server https://127.0.0.1:9443 --ca ... --cert ... --key ... --name ...`：注册 + 3 次心跳。
- Consumes: Task 4 的 AgentRegistry、Task 5 的 ProtocolStats。

- [ ] **Step 1: 证书脚本先看红**

`dev-certs.sh` 在 `var/certs/` 生成：
- `ca.pem` / `ca-key.pem`（ECDSA P-256，CA:TRUE）
- `server.pem` / `server-key.pem`（SAN DNS:localhost, IP:127.0.0.1）
- `agent.pem` / `agent-key.pem`（CN=edgepulse-agent）
- `agent-foreign.pem` / `agent-foreign-key.pem`（同 CA 签发，不在白名单）
脚本幂等；已存在则跳过。`gen-agent-certs.sh <name>` 生成客户端证书与私钥。两个脚本都用 `openssl`，密码空，权限 600（Windows 上忽略 chmod 失败）。

- [ ] **Step 2: mTLS 测试先看红**

启动 9443 监听器，`HttpClient`：
- 不带客户端证书 → 请求失败。
- 带 `agent-foreign.pem` → 握手失败（指纹不在白名单）。
- 带 `agent.pem` → `POST /api/agent/register` 200，返回节点。
同一测试断言 8080 上不存在 `/api/agent/register`（404），证明 Agent 面没有泄漏到公共监听器。

- [ ] **Step 3: h3 测试先看红**

启动 8443 TCP + UDP；`HttpClient` 打开 h3 请求 `/api/probe/h3`：
- 200 且 body 含 `"h3"`；
- 同一路径经 TCP 请求返回 404；
- `/api/ops/protocols` 的 h3 计数增加。
若引擎的 h3 探测在 Windows 上不可用，记录失败原因并保留 h3 专属路由与计数；不得把回落 TCP 的 200 当成 h3 通过（TCP 该路径 404 已保证这一点）。

- [ ] **Step 4: 实现代理与样例**

AgentProbe 用 `HttpClient(loop, {}, tlsPolicy)` 并 `setClientCertificate`；注册体包含自报指纹，指纹从证书文件读取并计算 SHA-256（用 `openssl x509 -fingerprint` 或引擎 `Core::Crypto::Digest`）。H3Probe 用同样的 TLS 策略，打印实际状态码与 body。

- [ ] **Step 5: 验收与提交**

```bash
bash scripts/dev-certs.sh
cmake --build --preset debug
ctest --preset debug -R "Mtls|H3" --output-on-failure
./build/debug/samples/H3Probe --url https://127.0.0.1:8443 --ca var/certs/ca.pem
git commit -m "feat(Net): mTLS Agent 监听器与真实 h3 探针"
```

---

### Task 8: App 装配、配置、日志、运行脚本与验收脚本

**Files:**
- Modify: `src/App/Application.h/.cpp`, `ListenerAssembly.h/.cpp`, `Main.cpp`, `ApplicationConfiguration.h/.cpp`
- Create: `scripts/run-dev.sh`, `scripts/check-acceptance.py`
- Create: `src/App/TlsAssembly.h/.cpp`（Task 7 已建则复用）
- Test: `tests/Protocol/TestAppSmoke.cpp`

**Interfaces:**
- Produces:
  - `edgepulse-server --config config/dev.yaml` 启动 8080/8443/8443-UDP/9443/9100。
  - `scripts/run-dev.sh` 读取 `var/ops-token`（不存在则生成 32 字节随机十六进制），导出 `EDGEPULSE_OPS_BEARER_TOKEN`，启动服务。
  - `python3 scripts/check-acceptance.py` 逐项验收并打印证据，失败非 0。
- Consumes: 全部前序任务。

- [ ] **Step 1: 配置与秘密**

`AppConfig` 字段：`httpPort=8080`、`httpsPort=8443`、`agentPort=9443`、`opsPort=9100`、`opsAddress=127.0.0.1`、`databasePath=var/edgepulse.db`、`webRoot=web`、`certDir=var/certs`、`captchaTtlSeconds=120`、`sessionTtlSeconds=7200`、`exposeCaptchaCode`（dev true/prod false）、`allowedOrigins`、`agentFingerprint`（可选，空表示只校验 CA）、`databaseDriver`（sqlite/mysql）。秘密字段（DB 口令、ops token）只从环境变量 `EDGEPULSE_*` 读取；启动时缺失且必需则退出并打印缺失键名。

- [ ] **Step 2: 装配监听器**

- 8080：HttpServer，公开路由（health/auth/nodes/ops/protocols）+ WS + 静态站点。
- 8443：HttpsServer，公开路由 + WS + 静态站点 + Alt-Svc。
- 8443/UDP：QuicServer，公开路由副本 + `/api/probe/h3`。
- 9443：HttpsServer，mTLS + 仅 Agent 路由。
- 9100：HttpServer 仅回环，运维端点。
每个监听器的 `start()` 协程由 `Application` 持有到收口。优雅停机：收到 SIGINT/SIGTERM 后停止接受、等待在途、flush 日志、关闭数据库。

- [ ] **Step 3: 启动期执行迁移与种子**

`Application::start()` 先应用 V1–V3，再检查 admin 是否存在；不存在则打印明确错误（不自动改种子）。日志初始化用 `LoggerConfigLoader::loadFromConfig(config/logging.yaml)`，控制台 + 滚动文件 + JSON；访问日志含 method/uri/status/duration/request_id/proto/tls/route，不含 Authorization/Cookie。

- [ ] **Step 4: 冒烟测试**

`TestAppSmoke`：在临时端口起 App 的 8080 等价装配，验证 health、静态首页、匿名 401、登录、节点列表。运行后关闭，断言退出码 0、无未捕获异常。

- [ ] **Step 5: 验收脚本**

`check-acceptance.py` 用标准库 `urllib`/`http.client` + `subprocess`：
1. 启动 `edgepulse-server`（或使用已启动实例）。
2. health、captcha/login/me/routes、节点列表。
3. 启动 H3Probe 与 AgentProbe（或调用 C++ 测试二进制），断言 h3 与 mTLS 结果。
4. 无证书 `curl` 9443 失败；错误指纹失败；正确证书成功。
5. `/metrics` Bearer 200、匿名 401。
6. WS：用 Python websocket 客户端？没有第三方库时用 `curl` 升级不够；改为运行 C++ `TestBrowserWebSocket` 并通过 `ctest -R BrowserWebSocket` 取证。脚本调用 ctest 并解析结果。
输出每项的 PASS/FAIL 与关键证据，失败返回 1。

- [ ] **Step 6: 提交**

```bash
git commit -m "feat(App): 装配多监听器、配置日志与开发运行脚本"
git commit -m "test: 增加 P0 验收脚本与冒烟用例"
```

---

### Task 9: 前端 fork、登录验证码与节点页

**Files:**
- Create: `frontend/`（从 `G:/Codes/Github/scaffold/scaffold-vue` 复制，排除 `node_modules`、`.npm-cache`）
- Modify: `frontend/package.json`（name `edgepulse-web`，移除 license 字段，保留 gsap 依赖；新增 `@playwright/test` devDependency、`test:e2e` 脚本）
- Modify: `frontend/vite.config.ts`（代理指向 `http://127.0.0.1:8080`，`ws: true`）
- Modify: `frontend/src/api/auth.ts`（`CaptchaResponse` 增加 `imageSvg?: string`）
- Modify: `frontend/src/views/Login.vue`（优先渲染 `imageSvg`，回退 `code`）
- Modify: `frontend/src/composables/auth.ts`（COMPONENT_MAP 增加 `device/Nodes`；移除 `tool/gen/index`）
- Create: `frontend/src/api/device.ts`, `frontend/src/api/__tests__/device.test.ts`
- Create: `frontend/src/composables/nodeSocket.ts`, `frontend/src/views/device/Nodes.vue`
- Create: `frontend/playwright.config.ts`, `frontend/e2e/login-nodes.spec.ts`
- Delete: `frontend/src/views/tool/Generator.vue` 及仅被它使用的接口（确认无其他引用）
- Create: `frontend/.gitignore` 补充 `node_modules`、`dist`、`test-results`

**Interfaces:**
- Produces:
  - `deviceApi.nodes.list({pageNum,pageSize,keyword,status}) -> PageResult<NodeRow>`
  - `useNodeSocket()`：首条 auth、指数退避、`document.hidden` 降频、卸载关闭。
  - 路由 component 标识 `device/Nodes`。
- Consumes: 后端 8080 API 与 WS。

- [ ] **Step 1: 复制并净化 scaffold**

复制时排除 `node_modules`、`.npm-cache`。删除 README 中 MIT 许可与 GSAP 许可说明；`package.json` 移除 `"license": "MIT"`；保留 `gsap` 依赖（页面动效仍在使用），不新增 UI/图表库。全局替换品牌：
- `package.json` name → `edgepulse-web`
- `index.html` title → `EdgePulse 边缘运维平台`
- `src/data/index.ts` 的 APP 名称/副标题 → EdgePulse
- 登录页能力卡去掉「代码生成」，换成「边缘节点」「遥测看板」等 P0/P2 能力

- [ ] **Step 2: 前端测试先看红**

`device.test.ts` 用注入 fetcher 的 `createHttpClient` 断言 `GET /nodes?...` 的 URL、查询过滤（空值丢弃）、分页解包、401 清理。`auth.test.ts` 增加 `imageSvg` 类型不影响既有断言。运行 `npm run test:unit` 看红。

- [ ] **Step 3: 登录验证码**

`refreshCaptcha` 保存 `uuid`、`imageSvg`、`code`。模板：`imageSvg` 存在时用 `<img :src="svgDataUrl">` 展示（`data:image/svg+xml;charset=utf-8,` + `encodeURIComponent`），否则回退现有字符。`svgDataUrl` 用 `computed`。保持 `form.captcha` 长度上限 6、提交字段不变。

- [ ] **Step 4: 节点页与 WS**

`Nodes.vue` 用 `ListPage`、`SearchForm`、`PermissionButton`、`StatusTag`：
- 列：节点名、状态（online/offline 映射为 启用/停用 或自定义 tag）、Agent 版本、最后心跳、标签。
- 工具栏：刷新；写权限显示「新增节点」。
- 新增/编辑用 `FormDialog`，字段 name/labels（JSON 文本校验）；删除用 `confirm()`。
- `onMounted` 调用列表与 `useNodeSocket`；收到 `node.status` 时按 id 合并行，不在列表的节点刷新列表。
- WS 连接：`ws(s)://<location.host>/api/ws`，首条发 `{type:'auth',token}`；失败按 1s/2s/4s/8s/最多 30s 退避；`visibilitychange` 隐藏时暂停重连，恢复时立即重连；`onBeforeUnmount` 关闭并清理定时器。

- [ ] **Step 5: COMPONENT_MAP 与菜单**

`composables/auth.ts` 的 `COMPONENT_MAP` 增加：

```ts
'device/Nodes': () => import('@/views/device/Nodes.vue'),
```

删除 `'tool/gen/index'` 与 `Generator.vue`；确认 `rg "Generator|tool/gen" src` 无残留（除测试/文档）。后端 V2 种子已提供 `/device/nodes` 菜单。

- [ ] **Step 6: 构建与 E2E**

`playwright.config.ts`：
- `webServer` 启动 `npm run dev -- --host 127.0.0.1`（或脚本启动后端 + Vite），`baseURL: 'http://127.0.0.1:5173'`，`channel: 'msedge'`，`headless: true`。
- `login-nodes.spec.ts` 拦截 `/api/auth/captcha` 响应读取 dev `code` 与 uuid；填 admin/admin123/验证码；断言进入节点页、节点表格可见、导航含「节点」；再断言 WS 状态（后端 AgentProbe 已注册节点时可断言 online，否则只断言连接成功）。

Run:

```bash
cd frontend
npm ci
npm run test:unit
npm run build
npx playwright install --with-deps msedge   # 用系统 Edge 时可省略
npm run test:e2e
```

Expected: 全部通过；`npm run build` 产出 `frontend/dist`。

- [ ] **Step 7: 提交（两笔）**

```bash
git commit -m "feat(Web): fork 前端并实现验证码登录与节点页"
git commit -m "test(Web): 增加 Playwright 登录与节点页验收"
```

---

### Task 10: 文档、覆盖率与最终验收

**Files:**
- Create/Modify: `docs/架构.md`, `docs/API.md`, `docs/部署.md`, `docs/验收.md`, `docs/覆盖率.md`, `README.md`, `CONTRIBUTING.md`
- Create: `deploy/docker/Dockerfile`, `deploy/docker/docker-compose.yml`, `deploy/prometheus/scrape.yml`, `deploy/systemd/edgepulse.service`（P0 只给可运行的最小样例，不声称已验收 Linux）
- Modify: `scripts/check-acceptance.py`（最终取证清单）

- [ ] **Step 1: 文档写实测结果**

README：项目定位、P0 能力、构建命令（带 Conan settings）、运行方式、默认账号（仅开发种子，注明必须改密）、已知边界（SQLite 开发档、P0 单进程、mTLS 指纹自报）。
架构：模块图、依赖方向、监听器、认证时序、数据模型。
API：P0 表格 + 示例请求/响应 + 错误码。
部署：dev 证书生成、环境变量、Linux systemd/Docker 为后续阶段样例，标注未验收项。
验收：逐条命令与期望输出。
覆盖率：按「场景 × 状态 × 验证方式」列 P0 场景与对应用例名，不写百分比。

- [ ] **Step 2: 全量验收**

```bash
# 后端
conan install . --build=missing -s compiler.cppstd=23 -s build_type=Release
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure

# 前端
cd frontend && npm ci && npm run test:unit && npm run build && npm run test:e2e

# 运行与协议
bash scripts/dev-certs.sh
bash scripts/run-dev.sh
python3 scripts/check-acceptance.py
```

全部通过后，停止服务，确认无残留进程。

- [ ] **Step 3: 最终提交**

```bash
git commit -m "docs: 补齐 P0 架构、API、部署与验收文档"
git commit -m "test: 固化 P0 验收证据"
```

---

## 计划自检

- **Spec 覆盖**：spec 第 1.1 节的 8 项分别落在 Task 1–10；R1–R10 在 Task 1/3/7/9 的约束里体现。
- **占位扫描**：没有 TBD/TODO；未实现的 P1+ 模块明确不建目录。
- **类型一致性**：`NodeRecord`、`Session`、`CaptchaChallenge`、`PageResult` 在 Task 2–5 中名称一致；`ProtocolStats` 在 Task 5/6 中一致。
- **已知风险**：Windows 上 h3 客户端探测可能受本机网络栈影响；Task 7 用「TCP 同路径 404」把回落伪装排除，失败时保留证据并如实报告，不用 TCP 200 冒充 h3。


## 执行记录（2026-10-20）

P0 按本计划推进并验收，以下偏差已写入实现与文档：

- Task 5 的 `ProtocolStats` 与 Task 6 的 `/api/ops/protocols` 未实现，移到 P3；h3 的判定改为“专属路径只挂在 QUIC 路由器上，TCP 同路径 404”。
- Task 5 的静态 SPA 与 `TestStaticSite`/`TestProtocolStats` 未实现；P0 浏览器入口为 Vite 5173，后端静态托管推迟到 P3 前。
- `TestBrowserWebSocket` 未写成 C++ 用例；WebSocket 端点由 Playwright 端到端验证首条认证与 `connected` 状态。
- WebSocket 首条认证的 5 秒兜底用计时线程实现（`shortcut`），等引擎提供带超时的 receive 后替换。
- `NodeStatusPublisher` 改为异步回调，以便直接 `co_await` 引擎 Hub 的 `publish`。
- 实际验收：后端 35/35、前端单测 22/22、构建通过、Playwright 1/1、`check-acceptance.py` 17/17。
