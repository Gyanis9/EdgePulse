import { expect, test } from '@playwright/test'

/**
 * 登录与节点页：验证码走开发档的 code 字段，登录后进入节点页并等待 WebSocket 认证完成。
 */
test('admin logs in and opens the nodes page with a live WebSocket', async ({ page }) => {
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

  await page.waitForURL('**/device/nodes', { timeout: 20_000 })
  await expect(page.getByRole('heading', { name: '节点', exact: true })).toBeVisible()
  await expect(page.getByText('节点列表')).toBeVisible()
  await expect(page.locator('[data-ws-state="connected"]')).toBeVisible({ timeout: 20_000 })
})
