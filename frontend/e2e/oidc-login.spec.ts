import { expect, test } from '@playwright/test'

/**
 * OIDC 登录前端接线：启用查询 → 跳转授权 → 回调一次性 code → 换取会话。
 * IdP 由路由拦截模拟，验证的是前端状态机而不是 IdP 实现。
 */
test('oidc login exchanges one-time code and enters the shell', async ({ page }) => {
  const cleanToken = 'a'.repeat(64)
  await page.route('**/api/auth/oidc/enabled', route => route.fulfill({
    json: { code: 200, message: 'ok', data: { enabled: true, label: '企业登录' }, traceId: 'e2e' },
  }))
  await page.route('**/api/auth/oidc/start**', route => route.fulfill({
    status: 302,
    headers: { location: '/login?oidcCode=one-time-code&return=%2F' },
    body: '',
  }))
  await page.route('**/api/auth/oidc/exchange', route => route.fulfill({
    json: { code: 200, message: 'ok', data: { token: cleanToken, expiresIn: 7200, returnPath: '/' }, traceId: 'e2e' },
  }))
  await page.route('**/api/auth/me', route => route.fulfill({
    json: {
      code: 200, message: 'ok',
      data: { username: 'oidcuser', nickname: 'OIDC User', deptId: 101, deptName: '研发中心', roles: ['ops_team'], permissions: [] },
      traceId: 'e2e',
    },
  }))
  await page.route('**/api/auth/routes', route => route.fulfill({
    json: { code: 200, message: 'ok', data: { routes: [] }, traceId: 'e2e' },
  }))

  await page.goto('/login')
  const oidcButton = page.getByRole('button', { name: '企业登录' })
  await expect(oidcButton).toBeVisible()
  await oidcButton.click()
  await page.waitForURL(url => url.pathname !== '/login', { timeout: 20_000 })
  await expect(page.getByRole('button', { name: /OIDC User/ })).toBeVisible()
})
