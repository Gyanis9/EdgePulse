import { expect, test, type Page } from '@playwright/test'

/**
 * 自定义数据范围：选择 CUSTOM 后必须出现部门多选，保存后再次编辑能读回同一部门。
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

test('custom data scope requires a department and persists it', async ({ page }) => {
  await login(page)
  await page.goto('/system/role')
  await expect(page.getByRole('heading', { name: '角色管理', exact: true })).toBeVisible()

  const suffix = Date.now().toString().slice(-6)
  const roleName = `自定义角色${suffix}`
  const roleCode = `custom_e2e_${suffix}`

  await page.getByRole('button', { name: '新增角色' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByPlaceholder('如：系统管理员').fill(roleName)
  await dialog.getByPlaceholder('如：sys_admin').fill(roleCode)
  await dialog.getByRole('combobox').click()
  await page.getByRole('option', { name: '自定义数据权限' }).click()

  const departments = dialog.getByRole('group', { name: '自定义部门' })
  await expect(departments).toBeVisible()
  const firstDepartment = departments.getByRole('checkbox').first()
  await expect(firstDepartment).toBeVisible()
  await firstDepartment.check()
  await dialog.getByRole('button', { name: '保存' }).click()
  await expect(page.getByText('角色已新增')).toBeVisible()

  // 开发库会累积历史角色：先按唯一名称过滤，避免被分页挤到第二页
  await page.getByPlaceholder('角色名称 / 编码').fill(roleName)
  await page.getByPlaceholder('角色名称 / 编码').press('Enter')
  const row = page.getByRole('row', { name: new RegExp(roleName) })
  await expect(row).toBeVisible()
  await row.getByRole('button', { name: '编辑' }).click()
  const editDialog = page.getByRole('dialog')
  await expect(editDialog.getByRole('group', { name: '自定义部门' })).toBeVisible()
  await expect(editDialog.getByRole('group', { name: '自定义部门' }).getByRole('checkbox').first()).toBeChecked()
})
