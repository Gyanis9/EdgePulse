import { expect, test } from '@playwright/test'

/**
 * 第二因素登录：拦截认证响应，验证前端步骤切换、错误处理与成功落会话。
 * 不启用真实账号的两步验证，避免影响共享的 admin 账号。
 */
function ok(data: unknown) {
  return { code: 200, message: 'ok', data, traceId: 'e2e' }
}

const MFA_USER = {
  id: 99,
  username: 'mfauser',
  nickname: 'MFA User',
  deptName: '研发中心',
  roles: ['ops_team'],
  permissions: [],
}

test('login shows the second-factor form when the server returns a challenge', async ({ page }) => {
  let verifyCalls = 0
  await page.route('**/api/auth/login', route => route.fulfill({
    json: ok({ mfaRequired: true, challengeToken: 'e2e-challenge', expiresIn: 300 }),
  }))
  await page.route('**/api/auth/mfa/verify', async (route) => {
    verifyCalls += 1
    const body = route.request().postDataJSON() as { challengeToken?: string; code?: string }
    expect(body.challengeToken).toBe('e2e-challenge')
    expect(body.code).toBe('123456')
    await route.fulfill({ json: { code: 401, message: '两步验证失败：验证码错误或已过期，请重新登录', data: null, traceId: 'e2e' } })
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

test('password login completes the second factor and enters the shell', async ({ page }) => {
  let verifyCalls = 0
  await page.route('**/api/auth/login', route => route.fulfill({
    json: ok({ mfaRequired: true, challengeToken: 'e2e-challenge', expiresIn: 300 }),
  }))
  await page.route('**/api/auth/mfa/verify', async (route) => {
    verifyCalls += 1
    expect(route.request().postDataJSON()).toEqual({ challengeToken: 'e2e-challenge', code: '654321' })
    await new Promise(resolve => setTimeout(resolve, 400))
    await route.fulfill({ json: ok({ token: 'e2e-session', expiresIn: 3600 }) })
  })
  await page.route('**/api/auth/me', route => route.fulfill({ json: ok(MFA_USER) }))
  await page.route('**/api/auth/routes', route => route.fulfill({ json: ok({ routes: [] }) }))

  const captchaResponse = page.waitForResponse(
    response => response.url().includes('/api/auth/captcha') && response.request().method() === 'GET',
  )
  await page.goto('/login')
  const captchaPayload = await (await captchaResponse).json()
  const captchaCode: string | undefined = captchaPayload?.data?.code
  expect(captchaCode, '开发档验证码应包含 code 供端到端测试使用').toBeTruthy()

  await page.getByPlaceholder('请输入账号').fill('mfauser')
  await page.getByPlaceholder('请输入密码').fill('secret123')
  await page.getByPlaceholder('请输入右侧字符').fill(captchaCode ?? '')
  await page.getByRole('button', { name: '登录', exact: true }).click()
  await expect(page.getByRole('heading', { name: '两步验证' })).toBeVisible()

  await page.getByPlaceholder('请输入 6 位动态码').fill('654321')
  // 同一帧内提交两次：submitting 置位后第二次提交应被忽略
  await page.locator('form.login-form').evaluate((form: HTMLFormElement) => {
    form.requestSubmit()
    form.requestSubmit()
  })

  await page.waitForURL(url => !url.pathname.includes('/login'), { timeout: 20_000 })
  await expect(page.getByRole('button', { name: /MFA User/ })).toBeVisible()
  expect(verifyCalls, '重复提交不应发起第二次校验请求').toBe(1)
  expect(await page.evaluate(() => localStorage.getItem('scaffold.token'))).toBe('e2e-session')
  expect(await page.evaluate(() => localStorage.getItem('scaffold.user'))).toContain('MFA User')
})

test('oidc challenge completes and the one-time code leaves the URL', async ({ page }) => {
  await page.route('**/api/auth/oidc/enabled', route => route.fulfill({
    json: ok({ enabled: true, label: '企业登录' }),
  }))
  await page.route('**/api/auth/oidc/exchange', async (route) => {
    expect(route.request().postDataJSON()).toEqual({ code: 'one-time-code' })
    await route.fulfill({
      json: ok({ mfaRequired: true, challengeToken: 'oidc-challenge', expiresIn: 300, returnPath: '/' }),
    })
  })
  await page.route('**/api/auth/mfa/verify', async (route) => {
    expect(route.request().postDataJSON()).toEqual({ challengeToken: 'oidc-challenge', code: '246810' })
    await route.fulfill({ json: ok({ token: 'oidc-session', expiresIn: 3600 }) })
  })
  await page.route('**/api/auth/me', route => route.fulfill({
    json: ok({ ...MFA_USER, username: 'oidcmfa', nickname: 'OIDC MFA' }),
  }))
  await page.route('**/api/auth/routes', route => route.fulfill({ json: ok({ routes: [] }) }))

  await page.goto('/login?oidcCode=one-time-code')
  await expect(page.getByRole('heading', { name: '两步验证' })).toBeVisible()
  await expect(page).not.toHaveURL(/oidcCode/)

  await page.getByPlaceholder('请输入 6 位动态码').fill('246810')
  await page.getByRole('button', { name: '验证并登录' }).click()
  await page.waitForURL(url => !url.pathname.includes('/login'), { timeout: 20_000 })
  await expect(page.getByRole('button', { name: /OIDC MFA/ })).toBeVisible()
  expect(await page.evaluate(() => localStorage.getItem('scaffold.token'))).toBe('oidc-session')
})
