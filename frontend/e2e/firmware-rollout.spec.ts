import { expect, test, type Page, type Route } from '@playwright/test'

/**
 * 灰度发布界面：向导默认值、创建请求正文、详情状态与人工动作。
 * 后端不创建真实灰度，全部通过路由拦截返回聚合快照。
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

async function fulfill(route: Route, data: unknown, status = 200, message = 'ok') {
  await route.fulfill({
    status,
    contentType: 'application/json',
    body: JSON.stringify({ code: status, message, data, traceId: 'e2e-rollout' }),
  })
}

const firmwareRows = [
  {
    id: 5,
    name: 'EdgePulse OS',
    version: '2.0.0',
    fileName: 'edgepulse-os-2.0.0.bin',
    contentType: 'application/octet-stream',
    sizeBytes: 1048576,
    sha256: 'a'.repeat(64),
    signed: true,
    signatureKeyId: 'firmware-2026',
    signatureAlgorithm: 'Ed25519',
    createdAt: '2026-10-25 08:00:00',
    createdBy: 1,
  },
  {
    id: 4,
    name: 'EdgePulse OS',
    version: '1.0.0',
    fileName: 'edgepulse-os-1.0.0.bin',
    contentType: 'application/octet-stream',
    sizeBytes: 1048576,
    sha256: 'b'.repeat(64),
    signed: true,
    signatureKeyId: 'firmware-2026',
    signatureAlgorithm: 'Ed25519',
    createdAt: '2026-10-20 08:00:00',
    createdBy: 1,
  },
]

const nodeRows = [
  {
    id: 101,
    name: 'edge-sh-01',
    status: 'online',
    agentVersion: '1.2.0',
    installedFirmwareVersion: '2.0.0',
    certFingerprint: 'aa',
    labels: null,
    lastSeen: '2026-10-26 10:00:00',
    groupId: 200,
    groupName: '默认分组',
    version: 2,
  },
  {
    id: 102,
    name: 'edge-sh-02',
    status: 'online',
    agentVersion: '1.2.0',
    installedFirmwareVersion: '1.0.0',
    certFingerprint: 'bb',
    labels: null,
    lastSeen: '2026-10-26 10:00:00',
    groupId: 200,
    groupName: '默认分组',
    version: 2,
  },
  {
    id: 103,
    name: 'edge-sh-03',
    status: 'offline',
    agentVersion: '1.2.0',
    installedFirmwareVersion: '1.0.0',
    certFingerprint: 'cc',
    labels: null,
    lastSeen: '2026-10-25 10:00:00',
    groupId: 200,
    groupName: '默认分组',
    version: 2,
  },
]

const policy = {
  waves: [{ percent: 5 }, { percent: 25 }, { percent: 100 }],
  maxConcurrentPerWave: 1,
  settleSeconds: 60,
  observeSeconds: 120,
  maxWaveSeconds: 1800,
  minWaveSuccessRatio: 1,
  minCumulativeSuccessRatio: 1,
  maxFailures: 0,
  autoRollback: false,
  onTimeout: 'pause',
  requiredFreshTelemetrySeconds: 300,
  exemptAlarmMetrics: [],
  requireInstalledVersion: true,
}

function makeRollout(status: 'running' | 'paused' | 'completed', version: number) {
  const completed = status === 'completed'
  const paused = status === 'paused'
  return {
    id: 31,
    tenantId: 1,
    firmwareId: 5,
    name: '生产环境灰度',
    policy,
    status,
    currentWave: completed ? 2 : 1,
    totalWaves: 3,
    createdBy: 1,
    createdAt: '2026-10-26 09:00:00',
    startedAt: '2026-10-26 09:01:00',
    finishedAt: completed ? '2026-10-26 09:20:00' : '',
    pausedAt: paused ? '2026-10-26 09:10:00' : '',
    pauseReason: paused ? '人工暂停' : '',
    lastError: '',
    version,
    waves: [
      {
        id: 1,
        rolloutId: 31,
        waveIndex: 0,
        status: 'passed',
        targetCount: 1,
        dispatchedAt: '2026-10-26 09:01:00',
        deadlineAt: '2026-10-26 09:31:00',
        settledAt: '2026-10-26 09:05:00',
        gate: [{ passed: true, healthy: 1, failed: 0, total: 1, at: '2026-10-26 09:05:00' }],
        createdAt: '2026-10-26 09:00:00',
        updatedAt: '2026-10-26 09:05:00',
      },
      {
        id: 2,
        rolloutId: 31,
        waveIndex: 1,
        status: completed ? 'passed' : 'observing',
        targetCount: 1,
        dispatchedAt: '2026-10-26 09:06:00',
        deadlineAt: '2026-10-26 09:36:00',
        settledAt: completed ? '2026-10-26 09:10:00' : '',
        gate: completed ? [{ passed: true, healthy: 1, failed: 0, total: 1, at: '2026-10-26 09:10:00' }] : [],
        createdAt: '2026-10-26 09:00:00',
        updatedAt: '2026-10-26 09:10:00',
      },
      {
        id: 3,
        rolloutId: 31,
        waveIndex: 2,
        status: completed ? 'passed' : 'pending',
        targetCount: 1,
        dispatchedAt: completed ? '2026-10-26 09:11:00' : '',
        deadlineAt: completed ? '2026-10-26 09:41:00' : '',
        settledAt: completed ? '2026-10-26 09:15:00' : '',
        gate: completed ? [{ passed: true, healthy: 1, failed: 0, total: 1, at: '2026-10-26 09:15:00' }] : [],
        createdAt: '2026-10-26 09:00:00',
        updatedAt: '2026-10-26 09:15:00',
      },
    ],
    targets: [
      {
        id: 11,
        rolloutId: 31,
        waveId: 1,
        tenantId: 1,
        nodeId: 101,
        previousFirmwareId: 4,
        taskId: 201,
        commandId: 301,
        status: 'healthy',
        health: { gate: 'healthy', reason: '心跳与遥测覆盖观察窗口', installedVersion: '2.0.0' },
        updatedAt: '2026-10-26 09:05:00',
      },
      {
        id: 12,
        rolloutId: 31,
        waveId: 2,
        tenantId: 1,
        nodeId: 102,
        previousFirmwareId: 4,
        taskId: 202,
        commandId: 302,
        status: completed ? 'healthy' : 'installing',
        health: completed
          ? { gate: 'healthy', reason: '观察窗口通过', installedVersion: '2.0.0' }
          : { gate: 'installing', reason: '心跳尚未覆盖观察窗口', installedVersion: '1.0.0' },
        updatedAt: '2026-10-26 09:10:00',
      },
      {
        id: 13,
        rolloutId: 31,
        waveId: 3,
        tenantId: 1,
        nodeId: 103,
        previousFirmwareId: 4,
        taskId: 0,
        commandId: 0,
        status: completed ? 'healthy' : 'pending',
        health: completed
          ? { gate: 'healthy', reason: '观察窗口通过', installedVersion: '2.0.0' }
          : { gate: 'pending', reason: '等待上一波通过', installedVersion: '1.0.0' },
        updatedAt: '2026-10-26 09:15:00',
      },
    ],
  }
}

function makeRollbackRollout() {
  return {
    id: 31,
    tenantId: 1,
    firmwareId: 5,
    name: '回滚演练',
    policy,
    status: 'rolling_back',
    currentWave: 1,
    totalWaves: 3,
    createdBy: 1,
    createdAt: '2026-10-26 09:00:00',
    startedAt: '2026-10-26 09:01:00',
    finishedAt: '',
    pausedAt: '',
    pauseReason: '',
    lastError: '回滚失败：节点 102 回滚任务失败',
    version: 4,
    waves: [
      {
        id: 1,
        rolloutId: 31,
        waveIndex: 0,
        status: 'passed',
        targetCount: 1,
        dispatchedAt: '2026-10-26 09:01:00',
        deadlineAt: '2026-10-26 09:31:00',
        settledAt: '2026-10-26 09:05:00',
        gate: [{ passed: true, healthy: 1, failed: 0, total: 1, at: '2026-10-26 09:05:00' }],
        createdAt: '2026-10-26 09:00:00',
        updatedAt: '2026-10-26 09:05:00',
      },
      {
        id: 2,
        rolloutId: 31,
        waveIndex: 1,
        status: 'rolling_back',
        targetCount: 1,
        dispatchedAt: '2026-10-26 09:06:00',
        deadlineAt: '2026-10-26 09:36:00',
        settledAt: '2026-10-26 09:20:00',
        gate: [{ rollback: true, issued: 1, unavailable: 1, at: '2026-10-26 09:21:00' }],
        createdAt: '2026-10-26 09:00:00',
        updatedAt: '2026-10-26 09:21:00',
      },
      {
        id: 3,
        rolloutId: 31,
        waveIndex: 2,
        status: 'pending',
        targetCount: 1,
        dispatchedAt: '',
        deadlineAt: '',
        settledAt: '',
        gate: [],
        createdAt: '2026-10-26 09:00:00',
        updatedAt: '2026-10-26 09:20:00',
      },
    ],
    targets: [
      {
        id: 11,
        rolloutId: 31,
        waveId: 1,
        tenantId: 1,
        nodeId: 101,
        previousFirmwareId: 4,
        taskId: 201,
        commandId: 301,
        status: 'healthy',
        health: { gate: 'healthy', reason: '心跳与遥测覆盖观察窗口', installedVersion: '2.0.0' },
        updatedAt: '2026-10-26 09:05:00',
      },
      {
        id: 12,
        rolloutId: 31,
        waveId: 2,
        tenantId: 1,
        nodeId: 102,
        previousFirmwareId: 4,
        taskId: 202,
        commandId: 302,
        status: 'failed',
        health: { gate: 'rollback_failed', reason: '节点 102 回滚任务失败', installedVersion: '1.0.0' },
        updatedAt: '2026-10-26 09:21:00',
      },
      {
        id: 13,
        rolloutId: 31,
        waveId: 2,
        tenantId: 1,
        nodeId: 103,
        previousFirmwareId: 4,
        taskId: 203,
        commandId: 303,
        status: 'rolling_back',
        health: { gate: 'rollback_pending', installedVersion: '1.0.0' },
        updatedAt: '2026-10-26 09:21:00',
      },
    ],
  }
}

test('firmware rollout wizard defaults, create request, detail statuses and force promote', async ({ page }) => {
  await login(page)

  let createBody: Record<string, unknown> | undefined
  let forceBody: Record<string, unknown> | undefined
  let forcePosts = 0
  let detailState = makeRollout('running', 3)

  await page.route('**/api/nodes**', async (route) => {
    if (route.request().method() !== 'GET') return route.continue()
    await fulfill(route, { rows: nodeRows, total: nodeRows.length, pageNum: 1, pageSize: 100 })
  })

  await page.route('**/api/firmware**', async (route) => {
    const url = new URL(route.request().url())
    const path = url.pathname
    const method = route.request().method()

    if (path === '/api/firmware' && method === 'GET') {
      await fulfill(route, { rows: firmwareRows, total: firmwareRows.length, pageNum: 1, pageSize: 10 })
      return
    }
    if (path === '/api/firmware/policy' && method === 'GET') {
      await fulfill(route, { requireSignature: false, trustedKeyIds: [] })
      return
    }
    if (path === '/api/firmware/5/rollouts' && method === 'POST') {
      createBody = route.request().postDataJSON()
      detailState = makeRollout('running', 3)
      await fulfill(route, detailState)
      return
    }
    if (path === '/api/firmware/5/rollouts' && method === 'GET') {
      await fulfill(route, { rows: [detailState], total: 1, pageNum: 1, pageSize: 10 })
      return
    }
    if (path === '/api/firmware/rollouts/31' && method === 'GET') {
      await fulfill(route, detailState)
      return
    }
    if (path === '/api/firmware/rollouts/31/pause' && method === 'POST') {
      detailState = makeRollout('paused', 4)
      await fulfill(route, detailState)
      return
    }
    if (path === '/api/firmware/rollouts/31/force-promote' && method === 'POST') {
      forcePosts += 1
      forceBody = route.request().postDataJSON()
      detailState = makeRollout('completed', 5)
      await fulfill(route, detailState)
      return
    }
    if (path === '/api/firmware/rollouts/31/resume' && method === 'POST') {
      detailState = makeRollout('running', 6)
      await fulfill(route, detailState)
      return
    }
    await route.continue()
  })

  await page.goto('/device/firmware')
  await expect(page.getByRole('heading', { name: '固件管理', exact: true })).toBeVisible()

  const firmwareRow = page.getByRole('row', { name: /EdgePulse OS/ }).filter({ hasText: '2.0.0' })
  await expect(firmwareRow).toBeVisible()
  await firmwareRow.getByRole('button', { name: '灰度发布' }).click()

  const wizard = page.getByRole('dialog', { name: '新建灰度发布' })
  await expect(wizard).toBeVisible()
  const nameInput = wizard.getByPlaceholder('例如 生产环境灰度')
  await expect(nameInput).toHaveValue('EdgePulse OS 2.0.0 灰度')

  const waveInputs = wizard.locator('.wave-list input')
  await expect(waveInputs).toHaveCount(3)
  await expect(waveInputs.nth(0)).toHaveValue('5')
  await expect(waveInputs.nth(1)).toHaveValue('25')
  await expect(waveInputs.nth(2)).toHaveValue('100')
  await expect(wizard.getByLabel('每波并发上限')).toHaveValue('0')
  await expect(wizard.getByLabel('沉降等待（秒）')).toHaveValue('60')
  await expect(wizard.getByLabel('观察窗口（秒）')).toHaveValue('120')
  await expect(wizard.getByLabel('单波最长时长（秒）')).toHaveValue('1800')
  await expect(wizard.getByLabel('单波最低成功率（%）')).toHaveValue('100')
  await expect(wizard.getByLabel('累计最低成功率（%）')).toHaveValue('100')
  await expect(wizard.getByLabel('最大失败数')).toHaveValue('0')
  await expect(wizard.getByLabel('遥测新鲜度（秒）')).toHaveValue('300')
  await expect(wizard.getByRole('combobox')).toContainText('超时暂停')
  await expect(wizard.getByRole('switch', { name: /自动回滚/ })).not.toBeChecked()
  await expect(wizard.getByRole('switch', { name: /要求节点上报已安装版本/ })).toBeChecked()

  await nameInput.fill('生产环境灰度')
  for (const name of ['edge-sh-01', 'edge-sh-02', 'edge-sh-03']) {
    const nodeRow = wizard.locator('.node-picker-row').filter({ hasText: name })
    await nodeRow.click()
    await expect(nodeRow.getByRole('checkbox')).toBeChecked()
  }

  const createRequestPromise = page.waitForRequest(
    request => request.method() === 'POST' && new URL(request.url()).pathname === '/api/firmware/5/rollouts',
  )
  await wizard.getByRole('button', { name: '创建并查看' }).click()
  await createRequestPromise

  expect(createBody).toEqual({
    name: '生产环境灰度',
    nodeIds: [101, 102, 103],
    policy: {
      waves: [{ percent: 5 }, { percent: 25 }, { percent: 100 }],
      maxConcurrentPerWave: 0,
      settleSeconds: 60,
      observeSeconds: 120,
      maxWaveSeconds: 1800,
      minWaveSuccessRatio: 1,
      minCumulativeSuccessRatio: 1,
      maxFailures: 0,
      autoRollback: false,
      onTimeout: 'pause',
      requiredFreshTelemetrySeconds: 300,
      exemptAlarmMetrics: [],
      requireInstalledVersion: true,
    },
  })

  const detail = page.getByRole('dialog', { name: '灰度发布详情' })
  await expect(detail).toBeVisible({ timeout: 10_000 })
  await expect(detail.getByText('运行中', { exact: true })).toBeVisible()
  await expect(detail.getByText('第 2 / 3 波', { exact: true })).toBeVisible()

  const wavePanel = detail.locator('.panel').nth(0)
  await expect(wavePanel.getByRole('row', { name: /第 1 波/ })).toContainText('已通过')
  await expect(wavePanel.getByRole('row', { name: /第 2 波/ })).toContainText('观察中')
  await expect(wavePanel.getByRole('row', { name: /第 3 波/ })).toContainText('待派发')

  const targetPanel = detail.locator('.panel').nth(1)
  const healthyTarget = targetPanel.getByRole('row', { name: /edge-sh-01/ })
  await expect(healthyTarget).toContainText('健康')
  await expect(healthyTarget).toContainText('2.0.0')
  await expect(healthyTarget).toContainText('EdgePulse OS 1.0.0')
  await expect(healthyTarget).toContainText('心跳与遥测覆盖观察窗口')
  const installingTarget = targetPanel.getByRole('row', { name: /edge-sh-02/ })
  await expect(installingTarget).toContainText('安装中')
  await expect(installingTarget).toContainText('1.0.0')
  await expect(installingTarget).toContainText('心跳尚未覆盖观察窗口')
  const pendingTarget = targetPanel.getByRole('row', { name: /edge-sh-03/ })
  await expect(pendingTarget).toContainText('待派发')
  await expect(pendingTarget).toContainText('等待上一波通过')

  for (const name of ['暂停', '提升', '强制提升', '中止', '回滚']) {
    await expect(detail.getByRole('button', { name, exact: true })).toBeVisible()
  }

  const pauseRequestPromise = page.waitForRequest(
    request => request.method() === 'POST' && new URL(request.url()).pathname === '/api/firmware/rollouts/31/pause',
  )
  await detail.getByRole('button', { name: '暂停', exact: true }).click()
  await pauseRequestPromise
  await expect(detail.getByText('已暂停', { exact: true })).toBeVisible()
  await expect(detail.getByRole('button', { name: '恢复', exact: true })).toBeVisible()
  await expect(detail.getByRole('button', { name: '暂停', exact: true })).toHaveCount(0)

  await detail.getByRole('button', { name: '强制提升', exact: true }).click()
  const forceDialog = page.getByRole('dialog', { name: '强制提升' })
  await expect(forceDialog).toBeVisible()
  await forceDialog.getByRole('button', { name: '确认提升' }).click()
  await expect(forceDialog.getByText('请填写强制提升原因')).toBeVisible()
  expect(forcePosts, '原因为空时不应发起强制提升请求').toBe(0)

  await forceDialog.getByPlaceholder('例如 现场运维确认新版本运行正常').fill('现场运维确认')
  const forceRequestPromise = page.waitForRequest(
    request => request.method() === 'POST' && new URL(request.url()).pathname === '/api/firmware/rollouts/31/force-promote',
  )
  await forceDialog.getByRole('button', { name: '确认提升' }).click()
  await forceRequestPromise

  expect(forceBody).toEqual({ reason: '现场运维确认' })
  await expect(page.getByRole('dialog', { name: '强制提升' })).toHaveCount(0)
  await expect(detail.getByText('已完成', { exact: true })).toBeVisible()
  await expect(detail.getByRole('button', { name: '强制提升', exact: true })).toHaveCount(0)
  await expect(detail.getByRole('button', { name: '中止', exact: true })).toHaveCount(0)
  await expect(detail.getByRole('button', { name: '回滚', exact: true })).toHaveCount(0)
})

