# 贡献指南

感谢参与 EdgePulse。改动必须在真实的 MSVC、GCC/Clang 构建与相关用例上验证，不接受只读代码的结论。

## 环境与构建

- Windows + MSVC ≥ 19.40，或 Linux + GCC ≥ 13 / Clang ≥ 17。
- CMake ≥ 3.25、Ninja、Conan 2、Python 3、Node.js ≥ 20。
- Git 含 submodule；OpenSSL 用于开发证书与固件签名。

```bash
git submodule update --init --recursive

conan create 3rd/AsynGyanis/packaging/conan --build=missing \
  -s compiler.cppstd=23 -s build_type=Release -o "&:with_mysql=True"

conan install . --build=missing -s compiler.cppstd=23 -s build_type=Release
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

`debug` 预设是 RelWithDebInfo；Windows/MSVC 下强制 `/MD` 运行期库，与 Release 引擎包共用 CRT。改了 `CMakeLists.txt`、依赖或公开头后重新执行 `conan install .`，不要手工改 `build/generators`。

## 后端测试

- 统一 GoogleTest，按单元、集成、协议分成三个目标；新增功能与用例同批提交。
- 默认用例使用真实 SQLite 临时库，不 mock 数据库；MySQL/Redis 集成由环境变量门控，缺变量时 `GTEST_SKIP`，不伪造通过。
- 并发与时序断言必须由用例自己构造成立条件，不赌调度；不要依赖机器上安装了某个字体、语言包或固定端口。
- 按名称过滤：

```bash
ctest --preset debug -R "Enrollment|NodeConfig|NodeLifecycle" --output-on-failure
ctest --preset debug -R "SystemEndpoints|RateLimit|StrictSystemEndpoints" --output-on-failure
```

- 真机 MySQL/Redis 用例（需要 Docker）：

```bash
python scripts/run-external-tests.py
```

脚本启动一次性 MySQL 8.4 与 Redis 7 容器、在随机宿主端口上设置 `EDGEPULSE_TEST_*`，跑完 `ExternalStores|RealtimeBus` 后删除容器；失败时加 `--keep` 保留现场，脚本会打印容器名与映射端口，用 `docker logs <容器名>` 接着排查。容器口令每次随机生成，只绑 `127.0.0.1` 的临时端口，并通过 Docker `-e` 出现在宿主进程参数里；这是仅用于测试的取舍，不要照搬到部署。镜像名可用 `--mysql-image`/`--redis-image` 覆盖，离线环境先用 `docker pull` 备好本地镜像，或指到本机已有的等价标签（如 `redis:7-alpine`）。

- 也可以自己准备实例，按下面的变量接线（端口不是固定的 6379/3306 时必须显式传）：

```bash
EDGEPULSE_TEST_MYSQL_HOST=127.0.0.1 EDGEPULSE_TEST_MYSQL_PORT=3306 \
EDGEPULSE_TEST_MYSQL_USER=root EDGEPULSE_TEST_MYSQL_PASSWORD=... \
EDGEPULSE_TEST_MYSQL_DATABASE=edgepulse_test \
EDGEPULSE_TEST_REDIS_HOST=127.0.0.1 EDGEPULSE_TEST_REDIS_PORT=6379 \
EDGEPULSE_TEST_REDIS_PASSWORD=... \
ctest --preset debug -R "ExternalStores|RealtimeBus" --output-on-failure
```

## 前端测试

```bash
cd frontend
npm ci
npm run test:unit
npm run build
npm run test:e2e
```

- 单测沿用 `node --test --experimental-strip-types`，不新增组件测试框架；只跑一个文件可直接把 `src/**/__tests__/*.test.ts` 路径交给 `node --test --experimental-strip-types`。
- `npm run build` 包含 `vue-tsc --noEmit` 与 Vite 构建，类型错误按失败处理。
- E2E 使用本机已安装的 Edge（`channel: 'msedge'`），不下载浏览器；Playwright 会通过 `webServer` 启动后端与 Vite，运行前先准备开发证书，并确保 8080、5173 未被不相关进程占用。
- CI 不跑 Playwright；E2E 在具备 Edge 的机器上本地执行。

## CI

- 流水线定义在 `.github/workflows/ci.yml`。`backend-linux` 用 MySQL 8.4 与 Redis 7 服务容器承载真机用例，并在 `ctest` 之后跑一次 MySQL 备份/恢复往返；改完工作流先在本机过 actionlint：

```bash
# Linux 与 CI
docker run --rm -v "$PWD:/repo:ro" -w /repo rhysd/actionlint:latest -color

# Windows Git Bash 需要阻止 MSYS 改写容器内路径
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/repo:ro" -w /repo rhysd/actionlint:latest -color
```

- 推送后查询 `main` 最近一次 run 的结果（换分支加 `--branch <名称>`）：

```bash
export GITHUB_TOKEN=...        # PowerShell：$env:GITHUB_TOKEN = "..."
python scripts/check-actions-status.py
```

令牌需要该仓库的 Actions 读权限（细粒度 PAT 勾 Actions: Read）。脚本只读环境变量且不打印令牌；缺令牌时打印 SKIP 并退出 77，最近一次 run 成功退出 0，失败、查询出错或没有 run 退出 1。令牌只放在当前 shell 会话，不要写进仓库文件或提交历史。

`python scripts/check-actions-status.py --self-test` 用含换行的假令牌走拒绝路径，确认令牌值不出现在 stdout/stderr。

## C++ 规范

- C++23；类/文件 PascalCase，函数/成员 camelCase；非静态成员 `m_` 前缀，静态成员 `s_` 前缀；全局常量 `kPascalCase`。
- 变量名用完整单词，不用 `cfg`/`msg`/`idx` 一类缩写（`id`、`io` 等公认缩写除外）。
- 头文件写 `@file`、`@brief`、`@author`、`@date`、`@version`、`@copyright`；`.cpp` 不写文件头。
- 公开接口写完整的 `@brief`/`@param`/`@return`；实现体在分支、加锁、错误返回与魔法数字处写中文 `//` 注释。
- 注释里不写 Markdown 排版记号，不引用计划编号、任务编号、轮次或会话产物。
- 可恢复错误优先用 `std::expected<T, E>`；调用方可见的文案用中文并写清原因与替代做法，禁止静默失败、静默截断与静默回退。
- RAII；禁止裸 `new`/`delete`、C 风格转换、`NULL`、`<sstream>` 与 `std::endl`。
- 事件循环所属对象只在循环线程上操作；跨线程投递用 `scheduleRemote()`/`postRemote()`。事件循环线程上不做 DNS、同步文件 IO、落盘日志或长锁等待。
- 所有可能承载任意字节的数据按「指针 + 长度」处理，不依赖零终止。
- 格式与静态检查以仓库根 `.clang-format`、`.clang-tidy` 为准。

## 迁移规则

- 迁移文件只追加，不修改已应用文件。当前最高是 `V19__enterprise_identity.sql`，下一个新迁移从 `V20__*.sql` 开始。
- 不要改历史迁移的 SQL、文件名或校验和：`schema_migrations` 记录 SHA-256，不一致会拒绝启动。
- 新增迁移必须同时更新 `tests/Integration/TestMigrations.cpp` 里对该版本的断言与总版本数，并在 SQLite 用例上通过；涉及 MySQL 的 DDL 还要在真机 MySQL 用例或 `scripts/check-docker-stack.py` 中验证。
- 迁移里不写环境相关路径、明文凭据或只对单一驱动生效的语法；需要区分驱动时先确认现有迁移的做法。

## 引擎边界

`3rd/AsynGyanis` 是只读 submodule。不得修改、复制源码进 `src/`、或以 `add_subdirectory` 消费。发现引擎缺陷时：

1. 记录最小复现步骤、期望行为与实际行为；
2. 在应用层绕过，并把绕过点与撤销条件写进代码注释与部署文档；
3. 不在引擎仓库内做本地修补。

## 安全与秘密

- 不提交 `build/`、`var/`、`.env`、`config/prod.yaml`、`config/firmware-keys/`、证书、私钥、日志或任何明文凭据。
- 数据库、Redis、运维口、UDP 遥测、OIDC 与初始口令提示只从环境变量读取；不要把秘密写进 YAML、命令行参数、镜像层或测试快照。
- Issue、PR、日志与截图中先删除 `Authorization`、Cookie、会话令牌、注册令牌明文、私钥、证书口令与个人信息。
- 安全问题不要开公开 Issue，按 [SECURITY.md](SECURITY.md) 的私密渠道报告。

## 提交与 Pull Request

- 使用约定式提交：`<类型>(<范围>): <中文描述>`，首行显示宽度 ≤ 72 列。类型用 `feat`、`fix`、`refactor`、`perf`、`test`、`docs`、`build`、`ci`、`chore`、`revert`。
- 范围用模块名，例如 `Device`、`Auth`、`Web`、`Firmware`、`Ops`、`Config`、`Docs`、`CI`。
- 一次提交只讲一件事；正文写动机、约束与取舍，不复述 diff。破坏性变化用 `!` 或 `BREAKING CHANGE:` 脚注写明迁移方式。
- 提交前：相关构建 rc=0，相关用例通过；用例数、通过数与性能数字以本人刚跑出的结果为准，不照抄旧文档。
- PR 描述按模板写「为什么 / 改了什么 / 怎么验证 / 破坏性变化 / 检查清单」，并说明新增或修改的配置键、迁移、权限码与 API。
- 新增配置键必须有真实消费方，并同步 `config/dev.yaml`、`config/prod.yaml.example` 与 `docs/部署.md`；新增管理端路由必须过 `requireSession(..., 权限码)`，查询必须带租户边界。

## 文档与本地资料

- 用户可见行为变化同步 `README.md`、`docs/API.md`、`docs/部署.md` 或 `docs/架构.md`；不要只改代码。
- `docs/superpowers/` 存放本机设计规范与实施计划，已被 `.gitignore` 排除，不进入版本库；公开文档不要依赖其中的编号或路径。
- 许可证、行为准则与安全策略分别在 `LICENSE`、`CODE_OF_CONDUCT.md`、`SECURITY.md`。
