import { expect, test } from '@playwright/test'

/**
 * 第二因素表单：拦截密码登录响应为 MFA 挑战，只验证前端步骤切换与错误处理。
 * 不启用真实账号的两步验证，避免影响共享的 admin 账号。
 */
test('login shows the second-factor form when the server returns a challenge', async ({ page }) => {
  let verifyCalls = 0
  await page.route('**/api/auth/login', route => route.fulfill({
    status: 200,
    contentType: 'application/json',
    body: JSON.stringify({
      code: 200,
      message: 'success',
      data: { mfaRequired: true, challengeToken: 'e2e-challenge', expiresIn: 300 },
      traceId: 'e2e',
    }),
  }))
  await page.route('**/api/auth/mfa/verify', async (route) => {
    verifyCalls += 1
    const body = route.request().postDataJSON() as { challengeToken?: string; code?: string }
    expect(body.challengeToken).toBe('e2e-challenge')
    expect(body.code).toBe('123456')
    await route.fulfill({
      status: 401,
      contentType: 'application/json',
      body: JSON.stringify({
        code: 401,
        message: '两步验证失败：验证码错误或已过期，请重新登录',
        data: null,
        traceId: 'e2e',
      }),
    })
  })

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

  await expect(page.getByRole('heading', { name: '两步验证' })).toBeVisible()
  await expect(page.getByPlaceholder('请输入 6 位动态码')).toBeVisible()
  await expect(page.getByPlaceholder('请输入密码')).toHaveCount(0)
  await expect(page.getByPlaceholder('请输入右侧字符')).toHaveCount(0)
  expect(await page.evaluate(() => localStorage.getItem('scaffold.token'))).toBeNull()
  expect(await page.evaluate(() => sessionStorage.getItem('scaffold.token'))).toBeNull()

  await page.getByPlaceholder('请输入 6 位动态码').fill('12345')
  await page.getByRole('button', { name: '验证并登录' }).click()
  await expect(page.getByText('请输入 6 位数字动态码')).toBeVisible()
  expect(verifyCalls).toBe(0)

  await page.getByPlaceholder('请输入 6 位动态码').fill('123456')
  await page.getByRole('button', { name: '验证并登录' }).click()
  await expect(page.getByText('两步验证失败：验证码错误或已过期，请重新登录')).toBeVisible()
  expect(verifyCalls).toBe(1)

  await page.getByRole('button', { name: '使用恢复码' }).click()
  await expect(page.getByPlaceholder('请输入 8 位恢复码')).toBeVisible()
  await expect(page.getByRole('button', { name: '使用身份验证器' })).toBeVisible()

  await page.getByRole('button', { name: '返回账号登录' }).click()
  await expect(page.getByPlaceholder('请输入密码')).toBeVisible()
  await expect(page.getByPlaceholder('请输入右侧字符')).toBeVisible()
  await expect(page.getByRole('button', { name: '登录', exact: true })).toBeVisible()
})
