import { expect, test, type Page } from '@playwright/test'

/**
 * API 令牌页：创建后一次性明文只出现一次，关闭后从 DOM 与浏览器存储消失；
 * 保存中重复回车只发一次创建请求，最后吊销测试令牌收尾。
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

async function storageDump(page: Page) {
  return page.evaluate(() => {
    const entries = (storage: Storage) => Object.fromEntries(Object.keys(storage).map(key => [key, storage.getItem(key) ?? '']))
    return JSON.stringify({ local: entries(localStorage), session: entries(sessionStorage) })
  })
}

let adminToken: string | undefined
let createdTokenId: number | undefined

test.afterEach(async ({ request }) => {
  if (!adminToken || !createdTokenId) return
  // 用例中途失败时也吊销已创建的令牌，避免开发库累积可用凭据
  try {
    await request.delete(`/api/system/api-tokens/${createdTokenId}`, { headers: { Authorization: `Bearer ${adminToken}` } })
  } catch {
    // 已吊销或后端已关闭时忽略
  }
})

test('api token create shows one-time secret once and revoke cleans it up', async ({ page }) => {
  await login(page)
  adminToken = await page.evaluate(() => localStorage.getItem('scaffold.token') ?? sessionStorage.getItem('scaffold.token') ?? undefined)

  // 拖慢创建响应，确保第二次回车落在保存中的窗口内
  await page.route('**/api/system/api-tokens', async (route) => {
    if (route.request().method() !== 'POST') return route.continue()
    const response = await route.fetch()
    await new Promise(resolve => setTimeout(resolve, 800))
    await route.fulfill({ response })
  })

  let createPosts = 0
  page.on('request', (request) => {
    if (request.method() === 'POST' && new URL(request.url()).pathname === '/api/system/api-tokens') createPosts += 1
  })

  const listResponse = page.waitForResponse(
    response => new URL(response.url()).pathname === '/api/system/api-tokens' && response.request().method() === 'GET',
  )
  await page.goto('/system/apitoken')
  await expect(page.getByRole('heading', { name: 'API 令牌', exact: true })).toBeVisible()
  const listPayload = await (await listResponse).json()
  expect(listPayload.code).toBe(200)
  expect(Array.isArray(listPayload.data)).toBe(true)

  const tokenName = `e2e-token-${Date.now().toString(36)}`
  await page.getByRole('button', { name: '新增令牌' }).click()
  const createDialog = page.getByRole('dialog', { name: '新增 API 令牌' })
  await expect(createDialog).toBeVisible()
  const nameInput = createDialog.getByPlaceholder('例如 ci-pipeline')
  await nameInput.fill(tokenName)
  await createDialog.locator('input[type="number"]').fill('30')
  await createDialog.getByPlaceholder('device:node:list, device:telemetry:read').fill('device:node:list')

  const createdResponse = page.waitForResponse(
    response => response.request().method() === 'POST' && new URL(response.url()).pathname === '/api/system/api-tokens',
  )
  // 保存中再按回车不得发起第二次 POST，否则第一份一次性明文会被覆盖
  await nameInput.press('Enter')
  await nameInput.press('Enter')
  const created = await createdResponse
  expect(created.status()).toBe(200)
  const createdPayload = await created.json()
  expect(createdPayload.code).toBe(200)
  expect(createdPayload.data.name).toBe(tokenName)
  expect(createdPayload.data.token).toMatch(/^ep_api_[0-9a-f]{64}$/)
  createdTokenId = createdPayload.data.id
  await expect.poll(() => createPosts, { message: '重复回车不应重复创建令牌' }).toBe(1)

  const secretDialog = page.getByRole('dialog', { name: '令牌已创建' })
  await expect(secretDialog).toBeVisible()
  const secret = (await secretDialog.locator('.token-value').innerText()).trim()
  expect(secret).toBe(createdPayload.data.token)
  await expect(secretDialog.getByText(/只显示这一次/)).toBeVisible()

  // 明文不得写入任何浏览器存储
  const storedBeforeClose = await storageDump(page)
  expect(storedBeforeClose).not.toContain(secret)
  expect(storedBeforeClose).not.toContain(secret.slice('ep_api_'.length))
  expect(storedBeforeClose).not.toContain('ep_api_')

  await secretDialog.getByRole('button', { name: '我已保存' }).click()
  await expect(secretDialog).toHaveCount(0)
  await expect(page.locator('.token-value')).toHaveCount(0)
  await expect(page.getByText(secret)).toHaveCount(0)
  const storedAfterClose = await storageDump(page)
  expect(storedAfterClose).not.toContain(secret)
  expect(storedAfterClose).not.toContain('ep_api_')

  const row = page.getByRole('row', { name: new RegExp(tokenName) })
  await expect(row).toHaveCount(1)
  await expect(row.getByText('有效', { exact: true })).toBeVisible()
  await expect(page.getByRole('button', { name: '新增令牌' })).toBeVisible()
  await expect(row.getByRole('button', { name: '轮换' })).toBeVisible()
  await expect(row.getByRole('button', { name: '吊销' })).toBeVisible()

  const revokedResponse = page.waitForResponse(
    response => response.request().method() === 'DELETE' && new URL(response.url()).pathname.startsWith('/api/system/api-tokens/'),
  )
  await row.getByRole('button', { name: '吊销' }).click()
  const confirmDialog = page.getByRole('dialog', { name: '吊销令牌' })
  await expect(confirmDialog).toBeVisible()
  await confirmDialog.getByRole('button', { name: '吊销', exact: true }).click()
  expect((await revokedResponse).status()).toBe(200)
  await expect(page.getByText('令牌已吊销')).toBeVisible()
  await expect(row.getByText('已吊销', { exact: true })).toBeVisible()
  await expect(row.getByRole('button', { name: '吊销' })).toHaveCount(0)
  await expect(row.getByRole('button', { name: '轮换' })).toHaveCount(0)
})