test('firmware rollout rejects cleared ratios, shows create summary and rollback statuses', async ({ page }) => {
  await login(page)

  let createPosts = 0
  let refreshFails = false
  const rollbackState = makeRollbackRollout()

  await page.route('**/api/nodes**', async (route) => {
    if (route.request().method() !== 'GET') return route.continue()
    await fulfill(route, { rows: nodeRows, total: nodeRows.length, pageNum: 1, pageSize: 100 })
  })

  await page.route('**/api/firmware**', async (route) => {
    const path = new URL(route.request().url()).pathname
    const method = route.request().method()

    if (path === '/api/firmware' && method === 'GET') {
      await fulfill(route, { rows: firmwareRows, total: firmwareRows.length, pageNum: 1, pageSize: 10 })
      return
    }
    if (path === '/api/firmware/policy' && method === 'GET') {
      await fulfill(route, { requireSignature: false, trustedKeyIds: [] })
      return
    }
    if (path === '/api/firmware/5/rollouts' && method === 'POST') {
      createPosts += 1
      await fulfill(route, rollbackState)
      return
    }
    if (path === '/api/firmware/rollouts/31' && method === 'GET') {
      if (refreshFails) {
        await fulfill(route, null, 500, '注入的刷新失败')
        return
      }
      await fulfill(route, rollbackState)
      return
    }
    await route.continue()
  })

  await page.goto('/device/firmware')
  const firmwareRow = page.getByRole('row', { name: /EdgePulse OS/ }).filter({ hasText: '2.0.0' })
  await expect(firmwareRow).toBeVisible()
  await firmwareRow.getByRole('button', { name: '灰度发布' }).click()

  const wizard = page.getByRole('dialog', { name: '新建灰度发布' })
  await expect(wizard).toBeVisible()
  await wizard.getByPlaceholder('例如 生产环境灰度').fill('回滚演练')
  await wizard.locator('.node-picker-row').filter({ hasText: 'edge-sh-01' }).click()

  const summary = wizard.locator('.rollout-summary')
  await expect(summary).toContainText('EdgePulse OS 2.0.0')
  await expect(summary).toContainText('1 个 · edge-sh-01')
  await expect(summary).toContainText('5% → 25% → 100%')
  await expect(summary).toContainText('单波 100% / 累计 100%')
  await expect(summary).toContainText('单波最长 1800 秒 · 超时暂停 · 不自动回滚')

  const waveRatio = wizard.getByLabel('单波最低成功率（%）')
  const cumulativeRatio = wizard.getByLabel('累计最低成功率（%）')
  await waveRatio.fill('')
  await cumulativeRatio.fill('')
  await expect(summary).toContainText('单波 未填写 / 累计 未填写')

  await wizard.getByRole('button', { name: '创建并查看' }).click()
  await expect(wizard.getByText('单波最低成功率不能为空')).toBeVisible()
  expect(createPosts, '必填成功率为空时不应发起创建请求').toBe(0)

  await waveRatio.fill('100')
  await cumulativeRatio.fill('100')
  const createRequestPromise = page.waitForRequest(
    request => request.method() === 'POST' && new URL(request.url()).pathname === '/api/firmware/5/rollouts',
  )
  await wizard.getByRole('button', { name: '创建并查看' }).click()
  await createRequestPromise
  expect(createPosts).toBe(1)

  const detail = page.getByRole('dialog', { name: '灰度发布详情' })
  await expect(detail).toBeVisible({ timeout: 10_000 })
  await expect(detail.locator('.kv-grid').first()).toContainText('回滚中')
  await expect(detail.locator('.kv-grid').first()).toContainText('回滚失败：节点 102 回滚任务失败')

  const wavePanel = detail.locator('.panel').nth(0)
  await expect(wavePanel.getByRole('row', { name: /第 2 波/ })).toContainText('回滚已派发 1 台，1 台无法回滚')

  const targetPanel = detail.locator('.panel').nth(1)
  await expect(targetPanel.getByRole('row', { name: /edge-sh-02/ })).toContainText('回滚失败')
  await expect(targetPanel.getByRole('row', { name: /edge-sh-03/ })).toContainText('回滚已派发')

  await expect(detail.getByRole('button', { name: '暂停', exact: true })).toHaveCount(0)
  await expect(detail.getByRole('button', { name: '提升', exact: true })).toHaveCount(0)
  await expect(detail.getByRole('button', { name: '中止', exact: true })).toBeVisible()
  await expect(detail.getByRole('button', { name: '回滚', exact: true })).toBeVisible()

  refreshFails = true
  const refreshPromise = page.waitForResponse(
    response => new URL(response.url()).pathname === '/api/firmware/rollouts/31' && response.status() === 500,
  )
  await detail.getByRole('button', { name: '刷新', exact: true }).click()
  await refreshPromise
  await expect(detail.getByRole('alert').filter({ hasText: '注入的刷新失败' })).toBeVisible()
  await expect(detail.locator('.kv-grid').first()).toContainText('回滚中')
})
