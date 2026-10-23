# 贡献指南

## 构建与测试

改动必须在真实的 MSVC（或 GCC/Clang）构建上验证，不接受只读代码的结论：

```bash
conan install . --build=missing -s compiler.cppstd=23 -s build_type=Release
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

前端改动：

```bash
cd frontend
npm ci
npm run test:unit
npm run build
npm run test:e2e
```

## C++ 规范

- C++23，CMake ≥ 3.25，Ninja，Conan 2。
- 类/文件 PascalCase；函数/成员 camelCase；非静态成员 `m_` 前缀；全局常量 `kPascalCase`。
- 变量名用完整单词，不用 `cfg`/`msg`/`idx` 一类缩写（`id`、`io` 等公认缩写除外）。
- 头文件写 `@file`、`@brief`、`@author`、`@date`、`@version`、`@copyright`；`.cpp` 不写文件头。
- 公开接口写完整的 `@brief`/`@param`/`@return`；实现体在分支、加锁、错误返回与魔法数字处写中文 `//` 注释。
- 注释里不写 Markdown 排版记号，不引用计划编号或会话产物。
- 可恢复错误优先用 `std::expected<T, E>`；调用方可见的文案用中文并写清原因与替代做法。
- RAII；禁止裸 `new`/`delete`、C 风格转换、`NULL`、`<sstream>` 与 `std::endl`。
- 事件循环所属对象只在循环线程上操作；跨线程投递用 `scheduleRemote()`/`postRemote()`。
- 异步接口的入参按值收，或写明必须活到首次 resume。

## 引擎边界

`3rd/AsynGyanis` 是只读 submodule。不得修改、复制源码进 `src/`、或以 `add_subdirectory` 消费。发现引擎缺陷时：

1. 记录最小复现步骤、期望行为与实际行为；
2. 在应用层绕过，并把绕过点与撤销条件写进代码注释与部署文档；
3. 不在引擎仓库内做本地修补。

## 提交

- 使用约定式提交：`<类型>(<范围>): <中文描述>`，首行显示宽度 ≤ 72 列。
- 一次提交只讲一件事；正文写动机、约束与取舍，不复述 diff。
- 提交前：相关构建 rc=0，相关用例通过；数字（用例数、通过数）以本人刚跑出的结果为准。
- 不提交 `build/`、`var/`、`.env`、证书、私钥、日志或任何明文凭据。

## 测试

- 统一 GoogleTest；新增功能与用例同批提交。
- 数据库集成测试用真实 SQLite 临时文件；MySQL/Redis 用环境变量门控，缺凭据即 `GTEST_SKIP`。
- 不伪造数据库集成测试，不为让用例变绿而删断言。
- 并发与时序断言必须自己构造成立条件，不赌调度运气。
