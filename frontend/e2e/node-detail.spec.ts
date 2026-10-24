import { expect, test, type Page } from '@playwright/test'

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

test('node detail opens from the list and the enrollment dialog is reachable', async ({ page }) => {
  const nodeName = `smoke-${Date.now()}`
  await login(page)
  await page.goto('/device/nodes')
  await expect(page.getByRole('heading', { name: '节点', exact: true })).toBeVisible()

  await page.getByRole('button', { name: '新增节点' }).click()
  const createDialog = page.getByRole('dialog')
  await createDialog.getByPlaceholder('例如 edge-sh-01').fill(nodeName)
  await createDialog.getByRole('button', { name: '保存' }).click()
  await expect(page.getByText('节点已创建')).toBeVisible({ timeout: 10_000 })

  const row = page.getByRole('row', { name: new RegExp(nodeName) })
  await expect(row).toBeVisible()
  await row.getByRole('button', { name: '详情' }).click()
  await expect(page).toHaveURL(/\/device\/nodes\/\d+$/)
  await expect(page.getByRole('heading', { name: nodeName, exact: true })).toBeVisible()
  await expect(page.getByText('节点身份')).toBeVisible()
  await expect(page.getByText('已同步')).toBeVisible()
  await page.getByRole('radio', { name: '配置', exact: true }).click()
  await expect(page.getByText('尚未下发配置')).toBeVisible()
  await expect(page.getByText('下发期望配置')).toBeVisible()

  await page.getByRole('button', { name: '返回列表' }).click()
  await expect(page).toHaveURL(/\/device\/nodes$/)
  await page.getByRole('button', { name: '注册令牌' }).click()
  const tokenDialog = page.getByRole('dialog')
  await expect(tokenDialog.getByRole('heading', { name: '注册令牌', exact: true })).toBeVisible()
  await expect(tokenDialog.getByText('已签发令牌')).toBeVisible()
  await tokenDialog.getByRole('button', { name: '关闭' }).last().click()
  await expect(tokenDialog).toBeHidden()

  const cleanupRow = page.getByRole('row', { name: new RegExp(nodeName) })
  await cleanupRow.getByRole('button', { name: '删除' }).click()
  await page.getByRole('dialog').getByRole('button', { name: '删除', exact: true }).click()
  await expect(page.getByText('节点已删除')).toBeVisible({ timeout: 10_000 })
})

test('node detail keeps an unsaved config draft when the page refreshes', async ({ page }) => {
  const nodeName = `draft-${Date.now()}`
  await login(page)
  await page.goto('/device/nodes')

  await page.getByRole('button', { name: '新增节点' }).click()
  const createDialog = page.getByRole('dialog')
  await createDialog.getByPlaceholder('例如 edge-sh-01').fill(nodeName)
  await createDialog.getByRole('button', { name: '保存' }).click()
  await expect(page.getByText('节点已创建')).toBeVisible({ timeout: 10_000 })

  const row = page.getByRole('row', { name: new RegExp(nodeName) })
  await row.getByRole('button', { name: '详情' }).click()
  await expect(page).toHaveURL(/\/device\/nodes\/\d+$/)
  await page.getByRole('radio', { name: '配置', exact: true }).click()
  // 等面板首次加载完成，避免加载结果在写入草稿之后才回填 textarea
  await expect(page.getByText('尚未下发配置')).toBeVisible()

  const editor = page.getByPlaceholder('{"interval":30}')
  const draft = '{"draft":true}'
  await editor.fill(draft)
  await page.getByRole('button', { name: '刷新' }).first().click()

  await expect(page.getByText('有未保存的配置修改，已保留草稿；保存或回滚后再刷新')).toBeVisible()
  await expect(editor).toHaveValue(draft)

  await page.getByRole('button', { name: '返回列表' }).click()
  await expect(page).toHaveURL(/\/device\/nodes$/)
  const cleanupRow = page.getByRole('row', { name: new RegExp(nodeName) })
  await cleanupRow.getByRole('button', { name: '删除' }).click()
  await page.getByRole('dialog').getByRole('button', { name: '删除', exact: true }).click()
  await expect(page.getByText('节点已删除')).toBeVisible({ timeout: 10_000 })
})
