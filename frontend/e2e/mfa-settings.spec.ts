import { expect, test, type Page } from '@playwright/test'

/**
 * 设置对话框：状态与设置接口都在浏览器侧拦截，不写真实的 MFA 记录。
 * 仍用 admin 登录进入外壳，因此准备口令与二维码渲染可以走完整界面。
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

test('MFA settings dialog renders the setup secret and QR code', async ({ page }) => {
  await page.route('**/api/auth/mfa', route => route.fulfill({
    status: 200,
    contentType: 'application/json',
    body: JSON.stringify({
      code: 200,
      message: 'success',
      data: { enabled: false, recoveryCodesRemaining: 0 },
      traceId: 'e2e',
    }),
  }))
  await page.route('**/api/auth/mfa/setup', async (route) => {
    expect(route.request().postDataJSON()).toEqual({ password: 'admin123' })
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        code: 200,
        message: 'success',
        data: {
          secret: 'JBSWY3DPEHPK3PXP',
          otpauthUri: 'otpauth://totp/EdgePulse:admin?secret=JBSWY3DPEHPK3PXP&issuer=EdgePulse',
        },
        traceId: 'e2e',
      }),
    })
  })

  await login(page)
  await page.locator('.user-chip').click()
  await page.getByRole('menuitem', { name: '两步验证' }).click()

  const dialog = page.getByRole('dialog')
  await expect(dialog.getByText('未启用')).toBeVisible()
  await dialog.getByRole('button', { name: '开始设置' }).click()
  await dialog.locator('input[type="password"]').fill('admin123')
  await dialog.getByRole('button', { name: '生成密钥' }).click()

  await expect(dialog.locator('.mfa-secret')).toHaveText('JBSWY3DPEHPK3PXP')
  await expect.poll(
    () => dialog.locator('.mfa-qr canvas').evaluate((canvas: HTMLCanvasElement) => canvas.width),
    { message: '二维码画布应已绘制宽度' },
  ).toBeGreaterThan(0)

  await dialog.getByRole('button', { name: '关闭' }).click()
  await expect(dialog).toHaveCount(0)
})
