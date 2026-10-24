import { expect, test, type Page } from '@playwright/test'

/**
 * 设置对话框：状态与设置接口都在浏览器侧拦截，不写真实的 MFA 记录。
 * 仍用 admin 登录进入外壳，因此准备口令与二维码渲染可以走完整界面。
 */
const SECRET = 'JBSWY3DPEHPK3PXP'
const OTPAUTH_URI = `otpauth://totp/EdgePulse:admin?secret=${SECRET}&issuer=EdgePulse`
const RECOVERY_CODES = Array.from({ length: 10 }, (_, i) => `CODE-${String(i + 1).padStart(4, '0')}`)
const REGENERATED_CODES = Array.from({ length: 10 }, (_, i) => `NEW-${String(i + 1).padStart(4, '0')}`)

function ok(data: unknown) {
  return { code: 200, message: 'ok', data, traceId: 'e2e' }
}

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

async function openSettings(page: Page, status: { enabled: boolean; recoveryCodesRemaining: number }) {
  await page.route('**/api/auth/mfa', route => route.fulfill({ json: ok(status) }))
  await login(page)
  await page.locator('.user-chip').click()
  await page.getByRole('menuitem', { name: '两步验证' }).click()
  const dialog = page.getByRole('dialog')
  await expect(dialog.getByText(status.enabled ? '已启用' : '未启用', { exact: true })).toBeVisible()
  return dialog
}

test('MFA settings dialog renders the setup secret and QR code', async ({ page }) => {
  let setupCalls = 0
  await page.route('**/api/auth/mfa/setup', async (route) => {
    setupCalls += 1
    expect(route.request().postDataJSON()).toEqual({ password: 'admin123' })
    await new Promise(resolve => setTimeout(resolve, 400))
    await route.fulfill({ json: ok({ secret: SECRET, otpauthUri: OTPAUTH_URI }) })
  })

  const dialog = await openSettings(page, { enabled: false, recoveryCodesRemaining: 0 })
  await dialog.getByRole('button', { name: '开始设置' }).click()
  await dialog.locator('input[type="password"]').fill('admin123')

  // 同一帧内连点两次：保存态应在第一次点击后立即置位，第二次被忽略
  await dialog.getByRole('button', { name: '生成密钥' }).evaluate((button: HTMLButtonElement) => {
    button.click()
    button.click()
  })

  await expect(dialog.locator('.mfa-secret')).toHaveText(SECRET)
  await expect(dialog.locator('.mfa-uri')).toHaveText(OTPAUTH_URI)
  expect(setupCalls, '重复点击不应发起第二次密钥请求').toBe(1)

  await expect.poll(
    () => dialog.locator('.mfa-qr canvas').evaluate((canvas: HTMLCanvasElement) => {
      const context = canvas.getContext('2d')
      if (!context) return 0
      const { data } = context.getImageData(0, 0, canvas.width, canvas.height)
      let dark = 0
      for (let i = 0; i < data.length; i += 4) {
        if (data[i] < 128) dark += 1
      }
      return dark
    }),
    { message: '二维码画布应包含实际绘制的深色模块' },
  ).toBeGreaterThan(100)
  // qrcode 按整数模块缩放，请求的 180 会落到 179；这里同时排除默认的 300 画布
  const qrWidth = await dialog.locator('.mfa-qr canvas').evaluate((canvas: HTMLCanvasElement) => canvas.width)
  expect(qrWidth).toBeGreaterThan(170)
  expect(qrWidth).toBeLessThanOrEqual(180)
  const qrDataUrl = await dialog.locator('.mfa-qr canvas').evaluate((canvas: HTMLCanvasElement) => canvas.toDataURL())
  expect(qrDataUrl.startsWith('data:image/png;base64,')).toBe(true)
  expect(qrDataUrl.length).toBeGreaterThan(1000)

  await dialog.getByRole('button', { name: '关闭', exact: true }).click()
  await expect(dialog).toHaveCount(0)
})

