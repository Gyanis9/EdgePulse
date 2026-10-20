import { expect, test, type Page } from '@playwright/test'

/**
 * 登录与主要页面：验证码走开发档的 code 字段；登录后依次打开节点、系统用户与设备域页面。
 */
async function login(page: Page) {
  const captchaResponse = page.waitForResponse(
    response => response.url().includes('/api/auth/captcha') && response.request().method() === 'GET',
  )
  await page.goto('/login')
  const captchaPayload = await (await captchaResponse).json()
  const captchaCode: string | undefined = captchaPayload?.data?.code
  expect(captchaCode, '开发档验证码应包含 code 供端到端测试使用').toBeTruthy()

  await page.getByPlaceholder('请输入账号').fill('admin')
  await page.getByPlaceholder('请输入密码').fill('admin123')
  await page.getByPlaceholder('请输入右侧字符').fill(captchaCode ?? '')
  await page.getByRole('button', { name: '登录', exact: true }).click()
  await page.waitForURL(url => !url.pathname.includes('/login'), { timeout: 20_000 })
}

test('admin logs in and opens the nodes page with a live WebSocket', async ({ page }) => {
  await login(page)
  await page.goto('/device/nodes')
  await expect(page.getByRole('heading', { name: '节点', exact: true })).toBeVisible()
  await expect(page.getByText('节点列表')).toBeVisible()
  await expect(page.locator('[data-ws-state="connected"]')).toBeVisible({ timeout: 20_000 })
})

test('system user page loads from the management API', async ({ page }) => {
  await login(page)
  await page.goto('/system/user')
  await expect(page.getByRole('heading', { name: '用户管理', exact: true })).toBeVisible()
  await expect(page.getByText('用户列表')).toBeVisible()
})

test('device domain pages render with live data endpoints', async ({ page }) => {
  await login(page)
  await page.goto('/device/alarms')
  await expect(page.getByRole('heading', { name: '告警中心', exact: true })).toBeVisible()
  await page.goto('/device/telemetry')
  await expect(page.getByRole('heading', { name: '遥测数据', exact: true })).toBeVisible()
  await page.goto('/device/firmware')
  await expect(page.getByRole('heading', { name: '固件管理', exact: true })).toBeVisible()
  await page.goto('/device/commands')
  await expect(page.getByRole('heading', { name: '命令中心', exact: true })).toBeVisible()
})
