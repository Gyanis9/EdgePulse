## 为什么

<!-- 要解决的问题、触发场景与不做的后果。 -->

## 改了什么

<!-- 按模块列出行为变化；新增配置键、迁移、权限码与 API 要单独点名。 -->

## 怎么验证

<!-- 写实际跑过的命令与结果，不要只写“已测试”。 -->

- [ ] `cmake --build --preset debug`
- [ ] `ctest --preset debug --output-on-failure`
- [ ] `cd frontend && npm run test:unit && npm run build`（涉及前端时）
- [ ] 手动或脚本验收（说明环境与步骤）

## 破坏性变化

<!-- 无则写“无”。有则说明受影响的配置、API、迁移、Agent 或部署方式，以及迁移步骤。 -->

## 检查清单

- [ ] 一次 PR 只讲一件事，提交信息符合约定式提交
- [ ] 新增或修改的行为有对应用例；没有删断言来换取通过
- [ ] 新增配置键已同步 `config/dev.yaml`、`config/prod.yaml.example` 与 `docs/部署.md`
- [ ] 新增管理端路由使用 `requireSession(..., 权限码)`，查询带租户边界
- [ ] 迁移只追加，未修改已应用文件；`TestMigrations` 的版本断言已同步
- [ ] 文档已同步（README、API、部署或架构）
- [ ] 提交内容与验证日志不含令牌、私钥、证书、口令或个人信息
