# EdgePulse 前端

EdgePulse 边缘节点与设备运维平台的浏览器管理后台，基于 Vue 3 + TypeScript + Vite，沿用内部管理脚手架的珍珠玻璃主题与组件库。

## 开发

```bash
npm ci
npm run dev
```

开发服务器监听 `5173`，`/api` 与 `/api/ws` 代理到 `http://127.0.0.1:8080`。后端由仓库根目录的 `scripts/run-dev.sh` 启动。

## 测试与构建

```bash
npm run test:unit
npm run build
npm run test:e2e
```

端到端用例使用系统 Edge（`channel: 'msedge'`），不下载浏览器；后端与 Vite 由 Playwright 的 `webServer` 启动。

## 约定

- 路由与菜单来自后端 `/api/auth/routes`；页面组件在 `src/composables/auth.ts` 的 `COMPONENT_MAP` 登记。
- 登录验证码优先渲染后端返回的 `imageSvg`，没有时回退到开发档的 `code`。
- 会话存储沿用脚手架的 `scaffold.*` 键，401 由 `http.ts` 统一清理并回登录页。