test('enable flow reveals one-time recovery codes and blocks closing until saved', async ({ page }) => {
  let enableBody: unknown
  await page.route('**/api/auth/mfa/setup', route => route.fulfill({ json: ok({ secret: SECRET, otpauthUri: OTPAUTH_URI }) }))
  await page.route('**/api/auth/mfa/enable', async (route) => {
    enableBody = route.request().postDataJSON()
    await route.fulfill({ json: ok({ recoveryCodes: RECOVERY_CODES }) })
  })

  const dialog = await openSettings(page, { enabled: false, recoveryCodesRemaining: 0 })
  await dialog.getByRole('button', { name: '开始设置' }).click()
  await dialog.locator('input[type="password"]').fill('admin123')
  await dialog.getByRole('button', { name: '生成密钥' }).click()
  await expect(dialog.locator('.mfa-secret')).toHaveText(SECRET)
  await dialog.getByPlaceholder('6 位数字').fill('123456')
  await dialog.getByRole('button', { name: '启用两步验证' }).click()

  await expect(dialog.getByText('恢复码只显示这一次')).toBeVisible()
  expect(enableBody).toEqual({ password: 'admin123', code: '123456' })
  await expect(dialog.getByText(RECOVERY_CODES[0], { exact: true })).toBeVisible()
  await expect(dialog.getByText(`共 ${RECOVERY_CODES.length} 枚`)).toBeVisible()

  // 恢复码只显示一次：该阶段不接受 Escape、标题栏关闭或返回
  await page.keyboard.press('Escape')
  await expect(dialog).toBeVisible()
  await expect(dialog.getByRole('button', { name: '关闭', exact: true })).toHaveCount(0)
  await expect(dialog.getByRole('button', { name: '返回', exact: true })).toHaveCount(0)
  await expect(dialog.getByText(RECOVERY_CODES[0], { exact: true })).toBeVisible()

  await dialog.getByRole('button', { name: '我已保存' }).click()
  await expect(dialog.getByText('已启用', { exact: true })).toBeVisible()
  await expect(dialog.getByText(`剩余 ${RECOVERY_CODES.length} 个恢复码`)).toBeVisible()
  await dialog.locator('.modal-head').getByRole('button', { name: '关闭' }).click()
  await expect(dialog).toHaveCount(0)
})

test('disable flow submits password and TOTP code', async ({ page }) => {
  let disableBody: unknown
  await page.route('**/api/auth/mfa/disable', async (route) => {
    disableBody = route.request().postDataJSON()
    await route.fulfill({ json: ok(null) })
  })

  const dialog = await openSettings(page, { enabled: true, recoveryCodesRemaining: 3 })
  await expect(dialog.getByText('剩余 3 个恢复码')).toBeVisible()
  await dialog.getByRole('button', { name: '关闭两步验证' }).click()
  await dialog.locator('input[type="password"]').fill('admin123')
  await dialog.getByPlaceholder('6 位数字').fill('654321')
  await dialog.getByRole('button', { name: '确认关闭' }).click()

  expect(disableBody).toEqual({ password: 'admin123', code: '654321' })
  await expect(dialog.getByText('未启用', { exact: true })).toBeVisible()
  await expect(dialog.getByRole('button', { name: '开始设置' })).toBeVisible()
})

test('regenerate flow replaces the recovery codes once more', async ({ page }) => {
  let regenerateBody: unknown
  await page.route('**/api/auth/mfa/recovery-codes', async (route) => {
    regenerateBody = route.request().postDataJSON()
    await route.fulfill({ json: ok({ recoveryCodes: REGENERATED_CODES }) })
  })

  const dialog = await openSettings(page, { enabled: true, recoveryCodesRemaining: 1 })
  await dialog.getByRole('button', { name: '重新生成恢复码' }).click()
  await dialog.getByPlaceholder('6 位数字').fill('112233')
  await dialog.getByRole('button', { name: '生成新恢复码' }).click()

  expect(regenerateBody).toEqual({ code: '112233' })
  await expect(dialog.getByText(REGENERATED_CODES[0], { exact: true })).toBeVisible()
  await expect(dialog.getByText(`共 ${REGENERATED_CODES.length} 枚`)).toBeVisible()
  await dialog.getByRole('button', { name: '我已保存' }).click()
  await expect(dialog.getByText('已启用', { exact: true })).toBeVisible()
  await expect(dialog.getByText(`剩余 ${REGENERATED_CODES.length} 个恢复码`)).toBeVisible()
})

test('closing during an in-flight setup discards the late secret', async ({ page }) => {
  let setupCalls = 0
  await page.route('**/api/auth/mfa/setup', async (route) => {
    setupCalls += 1
    await new Promise(resolve => setTimeout(resolve, 700))
    await route.fulfill({ json: ok({ secret: SECRET, otpauthUri: OTPAUTH_URI }) })
  })

  const dialog = await openSettings(page, { enabled: false, recoveryCodesRemaining: 0 })
  await dialog.getByRole('button', { name: '开始设置' }).click()
  await dialog.locator('input[type="password"]').fill('admin123')
  await dialog.getByRole('button', { name: '生成密钥' }).click()
  await dialog.getByRole('button', { name: '关闭', exact: true }).click()
  await expect(dialog).toHaveCount(0)

  // 重新打开时旧请求仍在路上，迟到的密钥不能落回界面
  await page.locator('.user-chip').click()
  await page.getByRole('menuitem', { name: '两步验证' }).click()
  const reopened = page.getByRole('dialog')
  await expect(reopened.getByText('未启用', { exact: true })).toBeVisible()
  await page.waitForTimeout(900)
  await expect(reopened.locator('.mfa-secret')).toHaveCount(0)
  await expect(reopened.locator('.mfa-uri')).toHaveCount(0)
  await expect(reopened.locator('.mfa-qr canvas')).toHaveCount(0)
  expect(setupCalls).toBe(1)

  await reopened.locator('.modal-head').getByRole('button', { name: '关闭' }).click()
  await expect(reopened).toHaveCount(0)
})
