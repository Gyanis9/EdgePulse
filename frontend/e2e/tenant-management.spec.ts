import { expect, test, type Page } from '@playwright/test'

/**
 * 租户管理：平台管理员创建租户后，租户管理员用租户编码登录，只进入本租户数据面。
 */
async function captchaCode(page: Page) {
  // 刷新一次验证码，确保拿到的是本次登录前刚签发的 code（避开页面重载带来的响应失效）
  const refresh = page.getByRole('button', { name: '看不清，换一张' })
  await expect(refresh).toBeEnabled()
  const captchaResponse = page.waitForResponse(
    response => response.url().includes('/api/auth/captcha') && response.request().method() === 'GET',
  )
  await refresh.click()
  const payload = await (await captchaResponse).json()
  const code: string | undefined = payload?.data?.code
  expect(code, '开发档验证码应包含 code 供端到端测试使用').toBeTruthy()
  return code ?? ''
}

async function loginAs(page: Page, username: string, password: string, tenantCode = '') {
  await page.goto('/login')
  const code = await captchaCode(page)
  if (tenantCode) await page.getByPlaceholder('默认租户可留空').fill(tenantCode)
  await page.getByPlaceholder('请输入账号').fill(username)
  await page.getByPlaceholder('请输入密码').fill(password)
  await page.getByPlaceholder('请输入右侧字符').fill(code)
  await page.getByRole('button', { name: '登录', exact: true }).click()
  await page.waitForURL(url => !url.pathname.includes('/login'), { timeout: 20_000 })
}

test('platform admin provisions a tenant and its admin logs in scoped', async ({ page }) => {
  await loginAs(page, 'admin', 'admin123')
  await page.goto('/system/tenant')
  await expect(page.getByRole('heading', { name: '租户管理', exact: true })).toBeVisible()

  const suffix = Date.now().toString().slice(-6)
  const code = `acme${suffix}`
  const name = `示范工厂${suffix}`
  const adminUser = `tenant${suffix}`

  await page.getByRole('button', { name: '新增租户' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByPlaceholder('例如 acme').fill(code)
  await dialog.getByPlaceholder('例如 示范工厂').fill(name)
  await dialog.getByPlaceholder('租户首个管理员账号').fill(adminUser)
  await dialog.getByPlaceholder('默认与登录名相同').fill('租户管理员')
  await dialog.getByPlaceholder('至少 6 位').fill('tenant-pass-1')
  await dialog.getByRole('button', { name: '保存' }).click()
  await expect(page.getByText('租户已创建，管理员账号可立即登录')).toBeVisible()

  const row = page.getByRole('row', { name: new RegExp(name) })
  await expect(row).toBeVisible()
  await expect(row.getByText(code)).toBeVisible()

  // 切换账号：直接清掉本地会话再进登录页，避免确认弹窗干扰租户流程
  await page.evaluate(() => localStorage.clear())
  await page.goto('/login')

  await loginAs(page, adminUser, 'tenant-pass-1', code)
  await expect(page.locator('.user-role')).toContainText(name)

  await page.goto('/system/user')
  await expect(page.getByRole('heading', { name: '用户管理', exact: true })).toBeVisible()
  await expect(page.getByRole('row', { name: new RegExp(adminUser) })).toBeVisible()
  // 默认租户的 admin 不在租户管理员的数据面里
  await expect(page.getByRole('row', { name: /@admin\b/ })).toHaveCount(0)
})
