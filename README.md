# EdgePulse

EdgePulse 是面向边缘节点与设备的运维平台：控制面是浏览器管理后台，数据面是运行在边缘节点上的 `edgepulse-agent`。后端基于 C++23 与 AsynGyanis 引擎，前端 fork 自内部 Vue 3 管理脚手架。

当前状态：P0 纵向切片开发中。已具备可运行的工程骨架、引擎 Conan 消费、统一响应包络与健康检查；认证、节点、mTLS Agent、h3 与前端页面按 `docs/superpowers/plans/2026-10-20-edgepulse-p0.md` 推进。

## 目录

- `src/`：后端模块（App、Common、Web、Auth、System、Device、Ops）
- `frontend/`：Vue 3 管理后台
- `third_party/AsynGyanis/`：引擎 git submodule，固定 `v4.0.0`，只读
- `migrations/`：SQL 迁移
- `config/`：非敏感配置样例
- `scripts/`：证书、开发运行与验收脚本
- `docs/`：架构、API、部署、验收与覆盖率

## 前置条件

- Windows + MSVC ≥ 19.40 或 Linux + GCC ≥ 13 / Clang ≥ 17
- CMake ≥ 3.25、Ninja、Conan 2、Python 3、Node.js ≥ 20
- Git（含 submodule）

本机已用的工具链位置：`G:\Tools\CMake`、`G:\Tools\ninja`、`G:\Tools\Python\Python314`、`G:\Tools\nodejs`、`G:\Tools\Microsoft Visual Studio\18\Community`。

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

构建目录 `build/` 与运行目录 `var/` 都不进版本库。

## 运行

```bash
./build/debug/edgepulse-server --config config/dev.yaml
```

当前可访问：

```bash
curl -i http://127.0.0.1:8080/api/health
```

开发档默认使用 SQLite（`var/edgepulse.db`，由迁移在启动时创建）。所有口令、token 与私钥只从环境变量或未跟踪文件读取；仓库内没有明文凭据。

## 测试

```bash
ctest --preset debug --output-on-failure
```

依赖真实 MySQL/Redis 的用例会用 `EDGEPULSE_TEST_*` 环境变量门控；缺凭据时 `GTEST_SKIP`，不 mock 数据库制造假绿。

## 编码与提交

- C++23；类/文件 PascalCase，函数/成员 camelCase，成员变量 `m_` 前缀；中文 Doxygen 注释。
- 代码格式以仓库根 `.clang-format`、`.clang-tidy` 为准（与引擎同一份）。
- 提交信息遵循约定式提交，首行 ≤ 72 列，正文写动机；按模块分批提交。
- 引擎目录只读；发现引擎缺陷时记录复现步骤，在应用层绕过。

## 文档

- [架构](docs/架构.md)（P0 后补齐）
- [API](docs/API.md)（P0 后补齐）
- [部署](docs/部署.md)（P0 后补齐）
- [验收](docs/验收.md)（P0 后补齐）
- [覆盖率](docs/覆盖率.md)（P0 后补齐）
- [设计规范](docs/superpowers/specs/2026-10-20-edgepulse-design.md)
- [P0 实施计划](docs/superpowers/plans/2026-10-20-edgepulse-p0.md)
